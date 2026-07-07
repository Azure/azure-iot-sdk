// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/* SPDX-License-Identifier: MIT */
/* ConnectionClient core (Phase 2.1 + 2.2 + DPS integration).
 *
 * Owns:
 *   - the adapter registry (factories registered by the application)
 *   - the active MQTT client for the current session
 *   - the lifecycle state machine
 *     (IDLE -> CONNECTING -> CONNECTED -> DISCONNECTING -> IDLE,
 *      with RECONNECTING and FAULTED side branches)
 *   - DPS provisioning (when opts.dps.id_scope is set and opts.host is NULL,
 *     open() internally provisions via DPS before connecting to the hub)
 *
 * Single-threaded contract: every state transition and every user state-callback
 * fires from inside az_iot_connection_client_do_work(). The MQTT adapter's
 * inbound callback is also delivered from process_loop() (which we drive from
 * do_work()); the adapter is responsible for marshalling its own background
 * thread events into that callback.
 *
 * Reconnect (Phase 2.2): when opts.reconnect is enabled
 * (initial_delay_ms > 0), unexpected drops (CONNACK fail, peer DISCONNECT,
 * inbound ERROR) transition to RECONNECTING; do_work() then re-opens after the
 * computed backoff (with jitter). User-initiated close() always goes to IDLE
 * regardless. If max_attempts > 0 is configured and reached, we transition to
 * FAULTED.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "azure/iot/az_iot_connection_client.h"
#include "azure/iot/az_iot_mqtt_iface.h"
#include "azure/iot/az_iot_result.h"

#include "internal/connection_client_internal.h"
#include "internal/dispatch.h"
#include "internal/log_internal.h"
#include "internal/protocol_profile.h"
#include "internal/reconnect.h"

#include <azure/az_core.h>
#include <azure/iot/az_iot_hub_client.h>
#include <azure/iot/az_iot_provisioning_client.h>

/* Local aliases for the enum constants exposed in the public header as
 * anonymous enum values.  Keeps the implementation readable. */
#define DEFER_NONE      AZ_IOT_CONN_DEFER_NONE
#define DEFER_FAULT     AZ_IOT_CONN_DEFER_FAULT
#define DEFER_RECONNECT AZ_IOT_CONN_DEFER_RECONNECT
#define DEFER_IDLE      AZ_IOT_CONN_DEFER_IDLE

#define DPS_PHASE_NONE        AZ_IOT_DPS_PHASE_NONE
#define DPS_PHASE_CONNECTING  AZ_IOT_DPS_PHASE_CONNECTING
#define DPS_PHASE_SUBSCRIBING AZ_IOT_DPS_PHASE_SUBSCRIBING
#define DPS_PHASE_REGISTERING AZ_IOT_DPS_PHASE_REGISTERING
#define DPS_PHASE_POLLING     AZ_IOT_DPS_PHASE_POLLING
#define DPS_PHASE_DONE        AZ_IOT_DPS_PHASE_DONE

/* ------------------------------------------------------------------------- */
/* helpers                                                                   */
/* ------------------------------------------------------------------------- */

const char* az_iot_mqtt_role_to_string(az_iot_mqtt_role_t r)
{
    switch (r)
    {
        case AZ_IOT_MQTT_ROLE_DPS:         return "DPS";
        case AZ_IOT_MQTT_ROLE_HUB_CLASSIC: return "HUB_CLASSIC";
        case AZ_IOT_MQTT_ROLE_HUB_NEXT:    return "HUB_NEXT";
        default:                        return "ROLE?";
    }
}

static bool reconnect_enabled(const az_iot_connection_client_t* c)
{
    return c->opts.reconnect.initial_delay_ms > 0;
}

static void transition(az_iot_connection_client_t* c,
                       az_iot_connection_state_t next,
                       az_iot_result_t reason)
{
    if (c->state == next) return;
    c->state = next;
    if (c->state_cb) c->state_cb(next, reason, c->state_cb_ctx);
}

static const az_iot_mqtt_factory_t* find_factory(
    const az_iot_connection_client_t* c,
    az_iot_mqtt_version_t version)
{
    for (size_t i = 0; i < c->factory_count; ++i)
    {
        const az_iot_mqtt_factory_t* f = &c->factories[i];
        if (f->version != version) continue;
        return f;
    }
    return NULL;
}

static void teardown_active(az_iot_connection_client_t* c)
{
    if (c->active_client && c->active_client->iface && c->active_client->iface->destroy)
    {
        c->active_client->iface->destroy(c->active_client);
    }
    c->active_client = NULL;
    /* Drop any pending PUBACK correlation entries: the packet_ids belonged to
     * the now-destroyed adapter session and won't be reused. Callers waiting
     * on these acks won't be notified, which matches the at-least-once
     * semantics of QoS 1 (the publish must be retried after reconnect). */
    for (size_t i = 0; i < AZ_IOT_MAX_PENDING_PUBACKS; ++i)
    {
        c->pending_pubacks[i].in_use = false;
        c->pending_pubacks[i].cb = NULL;
        c->pending_pubacks[i].user_ctx = NULL;
        c->pending_pubacks[i].packet_id = 0;
    }
}

/* Forward decl — used in on_mqtt_event via the deferred-action queue. */
static az_iot_result_t start_connect_attempt(az_iot_connection_client_t* c);

/* Forward decl — used in dps_apply_deferred(). */
static az_iot_result_t replace_owned_string(
    char** owned_slot, const char** opts_slot, const char* s);

static void schedule_reconnect(az_iot_connection_client_t* c, az_iot_result_t reason)
{
    teardown_active(c);
    c->reconnect_attempt++;

    if (c->opts.reconnect.max_attempts > 0 &&
        c->reconnect_attempt > c->opts.reconnect.max_attempts)
    {
        transition(c, AZ_IOT_CONN_STATE_FAULTED, reason);
        return;
    }

    uint32_t delay = az_iot_reconnect_delay_ms(
        &c->opts.reconnect, c->reconnect_attempt, &c->rng_state);
    c->reconnect_due_ms = az_iot_time_mono_ms() + delay;
    transition(c, AZ_IOT_CONN_STATE_RECONNECTING, reason);
}

/* ------------------------------------------------------------------------- */
/* DPS provisioning (internal, driven from open/do_work)                     */
/* ------------------------------------------------------------------------- */

static bool dps_configured(const az_iot_connection_client_t* c)
{
    return c->opts.dps.id_scope != NULL && c->opts.dps.id_scope[0] != '\0';
}

static void dps_teardown_mqtt(az_iot_connection_client_t* c)
{
    if (c->dps_mqtt && c->dps_mqtt->iface && c->dps_mqtt->iface->destroy)
        c->dps_mqtt->iface->destroy(c->dps_mqtt);
    c->dps_mqtt = NULL;
}

static void dps_finalize(az_iot_connection_client_t* c,
                         az_iot_result_t status, bool have_assignment)
{
    if (c->dps_pending_finalize) return;
    c->dps_pending_finalize = true;
    c->dps_pending_status = status;
    c->dps_pending_have_assignment = have_assignment;
}

static az_iot_result_t dps_do_register_publish(az_iot_connection_client_t* c)
{
    char topic[AZ_IOT_DPS_TOPIC_BUF];
    size_t topic_len = 0;
    az_result ar = az_iot_provisioning_client_register_get_publish_topic(
        &c->dps_prov, topic, sizeof(topic), &topic_len);
    if (az_result_failed(ar)) return AZ_IOT_ERR_INTERNAL;

    az_iot_mqtt_message_t msg = {0};
    msg.topic = topic;
    msg.payload = NULL;
    msg.payload_len = 0;
    msg.qos = AZ_IOT_MQTT_QOS_1;
    msg.retain = false;

    /* CSR-based enrollment (D2): request an operational cert by sending the
     * provider's CSR as the registration body {"csr":"<base64 DER>"}. The
     * registration id travels in the DPS username/topic, not the body. */
    char* csr_body = NULL;
    if (c->dps_enrolling)
    {
        az_iot_certificate_provider_t* p = c->opts.certificate_provider;
        az_iot_certificate_signing_request_t csr = {0};
        if (!p || !p->vtable->get_csr
            || p->vtable->get_csr(p, c->opts.dps.registration_id, &csr) != AZ_IOT_OK
            || !csr.csr_base64)
        {
            return AZ_IOT_ERR_INTERNAL;
        }
        size_t cap = strlen(csr.csr_base64) + sizeof("{\"csr\":\"\"}");
        csr_body = (char*)malloc(cap);
        int n = csr_body ? snprintf(csr_body, cap, "{\"csr\":\"%s\"}", csr.csr_base64) : -1;
        if (p->vtable->release_csr) p->vtable->release_csr(p, &csr);
        if (!csr_body) return AZ_IOT_ERR_OUT_OF_MEMORY;
        if (n < 0 || (size_t)n >= cap) { free(csr_body); return AZ_IOT_ERR_INTERNAL; }
        msg.payload = (const uint8_t*)csr_body;
        msg.payload_len = (size_t)n;
    }

    uint16_t pid = 0;
    az_iot_result_t r = c->dps_mqtt->iface->publish(c->dps_mqtt, &msg, &pid);
    free(csr_body); /* publish copies the payload synchronously */
    if (r == AZ_IOT_OK) c->dps_phase = DPS_PHASE_REGISTERING;
    return r;
}

