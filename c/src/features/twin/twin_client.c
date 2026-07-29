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
#include "internal/reconnect.h"
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

/* Defensive timeout schedule, shared with the presence handshake
 * (presence.md 5.6, twin.md 6): base grows 5, 6, 8 then 10 minutes, each with a
 * uniform [0, 5 min] jitter. The timeouts are deliberately long. MQTT
 * keep-alive is what proves the connection is healthy, so an exchange going
 * unanswered on a healthy connection means degeneration somewhere the device
 * cannot influence -- a slow backend, egress lag, an in-broker dispatch stall.
 * Retrying quickly would only add load to something already struggling, and
 * without jitter every affected device would retry in lockstep. */
static uint64_t twin_defensive_deadline(az_iot_twin_client* t, uint32_t attempt)
{
    static const uint64_t k_base_ms[] = { 300000u, 360000u, 480000u, 600000u };
    uint64_t base = k_base_ms[attempt < 4u ? attempt : 3u];

    /* Draw the jitter from the connection's PRNG. */
    uint8_t r[16];
    az_iot_connection_client__gen_uuid(TI(t).conn, r);
    uint64_t jitter = ((uint64_t)(((uint32_t)r[0] << 8) | r[1]) * 300000u) / 65535u;

    return az_iot_time_mono_ms() + base + jitter;
}

/* Drop everything bound to the lifetime of a connection (twin.md 6, 8.5).
 * Outstanding exchanges cannot survive a disconnect: twin runs at QoS 0, so
 * there is no transport-level resumption, and a reported patch must not be
 * retried with an if_match from the old connection -- the next birth-ack
 * delivers the authoritative version to use instead. Pending application
 * callbacks are completed with a transient error so no caller is left waiting. */
static void twin_next_abandon_exchanges(az_iot_twin_client* t)
{
    for (int i = 0; i < AZ_IOT_TWIN_MAX_PENDING; ++i)
    {
        if (!TI(t).pending[i].in_use) continue;

        int kind = TI(t).pending[i].kind;
        bool internal = TI(t).pending[i].internal;
        void* ctx = TI(t).pending[i].user_ctx;
        az_iot_twin_get_callback get_cb = TI(t).pending[i].cb.get_cb;
        az_iot_twin_patch_ack_callback patch_cb = TI(t).pending[i].cb.patch_cb;

        memset(&TI(t).pending[i], 0, sizeof(TI(t).pending[i]));

        if (internal) continue; /* the SDK's own resync GET has no caller */
        if (kind == TWIN_PENDING_GET && get_cb) get_cb(AZ_IOT_ERR_NOT_CONNECTED, NULL, ctx);
        else if (kind == TWIN_PENDING_PATCH && patch_cb) patch_cb(AZ_IOT_ERR_NOT_CONNECTED, NULL, ctx);
    }

    /* The resync buffer holds patches from the old connection's patch sequence;
     * they mean nothing against a fresh one. */
    TI(t).resyncing = false;
    TI(t).resync_used = 0;
    TI(t).resync_count = 0;
    TI(t).push_expected = false;
}

/* Re-anchor version state when the client lands on a new connection.
 *
 * The birth-ack carries the authoritative versions as of birth admission.
 * `reported_version` is adopted directly -- it is only ever used as the
 * if_match for the next write. `desired_local` is reset to 0 instead, because
 * the device advertised version 0 in its birth (this SDK does not persist twin
 * state) and holds no desired payload for this connection until a push or a GET
 * delivers one; treating the authoritative version as applied would make the
 * next incremental patch look in-order and get merged onto state the device
 * does not have.
 *
 * Also arms the twin-push expectation (twin.md 8.5): one timer covers both
 * sections, since a push is a single message. */
static void twin_next_sync_versions(az_iot_twin_client* t)
{
    uint8_t nonce[TWIN_NEXT_CORR_LEN];
    if (az_iot_connection_client__presence_nonce(TI(t).conn, nonce) != AZ_IOT_OK) return;

    if (TI(t).nonce_valid && memcmp(TI(t).nonce, nonce, TWIN_NEXT_CORR_LEN) == 0) return;

    uint64_t desired = 0, reported = 0;
    (void)az_iot_connection_client__presence_twin_versions(TI(t).conn, &desired, &reported);

    TI(t).desired_auth = desired;
    TI(t).desired_local = 0;
    TI(t).reported_version = reported;
    memcpy(TI(t).nonce, nonce, TWIN_NEXT_CORR_LEN);
    TI(t).nonce_valid = true;

    TI(t).resyncing = false;
    TI(t).resync_used = 0;
    TI(t).resync_count = 0;

    bool push_desired = false, push_reported = false;
    az_iot_connection_client__twin_push_flags(TI(t).conn, &push_desired, &push_reported);

    TI(t).push_expected = (push_desired && TI(t).desired_local != TI(t).desired_auth)
                          || (push_reported && reported != 0);
    TI(t).push_attempt = 0;
    if (TI(t).push_expected)
        TI(t).push_deadline_ms = twin_defensive_deadline(t, 0);
}

