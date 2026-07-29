// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/* SPDX-License-Identifier: MIT */
/* TwinClient (Phase 3.3).
 *
 * Supports both IoT Hub Classic (MQTT v3.1.1) and Hub-Next (MQTT v5).
 *
 * Classic:
 *   GET            : $iothub/twin/GET/?$rid=<n>
 *   PATCH reported : $iothub/twin/PATCH/properties/reported/?$rid=<n>
 *   Response       : $iothub/twin/res/<status>/?$rid=<n>[&$version=<v>]
 *   Desired        : $iothub/twin/PATCH/properties/desired/?$version=<v>
 *
 * Next (gateway/rfcs/aeg/twin.md):
 *   All device->service traffic on ih/{device_id}/srv/twin, all service->device
 *   traffic on ih/{device_id}/dev/twin, both QoS 0. The message kind rides the
 *   "type" user property and the body is protobuf. Request/response pairs are
 *   correlated by a per-attempt 16-byte UUID in Correlation Data; the
 *   backend-initiated twin-push and desired-patch instead carry the current
 *   connection's birth nonce so stale ones can be dropped.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "azure/iot/az_iot_twin_client.h"
#include "azure/iot/az_iot_mqtt_iface.h"

#include "internal/connection_client_internal.h"
#include "internal/protocol_profile.h"
#include "internal/proto3.h"
#include "internal/twin_client_internal.h"

#define AZ_IOT_TWIN_TOPIC_MAX   192

/* ---- Hub-Next wire constants (twin.md 2, 3) ----------------------------- */
#define TWIN_NEXT_TOPIC_DEV_FMT   "ih/%s/dev/twin"
#define TWIN_NEXT_TOPIC_SRV_FMT   "ih/%s/srv/twin"
#define TWIN_NEXT_CORR_LEN        16u
#define TWIN_CONTENT_TYPE         "application/protobuf"
#define TWIN_TYPE_KEY             "type"
#define TWIN_TYPE_GET             "get:1"
#define TWIN_TYPE_GET_RESPONSE    "get-response"
#define TWIN_TYPE_REPORTED_PATCH  "reported-patch:1"
/* Matched before the shorter "reported-patch" would be, so ordering matters at
 * the dispatch site rather than here. */
#define TWIN_TYPE_PATCH_RESPONSE  "reported-patch-response"
#define TWIN_TYPE_TWIN_PUSH       "twin-push"
#define TWIN_TYPE_DESIRED_PATCH   "desired-patch"

/* TwinGet.Sections enum: request both sections (twin.md 3.3). */
#define TWIN_SECTIONS_BOTH        3u

/* The pending slot kind values stored in _internal.pending[].kind */
#define TWIN_PENDING_NONE  0
#define TWIN_PENDING_GET   1
#define TWIN_PENDING_PATCH 2

/* Internal shorthand to access _internal fields */
#define TI(t) ((t)->_internal)

/* ------------------------------------------------------------------------- */
/* helpers                                                                   */
/* ------------------------------------------------------------------------- */

/* Find the pending slot matching a given request-id. Returns slot index or -1. */
static int find_pending_by_rid(az_iot_twin_client* t, uint32_t rid)
{
    for (int i = 0; i < AZ_IOT_TWIN_MAX_PENDING; ++i)
    {
        if (TI(t).pending[i].in_use && TI(t).pending[i].rid == rid)
        {
            return i;
        }
    }
    return -1;
}

/* Allocate an unused pending slot. Returns slot index or -1 if full. */
static int alloc_pending(az_iot_twin_client* t)
{
    for (int i = 0; i < AZ_IOT_TWIN_MAX_PENDING; ++i)
    {
        if (!TI(t).pending[i].in_use) return i;
    }
    return -1;
}

/* ------------------------------------------------------------------------- */
/* desired-property subscriber registry                                      */
/* ------------------------------------------------------------------------- */

/* Add cb/user_ctx to the given pool. Returns AZ_IOT_OK, AZ_IOT_ERR_BUSY (during
 * dispatch), AZ_IOT_ERR_INVALID_ARG, or AZ_IOT_ERR_NOT_SUPPORTED (pool full).
 * Idempotent: re-subscribing the same (cb,user_ctx) is a no-op success. */
static az_iot_result desired_subscribe(
    az_iot_twin_client* t,
    az_iot_twin_desired_sub* pool,
    size_t pool_cap,
    az_iot_twin_desired_callback cb,
    void* user_ctx)
{
    if (!t || !cb) return AZ_IOT_ERR_INVALID_ARG;
    if (TI(t).dispatching) return AZ_IOT_ERR_BUSY;

    int free_slot = -1;
    for (size_t i = 0; i < pool_cap; ++i)
    {
        if (pool[i].in_use)
        {
            if (pool[i].cb == cb && pool[i].user_ctx == user_ctx) return AZ_IOT_OK;
        }
        else if (free_slot < 0)
        {
            free_slot = (int)i;
        }
    }
    if (free_slot < 0) return AZ_IOT_ERR_NOT_SUPPORTED;

    pool[free_slot].cb       = cb;
    pool[free_slot].user_ctx = user_ctx;
    pool[free_slot].in_use   = true;
    return AZ_IOT_OK;
}