static az_iot_result_t dps_do_query_publish(az_iot_connection_client_t* c)
{
    char topic[AZ_IOT_DPS_TOPIC_BUF];
    size_t topic_len = 0;
    az_span op_id = az_span_create((uint8_t*)c->dps_operation_id, (int32_t)c->dps_operation_id_len);
    az_result ar = az_iot_provisioning_client_query_status_get_publish_topic(
        &c->dps_prov, op_id, topic, sizeof(topic), &topic_len);
    if (az_result_failed(ar)) return AZ_IOT_ERR_INTERNAL;

    az_iot_mqtt_message_t msg = {0};
    msg.topic = topic;
    msg.qos = AZ_IOT_MQTT_QOS_1;

    uint16_t pid = 0;
    az_iot_result_t r = c->dps_mqtt->iface->publish(c->dps_mqtt, &msg, &pid);
    if (r == AZ_IOT_OK) c->dps_phase = DPS_PHASE_REGISTERING;
    return r;
}

/* Reads base64 cert strings from a JSON array (reader positioned so the next
 * tokens are the array's string elements, i.e. the BEGIN_ARRAY was consumed),
 * PEM-wraps each into a heap buffer. On success fills pem[0..*out_count-1]
 * (caller frees each) and returns AZ_IOT_OK; on failure frees any it allocated. */
static az_iot_result_t json_collect_pem_chain(
    az_json_reader* jr, char** pem, size_t max, size_t* out_count)
{
    size_t count = 0;
    az_iot_result_t rc = AZ_IOT_OK;
    while (az_result_succeeded(az_json_reader_next_token(jr))
           && jr->token.kind != AZ_JSON_TOKEN_END_ARRAY)
    {
        if (jr->token.kind != AZ_JSON_TOKEN_STRING) { rc = AZ_IOT_ERR_PROTOCOL; goto fail; }
        if (count >= max) { rc = AZ_IOT_ERR_NOT_ENOUGH_SPACE; goto fail; }

        char b64[4096];
        int32_t vlen = 0;
        if (az_result_failed(az_json_token_get_string(&jr->token, b64, (int32_t)sizeof(b64), &vlen)))
        {
            rc = AZ_IOT_ERR_NOT_ENOUGH_SPACE;
            goto fail;
        }
        size_t cap = (size_t)vlen + 64;
        char* pw = (char*)malloc(cap);
        if (!pw) { rc = AZ_IOT_ERR_OUT_OF_MEMORY; goto fail; }
        int n = snprintf(pw, cap,
            "-----BEGIN CERTIFICATE-----\n%.*s\n-----END CERTIFICATE-----\n",
            (int)vlen, b64);
        if (n < 0 || (size_t)n >= cap) { free(pw); rc = AZ_IOT_ERR_INTERNAL; goto fail; }
        pem[count++] = pw;
    }
    *out_count = count;
    return AZ_IOT_OK;
fail:
    for (size_t i = 0; i < count; ++i) free(pem[i]);
    *out_count = 0;
    return rc;
}

/* Parse registrationState.issuedCertificateChain (an array of base64 DER certs)
 * from the DPS ASSIGNED payload, PEM-wrap each entry, and hand the chain to the
 * certificate_provider to persist as the operational identity. azure-sdk-for-c
 * does not surface this field, so we walk the raw payload with az_json. */
static az_iot_result_t dps_store_issued_cert(az_iot_connection_client_t* c, az_span payload)
{
    az_json_reader jr;
    if (az_result_failed(az_json_reader_init(&jr, payload, NULL))
        || az_result_failed(az_json_reader_next_token(&jr))
        || jr.token.kind != AZ_JSON_TOKEN_BEGIN_OBJECT)
    {
        return AZ_IOT_ERR_PROTOCOL;
    }

    /* Descend into registrationState. */
    bool in_reg = false;
    while (az_result_succeeded(az_json_reader_next_token(&jr))
           && jr.token.kind != AZ_JSON_TOKEN_END_OBJECT)
    {
        if (jr.token.kind != AZ_JSON_TOKEN_PROPERTY_NAME) continue;
        bool m = az_json_token_is_text_equal(&jr.token, AZ_SPAN_FROM_STR("registrationState"));
        if (az_result_failed(az_json_reader_next_token(&jr))) return AZ_IOT_ERR_PROTOCOL;
        if (m && jr.token.kind == AZ_JSON_TOKEN_BEGIN_OBJECT) { in_reg = true; break; }
        if (jr.token.kind == AZ_JSON_TOKEN_BEGIN_OBJECT || jr.token.kind == AZ_JSON_TOKEN_BEGIN_ARRAY)
            if (az_result_failed(az_json_reader_skip_children(&jr))) return AZ_IOT_ERR_PROTOCOL;
    }
    if (!in_reg) return AZ_IOT_ERR_NOT_FOUND;

    /* Find issuedCertificateChain array. */
    bool in_chain = false;
    while (az_result_succeeded(az_json_reader_next_token(&jr))
           && jr.token.kind != AZ_JSON_TOKEN_END_OBJECT)
    {
        if (jr.token.kind != AZ_JSON_TOKEN_PROPERTY_NAME) continue;
        bool m = az_json_token_is_text_equal(&jr.token, AZ_SPAN_FROM_STR("issuedCertificateChain"));
        if (az_result_failed(az_json_reader_next_token(&jr))) return AZ_IOT_ERR_PROTOCOL;
        if (m && jr.token.kind == AZ_JSON_TOKEN_BEGIN_ARRAY) { in_chain = true; break; }
        if (jr.token.kind == AZ_JSON_TOKEN_BEGIN_OBJECT || jr.token.kind == AZ_JSON_TOKEN_BEGIN_ARRAY)
            if (az_result_failed(az_json_reader_skip_children(&jr))) return AZ_IOT_ERR_PROTOCOL;
    }
    if (!in_chain) return AZ_IOT_ERR_NOT_FOUND;

    /* Collect + PEM-wrap the base64 chain (leaf first). */
    enum { DPS_MAX_CHAIN = 6 };
    char* pem[DPS_MAX_CHAIN] = {0};
    size_t count = 0;
    az_iot_result_t rc = json_collect_pem_chain(&jr, pem, DPS_MAX_CHAIN, &count);
    if (rc != AZ_IOT_OK) return rc;
    if (count == 0) return AZ_IOT_ERR_NOT_FOUND;

    az_iot_certificate_provider_t* p = c->opts.certificate_provider;
    az_iot_issued_certificate_t issued;
    issued.client_cert_chain_pem = (const char* const*)pem;
    issued.count = count;

    /* Persist via the provider (if capable) and/or notify the app (D4). The
     * issued chain must be handled by at least one of the two. */
    bool handled = false;
    if (p && p->vtable->store_issued_certificate)
    {
        rc = p->vtable->store_issued_certificate(p, &issued);
        handled = (rc == AZ_IOT_OK);
    }
    if (rc == AZ_IOT_OK && c->op_cert_cb)
    {
        c->op_cert_cb(&issued, c->op_cert_cb_ctx);
        handled = true;
    }
    if (rc == AZ_IOT_OK && !handled) rc = AZ_IOT_ERR_NOT_SUPPORTED;
    for (size_t i = 0; i < count; ++i) free(pem[i]);
    return rc;
}