/* Frame the saved reported patch into ReportedPatch { 1 if_match, 2 payload }
 * with the current authoritative version. The encode buffer holds the caller's
 * payload first and the framed message after it, so a retry can re-frame with a
 * newer if_match without the caller's buffer still being around. */
static bool twin_next_frame_patch(az_iot_twin_client* t, size_t* out_len)
{
    uint8_t* saved = TI(t).encode_buffer;
    size_t saved_len = TI(t).saved_patch_len;
    uint8_t* frame = saved + saved_len;
    size_t frame_cap = TI(t).encode_buffer_len - saved_len;

    size_t pos = 0;
    if ((TI(t).reported_version
         && !az_iot_proto3_write_varint_field(frame, frame_cap, &pos,
                                              1u, TI(t).reported_version))
        || !az_iot_proto3_write_bytes_field(frame, frame_cap, &pos, 2u, saved, saved_len))
    {
        return false;
    }
    *out_len = pos;
    return true;
}

/* Forward declaration: the desired state machine issues its own GET. */
static az_iot_result twin_next_issue_get(
    az_iot_twin_client* t, const az_iot_twin_get_options* opts,
    az_iot_twin_get_callback cb, void* user_ctx, bool internal);

/* Buffer a desired patch received while resyncing. Returns false when the
 * arena or the index is full, which sends the caller down the overflow path. */
static bool twin_resync_buffer_patch(
    az_iot_twin_client* t, uint64_t version, const uint8_t* payload, size_t len)
{
    if (!TI(t).resync_buffer) return false;
    if (TI(t).resync_count >= AZ_IOT_TWIN_MAX_RESYNC_PATCHES) return false;
    if (len > TI(t).resync_buffer_len - TI(t).resync_used) return false;

    size_t idx = TI(t).resync_count;
    TI(t).resync_patches[idx].version = version;
    TI(t).resync_patches[idx].offset = TI(t).resync_used;
    TI(t).resync_patches[idx].len = len;
    if (len) memcpy(TI(t).resync_buffer + TI(t).resync_used, payload, len);
    TI(t).resync_used += len;
    TI(t).resync_count++;
    return true;
}

/* Enter the resyncing state: the desired-patch stream has a gap, so the device
 * cannot apply what it just received on top of what it holds. Ask for a full
 * snapshot and buffer whatever keeps arriving until it lands. */
static void twin_resync_begin(az_iot_twin_client* t)
{
    TI(t).resyncing = true;
    TI(t).resync_used = 0;
    TI(t).resync_count = 0;

    az_iot_twin_get_options opts;
    memset(&opts, 0, sizeof(opts));
    opts.sections = AZ_IOT_TWIN_SECTIONS_DESIRED;
    (void)twin_next_issue_get(t, &opts, NULL, NULL, true);
}

/* Leave the resyncing state by applying a snapshot at `version`, then replaying
 * the buffered patches that are newer than it, in order. Patches at or below
 * the snapshot version are already folded into it and are discarded. */
static void twin_resync_complete(az_iot_twin_client* t, uint64_t version,
                                 const uint8_t* payload, size_t payload_len)
{
    TI(t).desired_local = version;
    dispatch_desired(t, payload, payload_len, version);

    for (size_t i = 0; i < TI(t).resync_count; ++i)
    {
        if (TI(t).resync_patches[i].version <= version) continue;
        TI(t).desired_local = TI(t).resync_patches[i].version;
        dispatch_desired(t,
                         TI(t).resync_buffer + TI(t).resync_patches[i].offset,
                         TI(t).resync_patches[i].len,
                         TI(t).resync_patches[i].version);
    }

    TI(t).resyncing = false;
    TI(t).resync_used = 0;
    TI(t).resync_count = 0;
}

/* Cancel the SDK's own outstanding resync GET. Any response that later arrives
 * for it no longer matches a pending slot and is dropped (twin.md 7.3). */