/* Dispatch a desired patch to all subscribers: feature-client pool first (in
 * registration order), then application pool. The dispatching guard forbids
 * subscribe/unsubscribe from within a callback. */
static void dispatch_desired(
    az_iot_twin_client* t,
    const uint8_t* payload,
    size_t payload_len,
    uint64_t version)
{
    TI(t).dispatching = true;
    for (size_t i = 0; i < AZ_IOT_TWIN_MAX_DESIRED_FEATURE_SUBS; ++i)
    {
        az_iot_twin_desired_sub* s = &TI(t).desired_feature_subs[i];
        if (s->in_use && s->cb) s->cb(payload, payload_len, version, s->user_ctx);
    }
    for (size_t i = 0; i < AZ_IOT_TWIN_MAX_DESIRED_APP_SUBS; ++i)
    {
        az_iot_twin_desired_sub* s = &TI(t).desired_app_subs[i];
        if (s->in_use && s->cb) s->cb(payload, payload_len, version, s->user_ctx);
    }
    TI(t).dispatching = false;
}

/* Find "<key>=<value>" inside a query string (starting at '?'); writes value
 * into out (NUL-terminated). Returns true if found. */
static bool query_value(const char* qs, const char* key,
                        char* out, size_t cap)
{
    if (!qs || !key) return false;
    size_t key_len = strlen(key);
    const char* p = qs;
    while (p && *p)
    {
        /* skip past '?' or '&' */
        if (*p == '?' || *p == '&') p++;
        if (strncmp(p, key, key_len) == 0 && p[key_len] == '=')
        {
            const char* val = p + key_len + 1;
            const char* end = strchr(val, '&');
            size_t n = end ? (size_t)(end - val) : strlen(val);
            if (n + 1 > cap) return false;
            memcpy(out, val, n);
            out[n] = '\0';
            return true;
        }
        p = strchr(p, '&');
    }
    return false;
}

/* Map an HTTP-style status (200, 204, 404, 429, 5xx) to an az_iot result code. */
static az_iot_result status_to_result(int status)
{
    if (status >= 200 && status < 300) return AZ_IOT_OK;
    if (status == 404) return AZ_IOT_ERR_INVALID_ARG;
    if (status == 429) return AZ_IOT_ERR_NOT_SUPPORTED;
    return AZ_IOT_ERR_MQTT;
}

/* ------------------------------------------------------------------------- */
/* dispatch handlers — Classic                                               */
/* ------------------------------------------------------------------------- */

static void on_twin_response(void* user_ctx, const az_iot_mqtt_message* msg)
{
    az_iot_twin_client* t = (az_iot_twin_client*)user_ctx;
    if (!t || !msg || !msg->topic) return;

    /* Topic: "$iothub/twin/res/<status>/?$rid=<n>[&$version=<v>]". */
    static const char k_prefix[] = "$iothub/twin/res/";
    size_t prefix_len = sizeof(k_prefix) - 1;
    if (strncmp(msg->topic, k_prefix, prefix_len) != 0) return;
    const char* status_str = msg->topic + prefix_len;
    char* qmark = strchr(status_str, '?');
    if (!qmark) return;
    /* Parse status code (decimal) up to '/' or '?'. */
    int status = 0;
    for (const char* p = status_str; p < qmark && *p && *p != '/'; ++p)
    {
        if (*p < '0' || *p > '9') return;
        status = status * 10 + (*p - '0');
    }
    char rid_buf[16];
    if (!query_value(qmark, "$rid", rid_buf, sizeof(rid_buf))) return;
    uint32_t rid = (uint32_t)strtoul(rid_buf, NULL, 10);

    int idx = find_pending_by_rid(t, rid);
    if (idx < 0) return; /* stale or unknown rid */

    az_iot_result r = status_to_result(status);
    if (TI(t).pending[idx].kind == TWIN_PENDING_GET)
    {
        az_iot_twin_get_callback cb = TI(t).pending[idx].cb.get_cb;
        void* ctx = TI(t).pending[idx].user_ctx;
        TI(t).pending[idx].in_use = false;
        TI(t).pending[idx].kind = TWIN_PENDING_NONE;
        /* Classic returns one JSON document holding both sections and no
         * per-section version, so only `document` is populated. */
        az_iot_twin_state twin;
        memset(&twin, 0, sizeof(twin));
        twin.document = msg->payload;
        twin.document_len = msg->payload_len;
        if (cb) cb(r, (r == AZ_IOT_OK) ? &twin : NULL, ctx);
    }
    else if (TI(t).pending[idx].kind == TWIN_PENDING_PATCH)
    {
        az_iot_twin_patch_ack_callback cb = TI(t).pending[idx].cb.patch_cb;
        void* ctx = TI(t).pending[idx].user_ctx;
        TI(t).pending[idx].in_use = false;
        TI(t).pending[idx].kind = TWIN_PENDING_NONE;
        /* Classic acknowledges with an HTTP-like status and carries neither a
         * result enum nor a version. */
        az_iot_twin_patch_result result;
        memset(&result, 0, sizeof(result));
        result.status = AZ_IOT_TWIN_PATCH_OK;
        if (cb) cb(r, (r == AZ_IOT_OK) ? &result : NULL, ctx);
    }
    else
    {
        TI(t).pending[idx].in_use = false;
    }
}