static void on_dps_mqtt_event(const az_iot_mqtt_event_t* evt, void* user_ctx)
{
    az_iot_connection_client_t* c = (az_iot_connection_client_t*)user_ctx;
    if (!c || !evt) return;

    switch (evt->kind)
    {
        case AZ_IOT_MQTT_EVT_CONNECTED:
            if (evt->status != AZ_IOT_OK)
            {
                dps_finalize(c, evt->status, false);
                return;
            }
            {
                uint16_t pid = 0;
                az_iot_result_t r = c->dps_mqtt->iface->subscribe(
                    c->dps_mqtt, AZ_IOT_PROVISIONING_CLIENT_REGISTER_SUBSCRIBE_TOPIC,
                    AZ_IOT_MQTT_QOS_1, &pid);
                if (r != AZ_IOT_OK) { dps_finalize(c, r, false); return; }
                c->dps_phase = DPS_PHASE_SUBSCRIBING;
            }
            break;

        case AZ_IOT_MQTT_EVT_SUBSCRIBE_ACK:
            if (c->dps_phase != DPS_PHASE_SUBSCRIBING) break;
            if (evt->status != AZ_IOT_OK)
            {
                dps_finalize(c, evt->status, false);
                return;
            }
            {
                az_iot_result_t r = dps_do_register_publish(c);
                if (r != AZ_IOT_OK) { dps_finalize(c, r, false); return; }
            }
            break;

        case AZ_IOT_MQTT_EVT_MESSAGE:
        {
            if (!evt->message || !evt->message->topic) break;
            if (c->dps_phase != DPS_PHASE_REGISTERING && c->dps_phase != DPS_PHASE_POLLING) break;

            az_span topic_span = az_span_create(
                (uint8_t*)(uintptr_t)evt->message->topic,
                (int32_t)strlen(evt->message->topic));
            az_span payload_span = az_span_create(
                (uint8_t*)(uintptr_t)evt->message->payload,
                (int32_t)evt->message->payload_len);

            az_iot_provisioning_client_register_response resp = {0};
            az_result ar = az_iot_provisioning_client_parse_received_topic_and_payload(
                &c->dps_prov, topic_span, payload_span, &resp);
            if (az_result_failed(ar)) break;

            switch (resp.operation_status)
            {
                case AZ_IOT_PROVISIONING_STATUS_ASSIGNED:
                {
                    int32_t hub_n = az_span_size(resp.registration_state.assigned_hub_hostname);
                    int32_t dev_n = az_span_size(resp.registration_state.device_id);
                    if (hub_n < 0 || (size_t)hub_n + 1 > sizeof(c->dps_assigned_hub) ||
                        dev_n < 0 || (size_t)dev_n + 1 > sizeof(c->dps_assigned_device_id))
                    {
                        dps_finalize(c, AZ_IOT_ERR_NOT_SUPPORTED, false);
                        return;
                    }
                    memcpy(c->dps_assigned_hub,
                           az_span_ptr(resp.registration_state.assigned_hub_hostname),
                           (size_t)hub_n);
                    c->dps_assigned_hub[hub_n] = '\0';
                    memcpy(c->dps_assigned_device_id,
                           az_span_ptr(resp.registration_state.device_id),
                           (size_t)dev_n);
                    c->dps_assigned_device_id[dev_n] = '\0';
                    if (c->dps_enrolling)
                    {
                        az_iot_result_t sc = dps_store_issued_cert(c, payload_span);
                        if (sc != AZ_IOT_OK) { dps_finalize(c, sc, false); return; }
                        c->dps_have_issued_cert = true;
                    }
                    dps_finalize(c, AZ_IOT_OK, true);
                    return;
                }
                case AZ_IOT_PROVISIONING_STATUS_FAILED:
                case AZ_IOT_PROVISIONING_STATUS_DISABLED:
                    dps_finalize(c, AZ_IOT_ERR_MQTT, false);
                    return;

                case AZ_IOT_PROVISIONING_STATUS_UNASSIGNED:
                case AZ_IOT_PROVISIONING_STATUS_ASSIGNING:
                default:
                {
                    int32_t op_n = az_span_size(resp.operation_id);
                    if (op_n < 0 || (size_t)op_n > sizeof(c->dps_operation_id))
                    {
                        dps_finalize(c, AZ_IOT_ERR_NOT_SUPPORTED, false);
                        return;
                    }
                    memcpy(c->dps_operation_id, az_span_ptr(resp.operation_id), (size_t)op_n);
                    c->dps_operation_id_len = (size_t)op_n;
                    c->dps_poll_due_ms = az_iot_time_mono_ms() +
                        (uint64_t)resp.retry_after_seconds * 1000ull;
                    c->dps_phase = DPS_PHASE_POLLING;
                    break;
                }
            }
            break;
        }

        case AZ_IOT_MQTT_EVT_DISCONNECTED:
        case AZ_IOT_MQTT_EVT_ERROR:
        {
            az_iot_result_t r = (evt->status != AZ_IOT_OK)
                ? evt->status : AZ_IOT_ERR_NOT_CONNECTED;
            dps_finalize(c, r, false);
            break;
        }

        default:
            break;
    }
}

/* Start the DPS provisioning flow. Called from _open() when DPS is configured. */
static az_iot_result_t dps_start(az_iot_connection_client_t* c)
{
    const char* endpoint = c->opts.dps.global_endpoint;
    if (!endpoint || !endpoint[0])
        endpoint = "global.azure-devices-provisioning.net";

    az_span ep_span = az_span_create_from_str((char*)(uintptr_t)endpoint);
    az_span scope_span = az_span_create_from_str((char*)(uintptr_t)c->opts.dps.id_scope);
    az_span reg_span = az_span_create_from_str((char*)(uintptr_t)c->opts.dps.registration_id);
    az_result ar = az_iot_provisioning_client_init(&c->dps_prov, ep_span, scope_span, reg_span, NULL);
    if (az_result_failed(ar)) return AZ_IOT_ERR_INVALID_ARG;

    const az_iot_mqtt_factory_t* f = find_factory(
        c, AZ_IOT_MQTT_VERSION_3_1_1);
    if (!f) return AZ_IOT_ERR_NOT_SUPPORTED;

    az_iot_mqtt_client_t* mc = f->create(f->factory_ctx);
    if (!mc || !mc->iface) return AZ_IOT_ERR_INTERNAL;

    mc->iface->set_inbound_cb(mc, on_dps_mqtt_event, c);

    az_iot_mqtt_connect_options_t copts = {0};
    copts.host = endpoint;
    copts.port = 8883;
    copts.client_id = c->opts.dps.registration_id;
    copts.keep_alive_seconds = 30;
    copts.connect_timeout_ms = 30000;

    /* Build DPS MQTT username via azure-sdk-for-c. */
    char dps_username[AZ_IOT_MQTT_USERNAME_BUF];
    size_t dps_username_len = 0;
    ar = az_iot_provisioning_client_get_user_name(
        &c->dps_prov, dps_username, sizeof(dps_username), &dps_username_len);
    if (az_result_failed(ar)) return AZ_IOT_ERR_INTERNAL;
    copts.username = dps_username;
    fprintf(stderr, "[conn] DPS username: %s\n", dps_username);

    /* Populate TLS from certificate_provider if available. DPS uses the bootstrap
     * identity; the operational cert (if any) is issued during this exchange. */
    if (c->opts.certificate_provider)
    {
        az_iot_certificate_material_t mat = {0};
        if (c->opts.certificate_provider->vtable->load(c->opts.certificate_provider, AZ_IOT_CRED_BOOTSTRAP, &mat) == AZ_IOT_OK)
        {
            copts.tls.trusted_ca_path   = mat.trusted_ca_path;
            copts.tls.client_cert_path  = mat.client_cert_path;
            copts.tls.client_key_path   = mat.client_key_path;
            copts.tls.client_key_password = mat.client_key_password;
            copts.tls.trusted_ca_pem    = mat.trusted_ca_pem;
            copts.tls.client_cert_pem   = mat.client_cert_pem;
            copts.tls.client_key_pem    = mat.client_key_pem;
            copts.tls.verify_server     = true;
            fprintf(stderr, "[conn] TLS ca=%s cert=%s key=%s\n",
                mat.trusted_ca_path ? mat.trusted_ca_path : "(null)",
                mat.client_cert_path ? mat.client_cert_path : "(null)",
                mat.client_key_path ? mat.client_key_path : "(null)");
            c->opts.certificate_provider->vtable->release(c->opts.certificate_provider, &mat);
        }
        else
        {
            fprintf(stderr, "[conn] certificate_provider load() failed\n");
        }
    }
    else
    {
        fprintf(stderr, "[conn] no certificate_provider set\n");
    }

    c->dps_mqtt = mc;
    c->dps_phase = DPS_PHASE_CONNECTING;
    c->dps_pending_finalize = false;
    c->dps_pending_have_assignment = false;
    c->dps_pending_status = AZ_IOT_OK;
    c->dps_enrolling = c->opts.dps.request_operational_certificate;

    transition(c, AZ_IOT_CONN_STATE_CONNECTING, AZ_IOT_OK);

    az_iot_result_t r = mc->iface->connect(mc, &copts);
    if (r != AZ_IOT_OK)
    {
        dps_teardown_mqtt(c);
        c->dps_phase = DPS_PHASE_NONE;
    }
    return r;
}

/* Process deferred DPS finalization. Called from _do_work() after process_loop.
 * On success, tears down DPS MQTT, sets host/client_id and starts hub connect.
 * On failure, transitions to FAULTED. */
static void dps_apply_deferred(az_iot_connection_client_t* c)
{
    if (!c->dps_pending_finalize) return;
    bool have_assignment = c->dps_pending_have_assignment;
    az_iot_result_t status = c->dps_pending_status;
    c->dps_pending_finalize = false;
    c->dps_pending_have_assignment = false;
    c->dps_pending_status = AZ_IOT_OK;

    /* Disconnect and destroy the DPS MQTT session. */
    if (c->dps_mqtt && c->dps_mqtt->iface && c->dps_mqtt->iface->disconnect)
        (void)c->dps_mqtt->iface->disconnect(c->dps_mqtt);
    dps_teardown_mqtt(c);
    c->dps_phase = DPS_PHASE_DONE;

    if (status != AZ_IOT_OK || !have_assignment)
    {
        transition(c, AZ_IOT_CONN_STATE_FAULTED, status);
        return;
    }

    /* Apply the assigned hub + device_id and connect to the hub. */
    az_iot_result_t r;
    r = replace_owned_string(&c->owned_host, &c->opts.host, c->dps_assigned_hub);
    if (r != AZ_IOT_OK) { transition(c, AZ_IOT_CONN_STATE_FAULTED, r); return; }
    r = replace_owned_string(&c->owned_client_id, &c->opts.client_id, c->dps_assigned_device_id);
    if (r != AZ_IOT_OK) { transition(c, AZ_IOT_CONN_STATE_FAULTED, r); return; }
    c->session_role = AZ_IOT_MQTT_ROLE_HUB_CLASSIC;
    c->dps_phase = DPS_PHASE_NONE;

    r = start_connect_attempt(c);
    if (r != AZ_IOT_OK)
    {
        transition(c, AZ_IOT_CONN_STATE_FAULTED, r);
    }
}