static void twin_resync_cancel_get(az_iot_twin_client* t)
{
    for (int i = 0; i < AZ_IOT_TWIN_MAX_PENDING; ++i)
    {
        if (TI(t).pending[i].in_use && TI(t).pending[i].internal)
            memset(&TI(t).pending[i], 0, sizeof(TI(t).pending[i]));
    }
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

    /* A push answers any outstanding expectation, whichever sections it
     * carries, so the defensive timer is disarmed regardless. */
    TI(t).push_expected = false;

    if (twin.reported.version) TI(t).reported_version = twin.reported.version;

    /* A snapshot older than what the device has applied says nothing new. */
    bool desired_is_new = twin.desired.version > TI(t).desired_local;

    if (desired_is_new)
    {
        if (TI(t).resyncing)
        {
            /* The snapshot resolves the gap the resync GET was chasing, so the
             * GET is no longer needed. */
            twin_resync_cancel_get(t);
            twin_resync_complete(t, twin.desired.version,
                                 twin.desired.payload, twin.desired.payload_len);
        }
        else
        {
            TI(t).desired_local = twin.desired.version;
        }
    }

    if (TI(t).push_cb) TI(t).push_cb(&twin, TI(t).push_user_ctx);
}

/* desired-patch:1 — DesiredPatch { 1 uint64 version, 2 optional bytes payload }.
 *
 * Implements the device-side desired state machine (twin.md 7.1). Patches are
 * incremental, so they may only be applied in order: one that skips ahead means
 * the device missed something and must resynchronize from a snapshot rather
 * than merge onto a state it does not have. A patch with no payload is a
 * version probe -- the service asking "are you current?" -- which never reaches
 * the application. */
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

    /* Anything at or behind the applied version is stale in every state. */
    if (version <= TI(t).desired_local && !(has_payload && TI(t).resyncing)) return;

    if (TI(t).resyncing)
    {
        /* A probe carries nothing to replay, and the in-flight GET already
         * resolves at a version no older than the probe's. */
        if (!has_payload) return;

        if (!twin_resync_buffer_patch(t, version, patch, patch_len))
        {
            /* Overflow. Discard the buffer and re-GET: the fresh snapshot is at
             * least as new as the highest patch that was buffered, so it
             * subsumes everything dropped (twin.md 8.3). */
            twin_resync_cancel_get(t);
            twin_resync_begin(t);
        }
        return;
    }

    if (!has_payload)
    {
        /* A probe ahead of us means we missed a patch; resync. */
        twin_resync_begin(t);
        return;
    }

    if (version == TI(t).desired_local + 1u)
    {
        TI(t).desired_local = version;
        dispatch_desired(t, patch, patch_len, version);
        return;
    }

    /* A gap: seed the buffer with this patch, then resynchronize. */
    twin_resync_begin(t);
    (void)twin_resync_buffer_patch(t, version, patch, patch_len);
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

    if (twin.reported.version) TI(t).reported_version = twin.reported.version;

    bool internal = TI(t).pending[idx].internal;
    az_iot_twin_get_callback cb = TI(t).pending[idx].cb.get_cb;
    void* ctx = TI(t).pending[idx].user_ctx;
    memset(&TI(t).pending[idx], 0, sizeof(TI(t).pending[idx]));

    /* A snapshot is exactly what the resync was waiting for: apply it and
     * replay the patches that arrived on top of it. */
    if (TI(t).resyncing && twin.desired.version > TI(t).desired_local)
    {
        twin_resync_complete(t, twin.desired.version,
                             twin.desired.payload, twin.desired.payload_len);
    }
    else if (twin.desired.version > TI(t).desired_local && twin.desired.payload)
    {
        TI(t).desired_local = twin.desired.version;
    }

    if (!internal && cb) cb(AZ_IOT_OK, &twin, ctx);
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

    /* Anchor the version state to this connection before handling anything, so
     * a handler that starts a resync is not immediately undone by the
     * re-anchoring. */
    twin_next_sync_versions(t);

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

/* Publish a Hub-Next GET into a freshly reserved pending slot. Shared by the
 * application-facing API and the desired resync, which issues its own
 * (`internal`) GET with no caller to report back to. */