static void on_twin_desired(void* user_ctx, const az_iot_mqtt_message* msg)
{
    az_iot_twin_client* t = (az_iot_twin_client*)user_ctx;
    if (!t || !msg || !msg->topic) return;

    /* Topic: "$iothub/twin/PATCH/properties/desired/?$version=<v>". */
    uint64_t version = 0;
    const char* qmark = strchr(msg->topic, '?');
    if (qmark)
    {
        char ver_buf[24];
        if (query_value(qmark, "$version", ver_buf, sizeof(ver_buf)))
        {
            version = strtoull(ver_buf, NULL, 10);
        }
    }
    dispatch_desired(t, msg->payload, msg->payload_len, version);
}

/* Re-seed the tracked twin versions from the birth-ack when the client is on a
 * new connection. The versions the service hands back at birth admission are
 * authoritative as of that moment; anything the SDK tracked on a previous
 * connection is stale. Detected by comparing the connection's birth nonce with
 * the one the current versions were seeded from. No-op on Classic, which has no
 * presence handshake and no version negotiation. */
static void twin_next_sync_versions(az_iot_twin_client* t)
{
    uint8_t nonce[TWIN_NEXT_CORR_LEN];
    if (az_iot_connection_client__presence_nonce(TI(t).conn, nonce) != AZ_IOT_OK) return;

    if (TI(t).nonce_valid && memcmp(TI(t).nonce, nonce, TWIN_NEXT_CORR_LEN) == 0) return;

    uint64_t desired = 0, reported = 0;
    if (az_iot_connection_client__presence_twin_versions(TI(t).conn, &desired, &reported)
        == AZ_IOT_OK)
    {
        TI(t).desired_version = desired;
        TI(t).reported_version = reported;
    }
    memcpy(TI(t).nonce, nonce, TWIN_NEXT_CORR_LEN);
    TI(t).nonce_valid = true;
}

/* ------------------------------------------------------------------------- */
/* dispatch handlers — Hub-Next                                              */
/* ------------------------------------------------------------------------- */

/* True when the message's "type" user property is `want`, matching either the
 * bare name or the "<name>:<schemaVersion>" form the service stamps. */
static bool next_type_is(const az_iot_mqtt_message* msg, const char* want)
{
    for (size_t i = 0; i < msg->user_properties_count; ++i)
    {
        const az_iot_mqtt_user_property* up = &msg->user_properties[i];
        if (!up->key || strcmp(up->key, TWIN_TYPE_KEY) != 0) continue;
        if (!up->value) return false;
        size_t n = strlen(want);
        return strncmp(up->value, want, n) == 0
               && (up->value[n] == '\0' || up->value[n] == ':');
    }
    return false;
}

/* Find the pending slot whose 16-byte correlation id matches the message's
 * Correlation Data. Returns the slot index or -1. */
static int find_pending_by_corr(az_iot_twin_client* t, const az_iot_mqtt_message* msg)
{
    if (!msg->correlation_data || msg->correlation_data_len != TWIN_NEXT_CORR_LEN) return -1;

    for (int i = 0; i < AZ_IOT_TWIN_MAX_PENDING; ++i)
    {
        if (TI(t).pending[i].in_use
            && memcmp(TI(t).pending[i].corr, msg->correlation_data, TWIN_NEXT_CORR_LEN) == 0)
        {
            return i;
        }
    }
    return -1;
}

/* True when a backend-initiated dev-bound message belongs to the connection the
 * client is currently on. twin-push and desired-patch carry the birth nonce as
 * Correlation Data precisely so traffic left over from a defunct connection can
 * be dropped (twin.md 2.1). */
static bool next_matches_connection(az_iot_twin_client* t, const az_iot_mqtt_message* msg)
{
    uint8_t nonce[TWIN_NEXT_CORR_LEN];
    if (az_iot_connection_client__presence_nonce(TI(t).conn, nonce) != AZ_IOT_OK) return false;

    return msg->correlation_data != NULL
           && msg->correlation_data_len == TWIN_NEXT_CORR_LEN
           && memcmp(msg->correlation_data, nonce, TWIN_NEXT_CORR_LEN) == 0;
}