/* Inbound MQTT events are dispatched here (synchronously from process_loop). */
static void on_mqtt_event(const az_iot_mqtt_event_t* evt, void* user_ctx)
{
    az_iot_connection_client_t* c = (az_iot_connection_client_t*)user_ctx;
    if (!c || !evt) return;

    switch (evt->kind)
    {
        case AZ_IOT_MQTT_EVT_CONNECTED:
            if (evt->status == AZ_IOT_OK)
            {
                /* Successful CONNACK: clear the burst counter and announce. */
                c->reconnect_attempt = 0;
                c->reconnect_due_ms = 0;
                transition(c, AZ_IOT_CONN_STATE_CONNECTED, AZ_IOT_OK);
                /* Re-issue every persistent subscription so feature clients
                 * (DirectMethod, Twin) regain their inbound topic filters
                 * transparently across reconnects. SUBACK failures are
                 * absorbed for now; later phases may surface them. */
                if (c->active_client && c->active_client->iface)
                {
                    for (size_t i = 0; i < AZ_IOT_MAX_PERSISTENT_SUBS; ++i)
                    {
                        if (!c->persistent_subs[i].in_use) continue;
                        uint16_t pid = 0;
                        (void)c->active_client->iface->subscribe(
                            c->active_client,
                            c->persistent_subs[i].topic_filter,
                            c->persistent_subs[i].qos,
                            &pid);
                    }
                }
            }
            else
            {
                c->deferred = (reconnect_enabled(c) && !c->user_close) ? DEFER_RECONNECT : DEFER_FAULT;
                c->deferred_reason = evt->status;
            }
            break;

        case AZ_IOT_MQTT_EVT_DISCONNECTED:
            if (c->user_close || !reconnect_enabled(c))
            {
                c->deferred = DEFER_IDLE;
                c->deferred_reason = evt->status;
            }
            else
            {
                c->deferred = DEFER_RECONNECT;
                c->deferred_reason = (evt->status != AZ_IOT_OK) ? evt->status : AZ_IOT_ERR_NOT_CONNECTED;
            }
            break;

        case AZ_IOT_MQTT_EVT_ERROR:
        {
            az_iot_result_t r = (evt->status != AZ_IOT_OK) ? evt->status : AZ_IOT_ERR_MQTT;
            c->deferred = (reconnect_enabled(c) && !c->user_close) ? DEFER_RECONNECT : DEFER_FAULT;
            c->deferred_reason = r;
            break;
        }

        /* Phase 2.3: route inbound application messages through the
         * dispatch table; unmatched messages are dropped silently (the same
         * behaviour MQTT brokers rely on for unsubscribed wildcards). */
        case AZ_IOT_MQTT_EVT_MESSAGE:
            if (evt->message)
            {
                (void)az_iot_dispatch_route(&c->dispatch, evt->message);
            }
            break;

        /* Phase 3.1: route PUBACK to the publishing feature client via the
         * correlation table. Unmatched packet_ids are dropped (could be a
         * publish issued by a future feature without an ack callback). */
        case AZ_IOT_MQTT_EVT_PUBLISH_ACK:
            for (size_t i = 0; i < AZ_IOT_MAX_PENDING_PUBACKS; ++i)
            {
                if (c->pending_pubacks[i].in_use &&
                    c->pending_pubacks[i].packet_id == evt->packet_id)
                {
                    az_iot_publish_ack_cb cb = c->pending_pubacks[i].cb;
                    void* ctx = c->pending_pubacks[i].user_ctx;
                    c->pending_pubacks[i].in_use = false;
                    c->pending_pubacks[i].cb = NULL;
                    c->pending_pubacks[i].user_ctx = NULL;
                    if (cb) cb(evt->status, ctx);
                    break;
                }
            }
            break;

        /* SUBSCRIBE_ACK / UNSUBSCRIBE_ACK get correlation handlers in later
         * Phase 3 slices when feature clients need to know subscriptions are
         * live. For now we just absorb them. */
        case AZ_IOT_MQTT_EVT_SUBSCRIBE_ACK:
        case AZ_IOT_MQTT_EVT_UNSUBSCRIBE_ACK:
        default:
            break;
    }
}

static az_iot_result_t start_connect_attempt(az_iot_connection_client_t* c)
{
    az_iot_mqtt_version_t version =
        az_iot_mqtt_required_version_for_role(c->session_role);
    const az_iot_mqtt_factory_t* f = find_factory(c, version);
    if (!f) return AZ_IOT_ERR_NOT_SUPPORTED;

    az_iot_mqtt_client_t* mc = f->create(f->factory_ctx);
    if (!mc || !mc->iface) return AZ_IOT_ERR_INTERNAL;

    mc->iface->set_inbound_cb(mc, on_mqtt_event, c);

    az_iot_mqtt_connect_options_t copts = {0};
    copts.host = c->opts.host;
    copts.port = c->opts.port ? c->opts.port : (uint16_t)8883;
    copts.client_id = c->opts.client_id;
    copts.keep_alive_seconds = 30;
    copts.connect_timeout_ms = 30000;

    /* Build hub MQTT username via azure-sdk-for-c (Classic only).
     * Hub-Next does not use the Classic username format. */
    if (c->session_role != AZ_IOT_MQTT_ROLE_HUB_NEXT &&
        c->opts.host && c->opts.client_id)
    {
        if (!c->hub_client_initialized)
        {
            az_span host_span = az_span_create_from_str((char*)(uintptr_t)c->opts.host);
            az_span id_span = az_span_create_from_str((char*)(uintptr_t)c->opts.client_id);
            az_iot_hub_client_options hub_opts = az_iot_hub_client_options_default();
            if (c->opts.model_id && c->opts.model_id[0])
            {
                hub_opts.model_id =
                    az_span_create_from_str((char*)(uintptr_t)c->opts.model_id);
            }
            az_result ar = az_iot_hub_client_init(&c->hub_client, host_span, id_span, &hub_opts);
            if (az_result_succeeded(ar)) c->hub_client_initialized = true;
        }
        if (c->hub_client_initialized)
        {
            size_t ulen = 0;
            az_result ar = az_iot_hub_client_get_user_name(
                &c->hub_client, c->hub_username, sizeof(c->hub_username), &ulen);
            if (az_result_succeeded(ar)) copts.username = c->hub_username;
        }
    }

    /* Populate TLS from certificate_provider if available. Prefer the issued
     * OPERATIONAL identity (from this DPS session, or persisted by the provider
     * on a prior run, or supplied for a direct hub connection); fall back to the
     * BOOTSTRAP identity when the provider has no operational cert yet. */
    if (c->opts.certificate_provider)
    {
        az_iot_certificate_provider_t* prov = c->opts.certificate_provider;
        az_iot_certificate_material_t mat = {0};
        az_iot_result_t lr = prov->vtable->load(prov, AZ_IOT_CRED_OPERATIONAL, &mat);
        if (lr == AZ_IOT_ERR_NOT_FOUND || lr == AZ_IOT_ERR_NOT_INITIALIZED)
        {
            lr = prov->vtable->load(prov, AZ_IOT_CRED_BOOTSTRAP, &mat);
        }
        if (lr == AZ_IOT_OK)
        {
            copts.tls.trusted_ca_path   = mat.trusted_ca_path;
            copts.tls.client_cert_path  = mat.client_cert_path;
            copts.tls.client_key_path   = mat.client_key_path;
            copts.tls.client_key_password = mat.client_key_password;
            copts.tls.trusted_ca_pem    = mat.trusted_ca_pem;
            copts.tls.client_cert_pem   = mat.client_cert_pem;
            copts.tls.client_key_pem    = mat.client_key_pem;
            copts.tls.verify_server     = true;
            prov->vtable->release(prov, &mat);
        }
    }

    transition(c, AZ_IOT_CONN_STATE_CONNECTING, AZ_IOT_OK);
    az_iot_result_t r = mc->iface->connect(mc, &copts);
    if (r != AZ_IOT_OK)
    {
        mc->iface->destroy(mc);
        return r;
    }
    c->active_client = mc;
    return AZ_IOT_OK;
}