static az_iot_result twin_next_issue_get(
    az_iot_twin_client* t, const az_iot_twin_get_options* opts,
    az_iot_twin_get_callback cb, void* user_ctx, bool internal)
{
    int idx = alloc_pending(t);
    if (idx < 0) return AZ_IOT_ERR_NOT_SUPPORTED;

    const char* device_id = az_iot_connection_client__device_id(TI(t).conn);
    char topic[AZ_IOT_TWIN_TOPIC_MAX];
    int n = snprintf(topic, sizeof(topic), TWIN_NEXT_TOPIC_SRV_FMT, device_id);
    if (n < 0 || (size_t)n >= sizeof(topic)) return AZ_IOT_ERR_NOT_SUPPORTED;

    az_iot_twin_get_options effective;
    memset(&effective, 0, sizeof(effective));
    if (opts) effective = *opts;
    if (effective.sections == 0) effective.sections = AZ_IOT_TWIN_SECTIONS_BOTH;

    /* TwinGet { 1 sections, 2 if_not_match_desired, 3 if_not_match_reported }.
     * proto3 omits zero-valued fields, and 0 already means "no filter". */
    uint8_t body[32];
    size_t body_len = 0;
    if (!az_iot_proto3_write_varint_field(body, sizeof(body), &body_len,
                                          1u, (uint64_t)effective.sections)
        || (effective.if_not_match_desired
            && !az_iot_proto3_write_varint_field(body, sizeof(body), &body_len,
                                                 2u, effective.if_not_match_desired))
        || (effective.if_not_match_reported
            && !az_iot_proto3_write_varint_field(body, sizeof(body), &body_len,
                                                 3u, effective.if_not_match_reported)))
    {
        return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
    }

    memset(&TI(t).pending[idx], 0, sizeof(TI(t).pending[idx]));
    az_iot_connection_client__gen_uuid(TI(t).conn, TI(t).pending[idx].corr);
    TI(t).pending[idx].in_use    = true;
    TI(t).pending[idx].kind      = TWIN_PENDING_GET;
    TI(t).pending[idx].cb.get_cb = cb;
    TI(t).pending[idx].user_ctx  = user_ctx;
    TI(t).pending[idx].internal  = internal;
    TI(t).pending[idx].get_opts  = effective;
    TI(t).pending[idx].attempt   = 0;
    TI(t).pending[idx].deadline_ms = twin_defensive_deadline(t, 0);

    az_iot_mqtt_user_property type_prop = { TWIN_TYPE_KEY, TWIN_TYPE_GET };
    az_iot_mqtt_message out = { 0 };
    out.topic = topic;
    out.qos = AZ_IOT_MQTT_QOS_0;
    out.payload = body;
    out.payload_len = body_len;
    out.content_type = TWIN_CONTENT_TYPE;
    out.user_properties = &type_prop;
    out.user_properties_count = 1;
    out.correlation_data = TI(t).pending[idx].corr;
    out.correlation_data_len = TWIN_NEXT_CORR_LEN;

    az_iot_result r = az_iot_connection_client__publish(TI(t).conn, &out, NULL, NULL);
    if (r != AZ_IOT_OK) memset(&TI(t).pending[idx], 0, sizeof(TI(t).pending[idx]));
    return r;
}

az_iot_result az_iot_twin_client_get(
    az_iot_twin_client* twin, az_iot_twin_get_callback cb, void* user_ctx)
{
    return az_iot_twin_client_get_with_options(twin, NULL, cb, user_ctx);
}