/* Decode a twin.md Section: { 1 uint64 version, 2 bytes payload }. */
static void next_decode_section(const uint8_t* buf, size_t len, az_iot_twin_section* out)
{
    size_t pos = 0;
    while (pos < len)
    {
        uint32_t field = 0;
        uint8_t wire = 0;
        if (!az_iot_proto3_read_tag(buf, len, &pos, &field, &wire)) return;

        if (field == 1u && wire == AZ_IOT_PROTO3_WIRE_VARINT)
        {
            if (!az_iot_proto3_read_varint(buf, len, &pos, &out->version)) return;
        }
        else if (field == 2u && wire == AZ_IOT_PROTO3_WIRE_LEN)
        {
            if (!az_iot_proto3_read_bytes(buf, len, &pos, &out->payload, &out->payload_len)) return;
        }
        else if (!az_iot_proto3_skip_field(buf, len, &pos, wire))
        {
            return;
        }
    }
}

/* twin-push:1 — TwinPush { 1 Section desired, 2 Section reported }. Either
 * section may be absent; the service only pushes what the device is behind on
 * and asked for. */
static void on_twin_push_next(az_iot_twin_client* t, const az_iot_mqtt_message* msg)
{
    az_iot_twin_state twin;
    memset(&twin, 0, sizeof(twin));

    const uint8_t* buf = msg->payload;
    size_t len = msg->payload_len;
    size_t pos = 0;
    while (buf && pos < len)
    {
        uint32_t field = 0;
        uint8_t wire = 0;
        if (!az_iot_proto3_read_tag(buf, len, &pos, &field, &wire)) break;

        if (wire == AZ_IOT_PROTO3_WIRE_LEN && (field == 1u || field == 2u))
        {
            const uint8_t* sec = NULL;
            size_t sec_len = 0;
            if (!az_iot_proto3_read_bytes(buf, len, &pos, &sec, &sec_len)) break;
            next_decode_section(sec, sec_len, field == 1u ? &twin.desired : &twin.reported);
        }
        else if (!az_iot_proto3_skip_field(buf, len, &pos, wire))
        {
            break;
        }
    }

    /* A push carries the authoritative state, so it also advances the versions
     * the SDK reports back to the service. */
    if (twin.desired.version) TI(t).desired_version = twin.desired.version;
    if (twin.reported.version) TI(t).reported_version = twin.reported.version;

    if (TI(t).push_cb) TI(t).push_cb(&twin, TI(t).push_user_ctx);
}

/* desired-patch:1 — DesiredPatch { 1 uint64 version, 2 optional bytes payload }.
 * An absent payload is a version probe (twin.md 3.5, 7.1): it advances nothing
 * for the application, so it updates the tracked version and is not dispatched
 * to desired subscribers. */
static void on_desired_patch_next(az_iot_twin_client* t, const az_iot_mqtt_message* msg)
{
    uint64_t version = 0;
    const uint8_t* patch = NULL;
    size_t patch_len = 0;
    bool has_payload = false;

    const uint8_t* buf = msg->payload;
    size_t len = msg->payload_len;
    size_t pos = 0;
    while (buf && pos < len)
    {
        uint32_t field = 0;
        uint8_t wire = 0;
        if (!az_iot_proto3_read_tag(buf, len, &pos, &field, &wire)) break;

        if (field == 1u && wire == AZ_IOT_PROTO3_WIRE_VARINT)
        {
            if (!az_iot_proto3_read_varint(buf, len, &pos, &version)) break;
        }
        else if (field == 2u && wire == AZ_IOT_PROTO3_WIRE_LEN)
        {
            if (!az_iot_proto3_read_bytes(buf, len, &pos, &patch, &patch_len)) break;
            has_payload = true;
        }
        else if (!az_iot_proto3_skip_field(buf, len, &pos, wire))
        {
            break;
        }
    }

    if (version) TI(t).desired_version = version;
    if (has_payload) dispatch_desired(t, patch, patch_len, version);
}

/* get-response:1 — TwinGetResponse { 1 desired_version, 2 reported_version,
 * 3 optional desired_payload, 4 optional reported_payload }. Both versions are
 * always present; a payload is omitted when the section was not requested or
 * the device's if_not_match already matched. */