static void apply_deferred(az_iot_connection_client_t* c)
{
    if (c->deferred == DEFER_NONE) return;
    int action = c->deferred;
    az_iot_result_t reason = c->deferred_reason;
    c->deferred = DEFER_NONE;
    c->deferred_reason = AZ_IOT_OK;

    switch (action)
    {
        case DEFER_FAULT:
            teardown_active(c);
            transition(c, AZ_IOT_CONN_STATE_FAULTED, reason);
            break;
        case DEFER_RECONNECT:
            schedule_reconnect(c, reason);
            break;
        case DEFER_IDLE:
            teardown_active(c);
            c->user_close = false;
            c->reconnect_attempt = 0;
            c->reconnect_due_ms = 0;
            transition(c, AZ_IOT_CONN_STATE_IDLE, reason);
            break;
        default:
            break;
    }
}

/* ------------------------------------------------------------------------- */
/* Hub-Next mock bypass (env-var-driven, for local dev/test only)            */
/* ------------------------------------------------------------------------- */

/* When AZ_IOT_HUB_NEXT_MOCK_ENDPOINT is set (e.g. "localhost:8883"), skip DPS
 * entirely and connect to the mock Hub-Next using MQTT v5. The device identity
 * comes from AZ_IOT_DEVICE_ID (must match the cert CN in the mock). This
 * avoids the need for a real DPS service during local development. */
static bool mock_next_configured(void)
{
#ifdef _WIN32
    char* buf = NULL;
    size_t len = 0;
    if (_dupenv_s(&buf, &len, "AZ_IOT_HUB_NEXT_MOCK_ENDPOINT") != 0 || !buf || !buf[0])
    {
        free(buf);
        return false;
    }
    free(buf);
    return true;
#else
    const char* val = getenv("AZ_IOT_HUB_NEXT_MOCK_ENDPOINT");
    return val && val[0];
#endif
}

/* Parse "host:port" into host string and port. Writes host into out_host
 * (up to cap), returns port (default 8883 if not specified). */
static uint16_t parse_host_port(const char* endpoint, char* out_host, size_t cap)
{
    uint16_t port = 8883;
    const char* colon = strrchr(endpoint, ':');
    size_t host_len;
    if (colon && colon != endpoint)
    {
        host_len = (size_t)(colon - endpoint);
        unsigned long p = strtoul(colon + 1, NULL, 10);
        if (p > 0 && p <= 65535) port = (uint16_t)p;
    }
    else
    {
        host_len = strlen(endpoint);
    }
    if (host_len >= cap) host_len = cap - 1;
    memcpy(out_host, endpoint, host_len);
    out_host[host_len] = '\0';
    return port;
}

static az_iot_result_t apply_mock_next_bypass(az_iot_connection_client_t* c)
{
    const char* endpoint;
    const char* device_id;

#ifdef _WIN32
    char* ep_buf = NULL;
    char* id_buf = NULL;
    size_t ep_len = 0, id_len = 0;
    if (_dupenv_s(&ep_buf, &ep_len, "AZ_IOT_HUB_NEXT_MOCK_ENDPOINT") != 0 || !ep_buf)
        return AZ_IOT_ERR_INTERNAL;
    if (_dupenv_s(&id_buf, &id_len, "AZ_IOT_DEVICE_ID") != 0 || !id_buf || !id_buf[0])
    {
        /* Fall back to DPS registration_id if AZ_IOT_DEVICE_ID not set. */
        free(id_buf);
        id_buf = NULL;
    }
    endpoint = ep_buf;
    device_id = id_buf ? id_buf : c->opts.dps.registration_id;
#else
    endpoint = getenv("AZ_IOT_HUB_NEXT_MOCK_ENDPOINT");
    device_id = getenv("AZ_IOT_DEVICE_ID");
    if (!device_id || !device_id[0])
        device_id = c->opts.dps.registration_id;
#endif

    if (!endpoint || !endpoint[0] || !device_id || !device_id[0])
    {
#ifdef _WIN32
        free(ep_buf);
        free(id_buf);
#endif
        return AZ_IOT_ERR_INVALID_ARG;
    }

    char host[256];
    uint16_t port = parse_host_port(endpoint, host, sizeof(host));

    az_iot_result_t r;
    r = replace_owned_string(&c->owned_host, &c->opts.host, host);
    if (r != AZ_IOT_OK) goto done;
    c->opts.port = port;
    r = replace_owned_string(&c->owned_client_id, &c->opts.client_id, device_id);
    if (r != AZ_IOT_OK) goto done;

    c->session_role = AZ_IOT_MQTT_ROLE_HUB_NEXT;
    c->dps_phase = DPS_PHASE_DONE;

    fprintf(stderr, "[conn] Mock-Next bypass: host=%s port=%u device=%s\n",
            host, (unsigned)port, device_id);
    r = AZ_IOT_OK;

done:
#ifdef _WIN32
    free(ep_buf);
    free(id_buf);
#endif
    return r;
}

/* ------------------------------------------------------------------------- */
/* public API                                                                */
/* ------------------------------------------------------------------------- */

az_iot_connection_client_options_t az_iot_connection_client_options_get_default(
    const char* id_scope,
    const char* registration_id,
    az_iot_certificate_provider_t* certificate_provider)
{
    az_iot_connection_client_options_t opts = { 0 };
    opts.host = NULL;
    opts.port = 8883;
    opts.client_id = NULL;
    opts.certificate_provider = certificate_provider;
    opts.dps.global_endpoint = NULL;
    opts.dps.id_scope = id_scope;
    opts.dps.registration_id = registration_id;
    return opts;
}

az_iot_result_t az_iot_connection_client_init(
    az_iot_connection_client_t* client,
    const az_iot_connection_client_options_t* opts)
{
    if (!client || !opts)
    {
        AZ_IOT_LOG_ERROR("connection_client_init: invalid arguments");
        return AZ_IOT_ERR_INVALID_ARG;
    }
    memset(client, 0, sizeof(*client));
    client->opts = *opts;
    client->state = AZ_IOT_CONN_STATE_IDLE;
    /* Determine session role early so feature clients can query the profile
     * during their init (which happens before open()). When mock-next is
     * configured, also resolve the device_id so subscriptions can be built. */
    if (mock_next_configured())
    {
        client->session_role = AZ_IOT_MQTT_ROLE_HUB_NEXT;
        /* Resolve device_id from AZ_IOT_DEVICE_ID or DPS registration_id */
        const char* dev_id = NULL;
#ifdef _WIN32
        char* id_buf = NULL;
        size_t id_len = 0;
        if (_dupenv_s(&id_buf, &id_len, "AZ_IOT_DEVICE_ID") == 0 && id_buf && id_buf[0])
            dev_id = id_buf;
#else
        dev_id = getenv("AZ_IOT_DEVICE_ID");
#endif
        if (!dev_id || !dev_id[0])
            dev_id = client->opts.dps.registration_id;
        if (dev_id && dev_id[0])
        {
            (void)replace_owned_string(
                &client->owned_client_id, &client->opts.client_id, dev_id);
        }
#ifdef _WIN32
        free(id_buf);
#endif
    }
    else
    {
        client->session_role = AZ_IOT_MQTT_ROLE_HUB_CLASSIC;
    }
    /* Seed jitter PRNG; tests can overwrite via the internal seed entry point
     * if they need determinism. */
    client->rng_state = az_iot_time_mono_ms() ^ 0xA5A5C3C3DEADBEEFull;
    if (client->rng_state == 0) client->rng_state = 1ull;
    return AZ_IOT_OK;
}

void az_iot_connection_client_deinit(az_iot_connection_client_t* client)
{
    if (!client) return;
    teardown_active(client);
    dps_teardown_mqtt(client);
    /* dispatch is embedded; nothing to free. */
    for (size_t i = 0; i < client->factory_count; ++i)
    {
        if (client->factories[i].destroy)
            client->factories[i].destroy(client->factories[i].factory_ctx);
    }
    free(client->owned_host);
    free(client->owned_client_id);
}

az_iot_result_t az_iot_connection_client_register_mqtt_factory(
    az_iot_connection_client_t* client,
    const az_iot_mqtt_factory_t* factory)
{
    if (!client || !factory || !factory->create)
    {
        AZ_IOT_LOG_ERROR("register_mqtt_factory: invalid arguments");
        return AZ_IOT_ERR_INVALID_ARG;
    }

    if (client->factory_count >= AZ_IOT_MAX_MQTT_FACTORIES)
    {
        return AZ_IOT_ERR_NOT_SUPPORTED;
    }
    client->factories[client->factory_count++] = *factory;
    return AZ_IOT_OK;
}

az_iot_result_t az_iot_connection_client_set_state_callback(
    az_iot_connection_client_t* client,
    az_iot_connection_state_cb cb,
    void* user_ctx)
{
    if (!client) return AZ_IOT_ERR_INVALID_ARG;
    client->state_cb = cb;
    client->state_cb_ctx = user_ctx;
    return AZ_IOT_OK;
}

az_iot_result_t az_iot_connection_client_set_operational_cert_callback(
    az_iot_connection_client_t* client,
    az_iot_operational_cert_cb cb,
    void* user_ctx)
{
    if (!client) return AZ_IOT_ERR_INVALID_ARG;
    client->op_cert_cb = cb;
    client->op_cert_cb_ctx = user_ctx;
    return AZ_IOT_OK;
}