az_iot_result az_iot_twin_client_get_with_options(
    az_iot_twin_client* twin,
    const az_iot_twin_get_options* opts,
    az_iot_twin_get_callback cb,
    void* user_ctx)
{
    if (!twin) return AZ_IOT_ERR_INVALID_ARG;

    const az_iot_protocol_profile* profile =
        az_iot_connection_client__profile(TI(twin).conn);
    if (profile && profile->flavor == AZ_IOT_HUB_FLAVOR_NEXT)
    {
        twin_next_sync_versions(twin);
        return twin_next_issue_get(twin, opts, cb, user_ctx, false);
    }

    /* Classic returns the whole twin document; there is no section selector and
     * no per-section version to filter on. */
    int idx = alloc_pending(twin);
    if (idx < 0) return AZ_IOT_ERR_NOT_SUPPORTED;

    uint32_t rid = TI(twin).next_rid++;
    if (TI(twin).next_rid == 0) TI(twin).next_rid = 1; /* never reuse 0 */

    char topic[AZ_IOT_TWIN_TOPIC_MAX];
    int n = snprintf(topic, sizeof(topic), "$iothub/twin/GET/?$rid=%u", (unsigned)rid);
    if (n < 0 || (size_t)n >= sizeof(topic)) return AZ_IOT_ERR_NOT_SUPPORTED;

    memset(&TI(twin).pending[idx], 0, sizeof(TI(twin).pending[idx]));
    TI(twin).pending[idx].in_use    = true;
    TI(twin).pending[idx].rid       = rid;
    TI(twin).pending[idx].kind      = TWIN_PENDING_GET;
    TI(twin).pending[idx].cb.get_cb = cb;
    TI(twin).pending[idx].user_ctx  = user_ctx;

    az_iot_mqtt_message out = { 0 };
    out.topic = topic;
    out.qos   = AZ_IOT_MQTT_QOS_0;

    az_iot_result r = az_iot_connection_client__publish(
        TI(twin).conn, &out, NULL, NULL);
    if (r != AZ_IOT_OK) memset(&TI(twin).pending[idx], 0, sizeof(TI(twin).pending[idx]));
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
    uint8_t corr[TWIN_NEXT_CORR_LEN];
    az_iot_mqtt_user_property type_prop = { TWIN_TYPE_KEY, TWIN_TYPE_REPORTED_PATCH };

    if (is_next)
    {
        const char* device_id = az_iot_connection_client__device_id(TI(twin).conn);
        n = snprintf(topic, sizeof(topic), TWIN_NEXT_TOPIC_SRV_FMT, device_id);

        twin_next_sync_versions(twin);

        /* Keep a copy of the caller's patch so the exchange can be re-sent with
         * a fresh if_match if the service never answers; the caller's buffer is
         * only borrowed for the duration of this call. */
        if (!TI(twin).encode_buffer || patch_len > TI(twin).encode_buffer_len)
            return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
        if (patch_len) memcpy(TI(twin).encode_buffer, patch, patch_len);
        TI(twin).saved_patch_len = patch_len;

        size_t framed_len = 0;
        if (!twin_next_frame_patch(twin, &framed_len)) return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
        body = TI(twin).encode_buffer + patch_len;
        body_len = framed_len;

        az_iot_connection_client__gen_uuid(TI(twin).conn, corr);
    }
    else
    {
        n = snprintf(topic, sizeof(topic),
                     "$iothub/twin/PATCH/properties/reported/?$rid=%u", (unsigned)rid);
    }
    if (n < 0 || (size_t)n >= sizeof(topic)) return AZ_IOT_ERR_NOT_SUPPORTED;

    memset(&TI(twin).pending[idx], 0, sizeof(TI(twin).pending[idx]));
    TI(twin).pending[idx].in_use      = true;
    TI(twin).pending[idx].rid         = rid;
    TI(twin).pending[idx].kind        = TWIN_PENDING_PATCH;
    TI(twin).pending[idx].cb.patch_cb = cb;
    TI(twin).pending[idx].user_ctx    = user_ctx;
    if (is_next)
    {
        memcpy(TI(twin).pending[idx].corr, corr, TWIN_NEXT_CORR_LEN);
        TI(twin).pending[idx].deadline_ms = twin_defensive_deadline(twin, 0);
    }

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
    TI(twin).saved_patch_len = 0;
    return AZ_IOT_OK;
}

az_iot_result az_iot_twin_client_set_resync_buffer(
    az_iot_twin_client* twin,
    uint8_t* buffer,
    size_t buffer_len)
{
    if (!twin) return AZ_IOT_ERR_INVALID_ARG;
    if (buffer && buffer_len == 0) return AZ_IOT_ERR_NOT_ENOUGH_SPACE;

    TI(twin).resync_buffer = buffer;
    TI(twin).resync_buffer_len = buffer ? buffer_len : 0;
    TI(twin).resync_used = 0;
    TI(twin).resync_count = 0;
    return AZ_IOT_OK;
}

/* Re-publish a timed-out exchange with a fresh correlation id and the next
 * step of the escalating schedule. The identifier must change so a late
 * response to the previous attempt is not mistaken for this one. */