static void on_get_response_next(az_iot_twin_client* t, int idx,
                                 const az_iot_mqtt_message* msg)
{
    az_iot_twin_state twin;
    memset(&twin, 0, sizeof(twin));

    const uint8_t* buf = msg->payload;
    size_t len = msg->payload_len;
    size_t pos = 0;
    while (buf && pos < len)
    {
        uint32_t field = 0;
        uint8_t wire = 0;
        if (!az_iot_proto3_read_tag(buf, len, &pos, &field, &wire)) break;

        if (field == 1u && wire == AZ_IOT_PROTO3_WIRE_VARINT)
        {
            if (!az_iot_proto3_read_varint(buf, len, &pos, &twin.desired.version)) break;
        }
        else if (field == 2u && wire == AZ_IOT_PROTO3_WIRE_VARINT)
        {
            if (!az_iot_proto3_read_varint(buf, len, &pos, &twin.reported.version)) break;
        }
        else if (field == 3u && wire == AZ_IOT_PROTO3_WIRE_LEN)
        {
            if (!az_iot_proto3_read_bytes(buf, len, &pos,
                                          &twin.desired.payload, &twin.desired.payload_len)) break;
        }
        else if (field == 4u && wire == AZ_IOT_PROTO3_WIRE_LEN)
        {
            if (!az_iot_proto3_read_bytes(buf, len, &pos,
                                          &twin.reported.payload, &twin.reported.payload_len)) break;
        }
        else if (!az_iot_proto3_skip_field(buf, len, &pos, wire))
        {
            break;
        }
    }

    if (twin.desired.version) TI(t).desired_version = twin.desired.version;
    if (twin.reported.version) TI(t).reported_version = twin.reported.version;

    az_iot_twin_get_callback cb = TI(t).pending[idx].cb.get_cb;
    void* ctx = TI(t).pending[idx].user_ctx;
    TI(t).pending[idx].in_use = false;
    TI(t).pending[idx].kind = TWIN_PENDING_NONE;
    if (cb) cb(AZ_IOT_OK, &twin, ctx);
}

/* reported-patch-response:1 — ReportedPatchResponse { 1 Result result,
 * 2 uint64 version }. On OK `version` is the new authoritative version; on
 * VERSION_MISMATCH it is the current one, which the SDK adopts so the
 * application's retry carries a correct if_match. */
static void on_patch_response_next(az_iot_twin_client* t, int idx,
                                   const az_iot_mqtt_message* msg)
{
    az_iot_twin_patch_result result;
    memset(&result, 0, sizeof(result));

    const uint8_t* buf = msg->payload;
    size_t len = msg->payload_len;
    size_t pos = 0;
    while (buf && pos < len)
    {
        uint32_t field = 0;
        uint8_t wire = 0;
        if (!az_iot_proto3_read_tag(buf, len, &pos, &field, &wire)) break;

        if (field == 1u && wire == AZ_IOT_PROTO3_WIRE_VARINT)
        {
            uint64_t v = 0;
            if (!az_iot_proto3_read_varint(buf, len, &pos, &v)) break;
            result.status = (az_iot_twin_patch_status)v;
        }
        else if (field == 2u && wire == AZ_IOT_PROTO3_WIRE_VARINT)
        {
            if (!az_iot_proto3_read_varint(buf, len, &pos, &result.version)) break;
        }
        else if (!az_iot_proto3_skip_field(buf, len, &pos, wire))
        {
            break;
        }
    }

    if (result.version) TI(t).reported_version = result.version;

    az_iot_twin_patch_ack_callback cb = TI(t).pending[idx].cb.patch_cb;
    void* ctx = TI(t).pending[idx].user_ctx;
    TI(t).pending[idx].in_use = false;
    TI(t).pending[idx].kind = TWIN_PENDING_NONE;
    if (cb) cb(AZ_IOT_OK, &result, ctx);
}

/* Single inbound handler for "ih/{device_id}/dev/twin". Everything the service
 * sends for twin arrives here and is dispatched on the "type" user property
 * (twin.md 2). */
static void on_twin_next(void* user_ctx, const az_iot_mqtt_message* msg)
{
    az_iot_twin_client* t = (az_iot_twin_client*)user_ctx;
    if (!t || !msg) return;

    if (next_type_is(msg, TWIN_TYPE_TWIN_PUSH))
    {
        if (next_matches_connection(t, msg)) on_twin_push_next(t, msg);
        return;
    }
    if (next_type_is(msg, TWIN_TYPE_DESIRED_PATCH))
    {
        if (next_matches_connection(t, msg)) on_desired_patch_next(t, msg);
        return;
    }

    /* The remaining types answer a request the device made; correlate them to
     * the pending slot that issued it. */
    int idx = find_pending_by_corr(t, msg);
    if (idx < 0) return;

    if (next_type_is(msg, TWIN_TYPE_GET_RESPONSE))
    {
        if (TI(t).pending[idx].kind == TWIN_PENDING_GET) on_get_response_next(t, idx, msg);
    }
    else if (next_type_is(msg, TWIN_TYPE_PATCH_RESPONSE))
    {
        if (TI(t).pending[idx].kind == TWIN_PENDING_PATCH) on_patch_response_next(t, idx, msg);
    }
}

/* ------------------------------------------------------------------------- */
/* public API                                                                 */
/* ------------------------------------------------------------------------- */