az_iot_result_t az_iot_connection_client_open(az_iot_connection_client_t* client)
{
    if (!client)
    {
        AZ_IOT_LOG_ERROR("connection_client_open: NULL client");
        return AZ_IOT_ERR_INVALID_ARG;
    }
    if (client->state != AZ_IOT_CONN_STATE_IDLE)
    {
        AZ_IOT_LOG_ERROR("connection_client_open: client not in IDLE state");
        return AZ_IOT_ERR_ALREADY_INITIALIZED;
    }

    /* CSR-based operational-cert enrollment (D2) requires a certificate_provider
     * whose vtable exposes get_csr (ABI version >= 2). Fail fast otherwise. */
    if (client->opts.dps.request_operational_certificate)
    {
        az_iot_certificate_provider_t* p = client->opts.certificate_provider;
        if (!p || p->vtable->version < 2u || p->vtable->get_csr == NULL)
        {
            AZ_IOT_LOG_ERROR("connection_client_open: request_operational_certificate set but provider does not support CSR enrollment");
            return AZ_IOT_ERR_NOT_SUPPORTED;
        }
    }

    client->user_close = false;
    client->reconnect_attempt = 0;
    client->reconnect_due_ms = 0;

    /* --- Mock-Next bypass: when AZ_IOT_HUB_NEXT_MOCK_ENDPOINT is set,
     * skip DPS and connect directly to the mock Hub-Next (MQTT v5). --- */
    if (mock_next_configured())
    {
        az_iot_result_t r = apply_mock_next_bypass(client);
        if (r != AZ_IOT_OK)
        {
            transition(client, AZ_IOT_CONN_STATE_IDLE, r);
            return r;
        }
        /* host + client_id are set, session_role = HUB_NEXT → fall through
         * to start_connect_attempt which will resolve the v5 factory. */
        r = start_connect_attempt(client);
        if (r != AZ_IOT_OK)
        {
            transition(client, AZ_IOT_CONN_STATE_IDLE, r);
        }
        return r;
    }

    /* When host is NULL but DPS is configured, provision first. */
    if (!client->opts.host && dps_configured(client))
    {
        az_iot_result_t r = dps_start(client);
        if (r != AZ_IOT_OK)
        {
            transition(client, AZ_IOT_CONN_STATE_IDLE, r);
        }
        return r;
    }

    if (!client->opts.host || !client->opts.client_id)
    {
        AZ_IOT_LOG_ERROR("connection_client_open: host or client_id not set (and DPS not configured)");
        return AZ_IOT_ERR_INVALID_ARG;
    }

    az_iot_result_t r = start_connect_attempt(client);
    if (r != AZ_IOT_OK)
    {
        transition(client, AZ_IOT_CONN_STATE_IDLE, r);
    }
    return r;
}

az_iot_result_t az_iot_connection_client_close(az_iot_connection_client_t* client)
{
    if (!client) return AZ_IOT_ERR_INVALID_ARG;
    if (client->state == AZ_IOT_CONN_STATE_IDLE)
    {
        return AZ_IOT_OK; /* idempotent */
    }

    /* Closing while waiting to reconnect: cancel the schedule and go straight
     * to IDLE. There is no live adapter to disconnect at this point. */
    if (client->state == AZ_IOT_CONN_STATE_RECONNECTING)
    {
        client->reconnect_attempt = 0;
        client->reconnect_due_ms = 0;
        client->user_close = false;
        transition(client, AZ_IOT_CONN_STATE_IDLE, AZ_IOT_OK);
        return AZ_IOT_OK;
    }

    if (!client->active_client) return AZ_IOT_ERR_NOT_INITIALIZED;

    client->user_close = true;
    transition(client, AZ_IOT_CONN_STATE_DISCONNECTING, AZ_IOT_OK);
    az_iot_result_t r = client->active_client->iface->disconnect(client->active_client);
    if (r != AZ_IOT_OK && r != AZ_IOT_ERR_NOT_CONNECTED)
    {
        return r;
    }
    return AZ_IOT_OK;
}

az_iot_result_t az_iot_connection_client_do_work(
    az_iot_connection_client_t* client, uint32_t timeout_ms)
{
    if (!client) return AZ_IOT_ERR_INVALID_ARG;

    /* --- DPS provisioning pump --- */
    if (client->dps_phase != DPS_PHASE_NONE && client->dps_phase != DPS_PHASE_DONE)
    {
        /* If polling deadline reached, issue query. */
        if (client->dps_phase == DPS_PHASE_POLLING &&
            az_iot_time_mono_ms() >= client->dps_poll_due_ms)
        {
            az_iot_result_t r = dps_do_query_publish(client);
            if (r != AZ_IOT_OK) dps_finalize(client, r, false);
        }

        az_iot_result_t r = AZ_IOT_OK;
        if (client->dps_mqtt && client->dps_mqtt->iface && client->dps_mqtt->iface->process_loop)
            r = client->dps_mqtt->iface->process_loop(client->dps_mqtt, timeout_ms);

        dps_apply_deferred(client);
        return r;
    }

    /* --- Normal hub session pump --- */
    az_iot_result_t r = AZ_IOT_OK;
    if (client->active_client)
    {
        r = client->active_client->iface->process_loop(client->active_client, timeout_ms);
    }

    apply_deferred(client);

    /* Fail an in-flight CSR renewal that never received a terminal response
     * (lost 200/error after a 202, or a hub that went silent) so the one-op
     * slot is not stuck BUSY for the life of the connection. */
    if (client->csr_op.in_use && az_iot_time_mono_ms() >= client->csr_op.deadline_ms)
    {
        az_iot_csr_cb cb = client->csr_op.cb;
        void* uc = client->csr_op.user_ctx;
        client->csr_op.in_use = false;
        az_iot_csr_event_t evt;
        memset(&evt, 0, sizeof(evt));
        evt.kind = AZ_IOT_CSR_FAILED;
        evt.status = AZ_IOT_ERR_TIMEOUT;
        if (cb) cb(&evt, uc);
    }

    /* If we're waiting to reconnect and the deadline has passed, attempt it. */
    if (client->state == AZ_IOT_CONN_STATE_RECONNECTING &&
        client->active_client == NULL &&
        az_iot_time_mono_ms() >= client->reconnect_due_ms)
    {
        az_iot_result_t cr = start_connect_attempt(client);
        if (cr != AZ_IOT_OK)
        {
            schedule_reconnect(client, cr);
        }
    }

    return r;
}

/* ------------------------------------------------------------------------- */
/* internal API                                                              */
/* ------------------------------------------------------------------------- */

az_iot_result_t az_iot_connection_client__set_session_role(
    az_iot_connection_client_t* client,
    az_iot_mqtt_role_t role)
{
    if (!client) return AZ_IOT_ERR_INVALID_ARG;
    if (client->state != AZ_IOT_CONN_STATE_IDLE) return AZ_IOT_ERR_ALREADY_INITIALIZED;
    client->session_role = role;
    return AZ_IOT_OK;
}

/* Internal helper used by both __set_host and __set_client_id. Duplicates `s`,
 * frees `*owned_slot`'s previous value, and writes the new pointer to both
 * `*owned_slot` (for ownership/free) and `*opts_slot` (the live pointer the
 * rest of the code reads). */
static az_iot_result_t replace_owned_string(
    char** owned_slot, const char** opts_slot, const char* s)
{
    if (!s || !s[0]) return AZ_IOT_ERR_INVALID_ARG;
    size_t n = strlen(s);
    char* dup = (char*)malloc(n + 1);
    if (!dup) return AZ_IOT_ERR_OUT_OF_MEMORY;
    memcpy(dup, s, n + 1);
    free(*owned_slot);
    *owned_slot = dup;
    *opts_slot = dup;
    return AZ_IOT_OK;
}

az_iot_result_t az_iot_connection_client__set_host(
    az_iot_connection_client_t* client, const char* host)
{
    if (!client) return AZ_IOT_ERR_INVALID_ARG;
    if (client->state != AZ_IOT_CONN_STATE_IDLE) return AZ_IOT_ERR_ALREADY_INITIALIZED;
    return replace_owned_string(&client->owned_host, &client->opts.host, host);
}

az_iot_result_t az_iot_connection_client__set_client_id(
    az_iot_connection_client_t* client, const char* client_id)
{
    if (!client) return AZ_IOT_ERR_INVALID_ARG;
    if (client->state != AZ_IOT_CONN_STATE_IDLE) return AZ_IOT_ERR_ALREADY_INITIALIZED;
    return replace_owned_string(&client->owned_client_id, &client->opts.client_id, client_id);
}

void az_iot_connection_client__seed_rng(
    az_iot_connection_client_t* client, uint64_t seed)
{
    if (!client) return;
    client->rng_state = seed ? seed : 1ull;
}

const az_iot_protocol_profile_t* az_iot_connection_client__profile(
    const az_iot_connection_client_t* client)
{
    if (!client) return NULL;
    return az_iot_protocol_profile_for_role(client->session_role);
}