static void twin_next_retry_pending(az_iot_twin_client* t, int idx)
{
    const char* device_id = az_iot_connection_client__device_id(TI(t).conn);
    char topic[AZ_IOT_TWIN_TOPIC_MAX];
    int n = snprintf(topic, sizeof(topic), TWIN_NEXT_TOPIC_SRV_FMT, device_id);
    if (n < 0 || (size_t)n >= sizeof(topic)) return;

    uint8_t body[32];
    size_t body_len = 0;
    const uint8_t* payload = body;
    az_iot_mqtt_user_property type_prop = { TWIN_TYPE_KEY, TWIN_TYPE_GET };

    if (TI(t).pending[idx].kind == TWIN_PENDING_GET)
    {
        const az_iot_twin_get_options* o = &TI(t).pending[idx].get_opts;
        if (!az_iot_proto3_write_varint_field(body, sizeof(body), &body_len,
                                              1u, (uint64_t)o->sections)
            || (o->if_not_match_desired
                && !az_iot_proto3_write_varint_field(body, sizeof(body), &body_len,
                                                     2u, o->if_not_match_desired))
            || (o->if_not_match_reported
                && !az_iot_proto3_write_varint_field(body, sizeof(body), &body_len,
                                                     3u, o->if_not_match_reported)))
        {
            return;
        }
    }
    else
    {
        /* Re-frame with the current authoritative version: it may have advanced
         * since the first attempt. */
        type_prop.value = TWIN_TYPE_REPORTED_PATCH;
        if (!twin_next_frame_patch(t, &body_len)) return;
        payload = TI(t).encode_buffer + TI(t).saved_patch_len;
    }

    az_iot_connection_client__gen_uuid(TI(t).conn, TI(t).pending[idx].corr);
    TI(t).pending[idx].attempt++;
    TI(t).pending[idx].deadline_ms =
        twin_defensive_deadline(t, TI(t).pending[idx].attempt);

    az_iot_mqtt_message out = { 0 };
    out.topic = topic;
    out.payload = payload;
    out.payload_len = body_len;
    out.qos = AZ_IOT_MQTT_QOS_0;
    out.content_type = TWIN_CONTENT_TYPE;
    out.user_properties = &type_prop;
    out.user_properties_count = 1;
    out.correlation_data = TI(t).pending[idx].corr;
    out.correlation_data_len = TWIN_NEXT_CORR_LEN;

    (void)az_iot_connection_client__publish(TI(t).conn, &out, NULL, NULL);
}

void az_iot_twin_client__force_timeouts(az_iot_twin_client* twin)
{
    if (!twin) return;
    if (TI(twin).push_expected) TI(twin).push_deadline_ms = 0;
    for (int i = 0; i < AZ_IOT_TWIN_MAX_PENDING; ++i)
    {
        if (TI(twin).pending[i].in_use && TI(twin).pending[i].deadline_ms != 0)
            TI(twin).pending[i].deadline_ms = 1;
    }
}

az_iot_result az_iot_twin_client_do_work(az_iot_twin_client* twin)
{
    if (!twin || !TI(twin).conn) return AZ_IOT_ERR_INVALID_ARG;

    const az_iot_protocol_profile* profile =
        az_iot_connection_client__profile(TI(twin).conn);
    if (!profile || profile->flavor != AZ_IOT_HUB_FLAVOR_NEXT) return AZ_IOT_OK;

    /* Everything below is scoped to a live connection. Once it drops, the
     * exchanges riding on it cannot be resumed (QoS 0), so they are abandoned
     * and their callers told, rather than left waiting forever. */
    uint8_t nonce[TWIN_NEXT_CORR_LEN];
    if (az_iot_connection_client__presence_nonce(TI(twin).conn, nonce) != AZ_IOT_OK)
    {
        if (TI(twin).nonce_valid)
        {
            twin_next_abandon_exchanges(twin);
            TI(twin).nonce_valid = false;
        }
        return AZ_IOT_OK;
    }

    twin_next_sync_versions(twin);

    uint64_t now = az_iot_time_mono_ms();

    /* A birth-triggered push that never arrived means the service believes it
     * dispatched state the device never saw. Only a fresh connection re-runs
     * that decision, so reconnect rather than paper over it with a GET. */
    if (TI(twin).push_expected && now >= TI(twin).push_deadline_ms)
    {
        TI(twin).push_expected = false;
        TI(twin).push_attempt++;
        (void)az_iot_connection_client_close(TI(twin).conn);
        return AZ_IOT_OK;
    }

    for (int i = 0; i < AZ_IOT_TWIN_MAX_PENDING; ++i)
    {
        if (!TI(twin).pending[i].in_use) continue;
        if (TI(twin).pending[i].deadline_ms == 0) continue; /* Classic slot */
        if (now < TI(twin).pending[i].deadline_ms) continue;

        twin_next_retry_pending(twin, i);
    }

    return AZ_IOT_OK;
}