az_iot_result az_iot_twin_client_init(
    az_iot_twin_client* client,
    az_iot_connection_client* conn)
{
    if (client == NULL || conn == NULL)
    {
        return AZ_IOT_ERR_INVALID_ARG;
    }
    const az_iot_protocol_profile* profile =
        az_iot_connection_client__profile(conn);
    if (!profile)
    {
        return AZ_IOT_ERR_NOT_SUPPORTED;
    }

    memset(client, 0, sizeof(*client));
    TI(client).conn = conn;
    TI(client).next_rid = 1;

    if (profile->flavor == AZ_IOT_HUB_FLAVOR_NEXT)
    {
        /* Hub-Next: one subscription covers everything the service sends for
         * twin; the message kind is carried in the "type" user property.
         *
         * The connection client already subscribes to ih/{device_id}/dev/# for
         * the presence handshake, so this filter is redundant on the wire, but
         * registering it keeps the twin client self-contained: it does not
         * depend on another feature's subscription staying in place. */
        const char* device_id = az_iot_connection_client__device_id(conn);
        if (!device_id)
        {
            memset(client, 0, sizeof(*client));
            return AZ_IOT_ERR_NOT_INITIALIZED;
        }

        char prefix[AZ_IOT_TWIN_TOPIC_MAX];
        int n = snprintf(prefix, sizeof(prefix), TWIN_NEXT_TOPIC_DEV_FMT, device_id);
        if (n < 0 || (size_t)n >= sizeof(prefix))
        {
            memset(client, 0, sizeof(*client));
            return AZ_IOT_ERR_INTERNAL;
        }

        az_iot_result r = az_iot_connection_client__register_inbound_handler(
            conn, prefix, on_twin_next, client);
        if (r != AZ_IOT_OK) { memset(client, 0, sizeof(*client)); return r; }

        /* QoS 1 is the cap requested; twin traffic is published at QoS 0 and is
         * delivered at QoS 0 regardless (MQTT 5 3.8.3.1). */
        r = az_iot_connection_client__add_subscription_on_connect(
            conn, prefix, AZ_IOT_MQTT_QOS_1);
        if (r != AZ_IOT_OK)
        {
            (void)az_iot_connection_client__unregister_inbound_handlers(conn, client);
            memset(client, 0, sizeof(*client));
            return r;
        }
    }
    else
    {
        /* Classic path */
        if (!profile->twin_response_topic_prefix ||
            !profile->twin_desired_topic_prefix)
        {
            memset(client, 0, sizeof(*client));
            return AZ_IOT_ERR_NOT_SUPPORTED;
        }

        az_iot_result r = az_iot_connection_client__register_inbound_handler(
            conn, profile->twin_response_topic_prefix, on_twin_response, client);
        if (r != AZ_IOT_OK) { memset(client, 0, sizeof(*client)); return r; }

        r = az_iot_connection_client__register_inbound_handler(
            conn, profile->twin_desired_topic_prefix, on_twin_desired, client);
        if (r != AZ_IOT_OK)
        {
            (void)az_iot_connection_client__unregister_inbound_handlers(conn, client);
            memset(client, 0, sizeof(*client));
            return r;
        }

        /* Persistent subscriptions: response + desired wildcards. */
        char filter[AZ_IOT_TWIN_TOPIC_MAX];
        int n = snprintf(filter, sizeof(filter), "%s#", profile->twin_response_topic_prefix);
        if (n < 0 || (size_t)n >= sizeof(filter))
        {
            (void)az_iot_connection_client__unregister_inbound_handlers(conn, client);
            memset(client, 0, sizeof(*client));
            return AZ_IOT_ERR_INTERNAL;
        }
        r = az_iot_connection_client__add_subscription_on_connect(
            conn, filter, AZ_IOT_MQTT_QOS_0);
        if (r != AZ_IOT_OK)
        {
            (void)az_iot_connection_client__unregister_inbound_handlers(conn, client);
            memset(client, 0, sizeof(*client));
            return r;
        }

        n = snprintf(filter, sizeof(filter), "%s#", profile->twin_desired_topic_prefix);
        if (n < 0 || (size_t)n >= sizeof(filter))
        {
            (void)az_iot_connection_client__unregister_inbound_handlers(conn, client);
            memset(client, 0, sizeof(*client));
            return AZ_IOT_ERR_INTERNAL;
        }
        r = az_iot_connection_client__add_subscription_on_connect(
            conn, filter, AZ_IOT_MQTT_QOS_0);
        if (r != AZ_IOT_OK)
        {
            (void)az_iot_connection_client__unregister_inbound_handlers(conn, client);
            memset(client, 0, sizeof(*client));
            return r;
        }
    }

    return AZ_IOT_OK;
}

void az_iot_twin_client_destroy(az_iot_twin_client* client)
{
    if (!client) return;
    (void)az_iot_connection_client__unregister_inbound_handlers(TI(client).conn, client);
    memset(client, 0, sizeof(*client));
}