az_iot_result_t az_iot_connection_client__register_inbound_handler(
    az_iot_connection_client_t* client,
    const char* topic_prefix,
    az_iot_inbound_handler_cb cb,
    void* user_ctx)
{
    if (!client) return AZ_IOT_ERR_INVALID_ARG;
    return az_iot_dispatch_register_prefix(&client->dispatch, topic_prefix, cb, user_ctx);
}

size_t az_iot_connection_client__unregister_inbound_handlers(
    az_iot_connection_client_t* client, void* user_ctx)
{
    if (!client) return 0;
    return az_iot_dispatch_unregister_by_ctx(&client->dispatch, user_ctx);
}

bool az_iot_connection_client__is_connected(
    const az_iot_connection_client_t* client)
{
    return client && client->state == AZ_IOT_CONN_STATE_CONNECTED;
}

const char* az_iot_connection_client__device_id(
    const az_iot_connection_client_t* client)
{
    return client ? client->opts.client_id : NULL;
}

az_iot_result_t az_iot_connection_client__publish(
    az_iot_connection_client_t* client,
    const az_iot_mqtt_message_t* msg,
    az_iot_publish_ack_cb ack_cb,
    void* ack_user_ctx)
{
    if (!client || !msg) return AZ_IOT_ERR_INVALID_ARG;
    if (!client->active_client || client->state != AZ_IOT_CONN_STATE_CONNECTED)
    {
        return AZ_IOT_ERR_NOT_CONNECTED;
    }

    uint16_t pid = 0;
    az_iot_result_t r = client->active_client->iface->publish(
        client->active_client, msg, &pid);
    if (r != AZ_IOT_OK) return r;

    /* QoS 0: there is no PUBACK on the wire. Fire the cb synchronously. */
    if (msg->qos == AZ_IOT_MQTT_QOS_0)
    {
        if (ack_cb) ack_cb(AZ_IOT_OK, ack_user_ctx);
        return AZ_IOT_OK;
    }

    /* QoS 1/2: register correlation entry. If no callback was requested, we
     * still succeeded; the future PUBACK will be silently absorbed. */
    if (ack_cb)
    {
        for (size_t i = 0; i < AZ_IOT_MAX_PENDING_PUBACKS; ++i)
        {
            if (!client->pending_pubacks[i].in_use)
            {
                client->pending_pubacks[i].packet_id = pid;
                client->pending_pubacks[i].cb = ack_cb;
                client->pending_pubacks[i].user_ctx = ack_user_ctx;
                client->pending_pubacks[i].in_use = true;
                return AZ_IOT_OK;
            }
        }
        /* Table full: the publish itself succeeded but we cannot deliver the
         * ack. Surface it so the caller can apply backpressure. */
        return AZ_IOT_ERR_NOT_SUPPORTED;
    }
    return AZ_IOT_OK;
}

az_iot_result_t az_iot_connection_client__subscribe(
    az_iot_connection_client_t* client,
    const char* topic_filter,
    az_iot_mqtt_qos_t qos,
    uint16_t* out_packet_id)
{
    if (!client || !topic_filter) return AZ_IOT_ERR_INVALID_ARG;
    if (!client->active_client || client->state != AZ_IOT_CONN_STATE_CONNECTED)
    {
        return AZ_IOT_ERR_NOT_CONNECTED;
    }
    return client->active_client->iface->subscribe(
        client->active_client, topic_filter, qos, out_packet_id);
}

az_iot_result_t az_iot_connection_client__add_subscription_on_connect(
    az_iot_connection_client_t* client,
    const char* topic_filter,
    az_iot_mqtt_qos_t qos)
{
    if (!client || !topic_filter) return AZ_IOT_ERR_INVALID_ARG;
    size_t n = strlen(topic_filter);
    if (n == 0 || n + 1 > AZ_IOT_PERSISTENT_SUB_TOPIC_MAX)
    {
        return AZ_IOT_ERR_INVALID_ARG;
    }
    /* Find a free slot. Duplicates are allowed but pointless; callers are
     * expected to register each filter once. */
    size_t slot = AZ_IOT_MAX_PERSISTENT_SUBS;
    for (size_t i = 0; i < AZ_IOT_MAX_PERSISTENT_SUBS; ++i)
    {
        if (!client->persistent_subs[i].in_use) { slot = i; break; }
    }
    if (slot == AZ_IOT_MAX_PERSISTENT_SUBS)
    {
        return AZ_IOT_ERR_NOT_SUPPORTED;
    }
    memcpy(client->persistent_subs[slot].topic_filter, topic_filter, n + 1);
    client->persistent_subs[slot].qos    = qos;
    client->persistent_subs[slot].in_use = true;

    /* If already CONNECTED, issue the SUBSCRIBE now so callers that register
     * after open() don't have to wait for the next reconnect. */
    if (client->active_client && client->state == AZ_IOT_CONN_STATE_CONNECTED)
    {
        uint16_t pid = 0;
        (void)client->active_client->iface->subscribe(
            client->active_client, topic_filter, qos, &pid);
    }
    return AZ_IOT_OK;
}

/* ------------------------------------------------------------------------- */
/* Runtime Hub-side certificate renewal ($iothub/credentials/...)            */
/* ------------------------------------------------------------------------- */

#define CSR_RES_PREFIX  "$iothub/credentials/res/"
#define CSR_RES_FILTER  "$iothub/credentials/res/#"
#define CSR_MAX_BASE64  8192   /* service cap: CSR <= 8 KB */
#define CSR_OP_TIMEOUT_MS 120000u /* give up on a renewal with no terminal response after 2 min */

static bool csr_is_base64(const char* s, size_t* out_len)
{
    size_t n = 0;
    for (; s[n]; ++n)
    {
        char ch = s[n];
        bool ok = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')
               || (ch >= '0' && ch <= '9') || ch == '+' || ch == '/' || ch == '=';
        if (!ok) return false;
    }
    *out_len = n;
    return n > 0;
}

static void csr_gen_request_id(az_iot_connection_client_t* c, char* buf, size_t cap)
{
    uint64_t x = az_iot_time_mono_ms()
               ^ (c->rng_state * 6364136223846793005ull + 1442695040888963407ull);
    c->rng_state = x;
    (void)snprintf(buf, cap, "%08x-%08x",
                   (unsigned)(x >> 32), (unsigned)(x & 0xffffffffu));
}

/* Parse errorCode + retryAfter (both optional) from an error response body. */
static void csr_parse_error(az_span payload, int32_t* out_code, int32_t* out_retry)
{
    *out_code = 0;
    *out_retry = 0;
    az_json_reader jr;
    if (az_result_failed(az_json_reader_init(&jr, payload, NULL))
        || az_result_failed(az_json_reader_next_token(&jr))
        || jr.token.kind != AZ_JSON_TOKEN_BEGIN_OBJECT)
    {
        return;
    }
    while (az_result_succeeded(az_json_reader_next_token(&jr))
           && jr.token.kind != AZ_JSON_TOKEN_END_OBJECT)
    {
        if (jr.token.kind != AZ_JSON_TOKEN_PROPERTY_NAME) continue;
        bool is_code  = az_json_token_is_text_equal(&jr.token, AZ_SPAN_FROM_STR("errorCode"));
        bool is_retry = az_json_token_is_text_equal(&jr.token, AZ_SPAN_FROM_STR("retryAfter"));
        if (az_result_failed(az_json_reader_next_token(&jr))) return;
        if (is_code && jr.token.kind == AZ_JSON_TOKEN_NUMBER)
        {
            if (az_result_failed(az_json_token_get_int32(&jr.token, out_code)))
                *out_code = 0;
        }
        else if (is_retry && jr.token.kind == AZ_JSON_TOKEN_NUMBER)
        {
            if (az_result_failed(az_json_token_get_int32(&jr.token, out_retry)))
                *out_retry = 0;
        }
        else if (jr.token.kind == AZ_JSON_TOKEN_BEGIN_OBJECT || jr.token.kind == AZ_JSON_TOKEN_BEGIN_ARRAY)
        {
            if (az_result_failed(az_json_reader_skip_children(&jr))) return;
        }
    }
}