az_iot_result az_iot_twin_client_get(
    az_iot_twin_client* twin, az_iot_twin_get_callback cb, void* user_ctx)
{
    if (!twin) return AZ_IOT_ERR_INVALID_ARG;

    int idx = alloc_pending(twin);
    if (idx < 0) return AZ_IOT_ERR_NOT_SUPPORTED;

    uint32_t rid = TI(twin).next_rid++;
    if (TI(twin).next_rid == 0) TI(twin).next_rid = 1; /* never reuse 0 */

    const az_iot_protocol_profile* profile =
        az_iot_connection_client__profile(TI(twin).conn);
    bool is_next = profile && profile->flavor == AZ_IOT_HUB_FLAVOR_NEXT;

    char topic[AZ_IOT_TWIN_TOPIC_MAX];
    int n;

    /* Classic correlates by rid in the topic query string. */
    char corr_buf[16];
    int corr_len = snprintf(corr_buf, sizeof(corr_buf), "%u", (unsigned)rid);
    (void)corr_len;

    /* Hub-Next: TwinGet { 1 Sections sections }. Both sections, no
     * if_not_match filter -- the caller asked for the whole twin. */
    uint8_t body[16];
    size_t body_len = 0;
    az_iot_mqtt_user_property type_prop = { TWIN_TYPE_KEY, TWIN_TYPE_GET };

    if (is_next)
    {
        const char* device_id = az_iot_connection_client__device_id(TI(twin).conn);
        n = snprintf(topic, sizeof(topic), TWIN_NEXT_TOPIC_SRV_FMT, device_id);

        if (!az_iot_proto3_write_varint_field(body, sizeof(body), &body_len,
                                              1u, TWIN_SECTIONS_BOTH))
        {
            return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
        }
        az_iot_connection_client__gen_uuid(TI(twin).conn, TI(twin).pending[idx].corr);
    }
    else
    {
        /* Classic: "$iothub/twin/GET/?$rid=<n>" */
        n = snprintf(topic, sizeof(topic), "$iothub/twin/GET/?$rid=%u", (unsigned)rid);
    }
    if (n < 0 || (size_t)n >= sizeof(topic)) return AZ_IOT_ERR_NOT_SUPPORTED;

    /* Reserve the slot before publish. */
    TI(twin).pending[idx].in_use    = true;
    TI(twin).pending[idx].rid       = rid;
    TI(twin).pending[idx].kind      = TWIN_PENDING_GET;
    TI(twin).pending[idx].cb.get_cb = cb;
    TI(twin).pending[idx].user_ctx  = user_ctx;

    az_iot_mqtt_message out = { 0 };
    out.topic = topic;
    out.qos   = AZ_IOT_MQTT_QOS_0;

    if (is_next)
    {
        out.payload = body;
        out.payload_len = body_len;
        out.content_type = TWIN_CONTENT_TYPE;
        out.user_properties = &type_prop;
        out.user_properties_count = 1;
        out.correlation_data = TI(twin).pending[idx].corr;
        out.correlation_data_len = TWIN_NEXT_CORR_LEN;
    }

    az_iot_result r = az_iot_connection_client__publish(
        TI(twin).conn, &out, NULL, NULL);
    if (r != AZ_IOT_OK)
    {
        TI(twin).pending[idx].in_use = false;
        TI(twin).pending[idx].kind   = TWIN_PENDING_NONE;
    }
    return r;
}

az_iot_result az_iot_twin_client_patch_reported(
    az_iot_twin_client* twin,
    const uint8_t* patch,
    size_t patch_len,
    az_iot_twin_patch_ack_callback cb,
    void* user_ctx)
{
    if (!twin) return AZ_IOT_ERR_INVALID_ARG;
    if (patch_len > 0 && patch == NULL) return AZ_IOT_ERR_INVALID_ARG;

    int idx = alloc_pending(twin);
    if (idx < 0) return AZ_IOT_ERR_NOT_SUPPORTED;

    uint32_t rid = TI(twin).next_rid++;
    if (TI(twin).next_rid == 0) TI(twin).next_rid = 1;

    const az_iot_protocol_profile* profile =
        az_iot_connection_client__profile(TI(twin).conn);
    bool is_next = profile && profile->flavor == AZ_IOT_HUB_FLAVOR_NEXT;

    char topic[AZ_IOT_TWIN_TOPIC_MAX];
    int n;

    const uint8_t* body = patch;
    size_t body_len = patch_len;
    az_iot_mqtt_user_property type_prop = { TWIN_TYPE_KEY, TWIN_TYPE_REPORTED_PATCH };

    if (is_next)
    {
        const char* device_id = az_iot_connection_client__device_id(TI(twin).conn);
        n = snprintf(topic, sizeof(topic), TWIN_NEXT_TOPIC_SRV_FMT, device_id);

        /* ReportedPatch { 1 uint64 if_match, 2 bytes payload }. if_match is the
         * device's view of the authoritative reported version; the service
         * rejects the write with VERSION_MISMATCH if it has moved on. */
        twin_next_sync_versions(twin);

        if (!TI(twin).encode_buffer) return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
        size_t pos = 0;
        if ((TI(twin).reported_version
             && !az_iot_proto3_write_varint_field(TI(twin).encode_buffer,
                                                  TI(twin).encode_buffer_len, &pos,
                                                  1u, TI(twin).reported_version))
            || !az_iot_proto3_write_bytes_field(TI(twin).encode_buffer,
                                                TI(twin).encode_buffer_len, &pos,
                                                2u, patch, patch_len))
        {
            return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
        }
        body = TI(twin).encode_buffer;
        body_len = pos;

        az_iot_connection_client__gen_uuid(TI(twin).conn, TI(twin).pending[idx].corr);
    }
    else
    {
        n = snprintf(topic, sizeof(topic),
                     "$iothub/twin/PATCH/properties/reported/?$rid=%u", (unsigned)rid);
    }
    if (n < 0 || (size_t)n >= sizeof(topic)) return AZ_IOT_ERR_NOT_SUPPORTED;

    TI(twin).pending[idx].in_use      = true;
    TI(twin).pending[idx].rid         = rid;
    TI(twin).pending[idx].kind        = TWIN_PENDING_PATCH;
    TI(twin).pending[idx].cb.patch_cb = cb;
    TI(twin).pending[idx].user_ctx    = user_ctx;

    az_iot_mqtt_message out = { 0 };
    out.topic       = topic;
    out.payload     = body;
    out.payload_len = body_len;
    out.qos         = AZ_IOT_MQTT_QOS_0;

    if (is_next)
    {
        out.content_type = TWIN_CONTENT_TYPE;
        out.user_properties = &type_prop;
        out.user_properties_count = 1;
        out.correlation_data = TI(twin).pending[idx].corr;
        out.correlation_data_len = TWIN_NEXT_CORR_LEN;
    }

    az_iot_result r = az_iot_connection_client__publish(
        TI(twin).conn, &out, NULL, NULL);
    if (r != AZ_IOT_OK)
    {
        TI(twin).pending[idx].in_use = false;
        TI(twin).pending[idx].kind   = TWIN_PENDING_NONE;
    }
    return r;
}