/* Inbound handler for $iothub/credentials/res/{status}/?$rid={rid}. */
static void on_csr_response(void* user_ctx, const az_iot_mqtt_message_t* msg)
{
    az_iot_connection_client_t* c = (az_iot_connection_client_t*)user_ctx;
    if (!c || !msg || !msg->topic || !c->csr_op.in_use) return;

    /* Parse the status code and $rid from the topic. */
    const char* p = msg->topic + (sizeof(CSR_RES_PREFIX) - 1);
    int status = 0;
    while (*p >= '0' && *p <= '9') { status = status * 10 + (*p - '0'); ++p; }
    const char* rid = strstr(p, "$rid=");
    if (!rid) return;
    rid += 5;
    size_t rid_len = strcspn(rid, "&");
    if (rid_len != strlen(c->csr_op.request_id)
        || strncmp(rid, c->csr_op.request_id, rid_len) != 0)
    {
        return; /* response for a different request */
    }

    az_iot_csr_cb cb = c->csr_op.cb;
    void* uc = c->csr_op.user_ctx;
    az_span payload = az_span_create((uint8_t*)(uintptr_t)msg->payload, (int32_t)msg->payload_len);
    az_iot_csr_event_t evt;
    memset(&evt, 0, sizeof(evt));

    if (status == 202)
    {
        /* Accepted: signing in progress; keep the op open for the 200/error and
         * extend the deadline so a slow-but-alive signer is not timed out. */
        c->csr_op.deadline_ms = az_iot_time_mono_ms() + CSR_OP_TIMEOUT_MS;
        evt.kind = AZ_IOT_CSR_ACCEPTED;
        evt.status = AZ_IOT_OK;
        if (cb) cb(&evt, uc);
        return;
    }

    /* Terminal: the operation completes here regardless of outcome. */
    c->csr_op.in_use = false;

    if (status == 200)
    {
        enum { CSR_MAX_CHAIN = 6 };
        char* pem[CSR_MAX_CHAIN] = {0};
        size_t count = 0;
        az_iot_result_t rc = AZ_IOT_ERR_PROTOCOL;
        az_json_reader jr;
        if (az_result_succeeded(az_json_reader_init(&jr, payload, NULL))
            && az_result_succeeded(az_json_reader_next_token(&jr))
            && jr.token.kind == AZ_JSON_TOKEN_BEGIN_OBJECT)
        {
            bool in_arr = false;
            while (az_result_succeeded(az_json_reader_next_token(&jr))
                   && jr.token.kind != AZ_JSON_TOKEN_END_OBJECT)
            {
                if (jr.token.kind != AZ_JSON_TOKEN_PROPERTY_NAME) continue;
                bool m = az_json_token_is_text_equal(&jr.token, AZ_SPAN_FROM_STR("certificates"));
                if (az_result_failed(az_json_reader_next_token(&jr))) break;
                if (m && jr.token.kind == AZ_JSON_TOKEN_BEGIN_ARRAY) { in_arr = true; break; }
                if (jr.token.kind == AZ_JSON_TOKEN_BEGIN_OBJECT || jr.token.kind == AZ_JSON_TOKEN_BEGIN_ARRAY)
                    if (az_result_failed(az_json_reader_skip_children(&jr))) break;
            }
            if (in_arr) rc = json_collect_pem_chain(&jr, pem, CSR_MAX_CHAIN, &count);
        }

        if (rc == AZ_IOT_OK && count > 0)
        {
            az_iot_issued_certificate_t issued;
            issued.client_cert_chain_pem = (const char* const*)pem;
            issued.count = count;
            evt.kind = AZ_IOT_CSR_ISSUED;
            evt.status = AZ_IOT_OK;
            evt.issued = &issued;
            if (cb) cb(&evt, uc);
            for (size_t i = 0; i < count; ++i) free(pem[i]);
        }
        else
        {
            evt.kind = AZ_IOT_CSR_FAILED;
            evt.status = (rc == AZ_IOT_OK) ? AZ_IOT_ERR_PROTOCOL : rc;
            if (cb) cb(&evt, uc);
        }
        return;
    }

    /* Error response (4xx/5xx). */
    {
        int32_t code = 0, retry = 0;
        csr_parse_error(payload, &code, &retry);
        evt.kind = AZ_IOT_CSR_FAILED;
        evt.status = AZ_IOT_ERR_MQTT;
        evt.service_code = code;
        evt.retry_after_s = (uint32_t)(retry > 0 ? retry : 0);
        if (cb) cb(&evt, uc);
    }
}

az_iot_result_t az_iot_connection_client_send_csr(
    az_iot_connection_client_t* client,
    const az_iot_certificate_signing_request_t* csr,
    const char* request_id,
    const char* replace,
    az_iot_csr_cb cb,
    void* user_ctx)
{
    if (!client || !csr || !csr->csr_base64 || !cb)
        return AZ_IOT_ERR_INVALID_ARG;
    if (!az_iot_connection_client__is_connected(client))
        return AZ_IOT_ERR_NOT_CONNECTED;
    if (client->session_role != AZ_IOT_MQTT_ROLE_HUB_CLASSIC)
        return AZ_IOT_ERR_NOT_SUPPORTED;   /* Hub-Next (AEG) path not defined yet */
    if (client->csr_op.in_use)
        return AZ_IOT_ERR_BUSY;

    /* Validate the CSR: base64 and within the 8 KB service cap. */
    size_t csr_len = 0;
    if (!csr_is_base64(csr->csr_base64, &csr_len) || csr_len > CSR_MAX_BASE64)
        return AZ_IOT_ERR_INVALID_ARG;

    /* Request id: caller-provided (resubmit) or generated. */
    if (request_id && request_id[0])
    {
        size_t n = strlen(request_id);
        if (n + 1 > sizeof(client->csr_op.request_id)) return AZ_IOT_ERR_INVALID_ARG;
        memcpy(client->csr_op.request_id, request_id, n + 1);
    }
    else
    {
        csr_gen_request_id(client, client->csr_op.request_id, sizeof(client->csr_op.request_id));
    }

    /* Subscribe to the response topic + register the handler once. */
    if (!client->csr_op.subscribed)
    {
        az_iot_result_t r = az_iot_connection_client__register_inbound_handler(
            client, CSR_RES_PREFIX, on_csr_response, client);
        if (r != AZ_IOT_OK) return r;
        r = az_iot_connection_client__add_subscription_on_connect(
            client, CSR_RES_FILTER, AZ_IOT_MQTT_QOS_1);
        if (r != AZ_IOT_OK)
        {
            (void)az_iot_connection_client__unregister_inbound_handlers(client, client);
            return r;
        }
        client->csr_op.subscribed = true;
    }

    /* Build the request body: {"id":"<device>","csr":"<base64>"[,"replace":"..."]} */
    const char* device_id = az_iot_connection_client__device_id(client);
    if (!device_id) device_id = "";
    size_t body_cap = strlen(device_id) + csr_len + (replace ? strlen(replace) : 0) + 64;
    char* body = (char*)malloc(body_cap);
    if (!body) return AZ_IOT_ERR_OUT_OF_MEMORY;
    int bn = (replace && replace[0])
        ? snprintf(body, body_cap, "{\"id\":\"%s\",\"csr\":\"%s\",\"replace\":\"%s\"}",
                   device_id, csr->csr_base64, replace)
        : snprintf(body, body_cap, "{\"id\":\"%s\",\"csr\":\"%s\"}",
                   device_id, csr->csr_base64);
    if (bn < 0 || (size_t)bn >= body_cap) { free(body); return AZ_IOT_ERR_INTERNAL; }

    char topic[128];
    int tn = snprintf(topic, sizeof(topic),
                      "$iothub/credentials/POST/issueCertificate/?$rid=%s",
                      client->csr_op.request_id);
    if (tn < 0 || (size_t)tn >= sizeof(topic)) { free(body); return AZ_IOT_ERR_INTERNAL; }

    az_iot_mqtt_message_t msg = {0};
    msg.topic = topic;
    msg.payload = (const uint8_t*)body;
    msg.payload_len = (size_t)bn;
    msg.qos = AZ_IOT_MQTT_QOS_1;

    client->csr_op.cb = cb;
    client->csr_op.user_ctx = user_ctx;
    client->csr_op.in_use = true;
    client->csr_op.deadline_ms = az_iot_time_mono_ms() + CSR_OP_TIMEOUT_MS;

    az_iot_result_t r = az_iot_connection_client__publish(client, &msg, NULL, NULL);
    free(body);
    if (r != AZ_IOT_OK) { client->csr_op.in_use = false; return r; }
    return AZ_IOT_OK;
}

az_iot_result_t az_iot_connection_client_cancel_csr(az_iot_connection_client_t* client)
{
    if (!client) return AZ_IOT_ERR_INVALID_ARG;
    if (!client->csr_op.in_use) return AZ_IOT_ERR_NOT_FOUND;
    /* App-initiated abandon: drop the in-flight operation so a new send_csr()
     * can proceed. No callback fires (the caller already knows). A late hub
     * response for this rid is ignored (in_use is clear). */
    client->csr_op.in_use = false;
    return AZ_IOT_OK;
}

const char* az_iot_connection_state_to_string(az_iot_connection_state_t s)
{
    switch (s)
    {
        case AZ_IOT_CONN_STATE_IDLE:          return "AZ_IOT_CONN_STATE_IDLE";
        case AZ_IOT_CONN_STATE_CONNECTING:    return "AZ_IOT_CONN_STATE_CONNECTING";
        case AZ_IOT_CONN_STATE_CONNECTED:     return "AZ_IOT_CONN_STATE_CONNECTED";
        case AZ_IOT_CONN_STATE_RECONNECTING:  return "AZ_IOT_CONN_STATE_RECONNECTING";
        case AZ_IOT_CONN_STATE_DISCONNECTING: return "AZ_IOT_CONN_STATE_DISCONNECTING";
        case AZ_IOT_CONN_STATE_FAULTED:       return "AZ_IOT_CONN_STATE_FAULTED";
        default:                                  return "UNKNOWN";
    }
}