az_iot_result az_iot_twin_client_subscribe_desired(
    az_iot_twin_client* twin,
    az_iot_twin_desired_callback cb,
    void* user_ctx)
{
    if (!twin) return AZ_IOT_ERR_INVALID_ARG;
    return desired_subscribe(
        twin, TI(twin).desired_app_subs, AZ_IOT_TWIN_MAX_DESIRED_APP_SUBS, cb, user_ctx);
}

az_iot_result az_iot_twin_client__subscribe_desired(
    az_iot_twin_client* twin,
    az_iot_twin_desired_callback cb,
    void* user_ctx)
{
    if (!twin) return AZ_IOT_ERR_INVALID_ARG;
    return desired_subscribe(
        twin, TI(twin).desired_feature_subs, AZ_IOT_TWIN_MAX_DESIRED_FEATURE_SUBS, cb, user_ctx);
}

az_iot_result az_iot_twin_client_unsubscribe_desired(
    az_iot_twin_client* twin,
    az_iot_twin_desired_callback cb,
    void* user_ctx)
{
    if (!twin || !cb) return AZ_IOT_ERR_INVALID_ARG;
    if (TI(twin).dispatching) return AZ_IOT_ERR_BUSY;

    /* Search both pools; an entry matches on (cb, user_ctx). */
    for (size_t i = 0; i < AZ_IOT_TWIN_MAX_DESIRED_FEATURE_SUBS; ++i)
    {
        az_iot_twin_desired_sub* s = &TI(twin).desired_feature_subs[i];
        if (s->in_use && s->cb == cb && s->user_ctx == user_ctx)
        {
            s->in_use = false; s->cb = NULL; s->user_ctx = NULL;
            return AZ_IOT_OK;
        }
    }
    for (size_t i = 0; i < AZ_IOT_TWIN_MAX_DESIRED_APP_SUBS; ++i)
    {
        az_iot_twin_desired_sub* s = &TI(twin).desired_app_subs[i];
        if (s->in_use && s->cb == cb && s->user_ctx == user_ctx)
        {
            s->in_use = false; s->cb = NULL; s->user_ctx = NULL;
            return AZ_IOT_OK;
        }
    }
    return AZ_IOT_ERR_INVALID_ARG;
}

az_iot_result az_iot_twin_client_set_push_callback(
    az_iot_twin_client* twin,
    az_iot_twin_push_callback cb,
    void* user_ctx)
{
    if (!twin) return AZ_IOT_ERR_INVALID_ARG;
    TI(twin).push_cb = cb;
    TI(twin).push_user_ctx = user_ctx;
    return AZ_IOT_OK;
}

az_iot_result az_iot_twin_client_set_encode_buffer(
    az_iot_twin_client* twin,
    uint8_t* buffer,
    size_t buffer_len)
{
    if (!twin) return AZ_IOT_ERR_INVALID_ARG;
    if (buffer && buffer_len <= AZ_IOT_TWIN_ENCODE_OVERHEAD) return AZ_IOT_ERR_NOT_ENOUGH_SPACE;

    TI(twin).encode_buffer = buffer;
    TI(twin).encode_buffer_len = buffer ? buffer_len : 0;
    return AZ_IOT_OK;
}
