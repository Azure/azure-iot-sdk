// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/* SPDX-License-Identifier: MIT */
#include "mock_mqtt_iface.h"

#include <stdlib.h>
#include <string.h>

#define AZ_IOT_MOCK_EVENT_QUEUE_MAX 32
#define AZ_IOT_MOCK_CALL_KIND_COUNT (AZ_IOT_MOCK_CALL_DESTROY + 1)

typedef struct queued_event
{
    az_iot_mqtt_event evt;
    /* Backing storage so the queued event survives until delivery. */
    az_iot_mqtt_message msg;
    char topic[AZ_IOT_MOCK_TOPIC_MAX];
    uint8_t payload[AZ_IOT_MOCK_PAYLOAD_MAX];
    size_t payload_len;
    bool has_message;
} queued_event;

struct az_iot_mock_mqtt_client
{
    /* MUST be first so a pointer to this struct is also a valid
     * az_iot_mqtt_client* (which only has `iface` as its public field). */
    az_iot_mqtt_client base;

    az_iot_mqtt_iface  iface_storage;

    az_iot_mqtt_event_callback inbound_cb;
    void*                    inbound_ctx;

    uint16_t next_packet_id;

    az_iot_mock_call calls[AZ_IOT_MOCK_HISTORY_MAX];
    size_t call_count;

    /* one-shot return code overrides, indexed by call kind. INT_MIN sentinel
     * is encoded as has_override[kind] == false. */
    bool         has_override[AZ_IOT_MOCK_CALL_KIND_COUNT];
    az_iot_result override_result[AZ_IOT_MOCK_CALL_KIND_COUNT];

    queued_event pending[AZ_IOT_MOCK_EVENT_QUEUE_MAX];
    size_t pending_head;
    size_t pending_count;

    /* Back-link to the factory so factory can null its last_client when we are
     * destroyed. */
    struct az_iot_mock_mqtt_factory_state* owner;
};

typedef struct az_iot_mock_mqtt_factory_state
{
    az_iot_mqtt_factory public_;
    az_iot_mock_mqtt_client* last_client;
} az_iot_mock_mqtt_factory_state;

/* ------------------------------------------------------------------------- */
/* helpers                                                                   */
/* ------------------------------------------------------------------------- */

static az_iot_mock_mqtt_client* mock_self(az_iot_mqtt_client* c)
{
    /* mock_mqtt_client_t starts with az_iot_mqtt_client base, so the cast
     * is safe for any client created by this mock. */
    return (az_iot_mock_mqtt_client*)c;
}

static az_iot_result take_override(az_iot_mock_mqtt_client* m, az_iot_mock_call_kind k)
{
    if (m->has_override[k])
    {
        m->has_override[k] = false;
        return m->override_result[k];
    }
    return AZ_IOT_OK;
}

static az_iot_mock_call* push_call(az_iot_mock_mqtt_client* m, az_iot_mock_call_kind k)
{
    az_iot_mock_call* slot;
    if (m->call_count < AZ_IOT_MOCK_HISTORY_MAX)
    {
        slot = &m->calls[m->call_count++];
    }
    else
    {
        /* drop oldest */
        memmove(&m->calls[0], &m->calls[1], (AZ_IOT_MOCK_HISTORY_MAX - 1) * sizeof(az_iot_mock_call));
        slot = &m->calls[AZ_IOT_MOCK_HISTORY_MAX - 1];
    }
    memset(slot, 0, sizeof(*slot));
    slot->kind = k;
    return slot;
}

static uint16_t alloc_packet_id(az_iot_mock_mqtt_client* m)
{
    if (++m->next_packet_id == 0)
    {
        m->next_packet_id = 1;
    }
    return m->next_packet_id;
}

static void copy_str(char* dst, size_t cap, const char* src)
{
    if (!src || !cap)
    {
        if (cap) dst[0] = '\0';
        return;
    }
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void copy_bytes(uint8_t* dst, size_t cap, const uint8_t* src, size_t n, size_t* out_n)
{
    if (n > cap) n = cap;
    if (n && src) memcpy(dst, src, n);
    *out_n = n;
}

/* ------------------------------------------------------------------------- */
/* vtable                                                                    */
/* ------------------------------------------------------------------------- */

static az_iot_result mock_connect(az_iot_mqtt_client* self, const az_iot_mqtt_connect_options* opts)
{
    az_iot_mock_mqtt_client* m = mock_self(self);
    az_iot_mock_call* c = push_call(m, AZ_IOT_MOCK_CALL_CONNECT);
    if (opts)
    {
        copy_str(c->topic, sizeof(c->topic), opts->host);
        copy_str(c->username, sizeof(c->username), opts->username);
    }
    return take_override(m, AZ_IOT_MOCK_CALL_CONNECT);
}

static az_iot_result mock_disconnect(az_iot_mqtt_client* self)
{
    az_iot_mock_mqtt_client* m = mock_self(self);
    push_call(m, AZ_IOT_MOCK_CALL_DISCONNECT);
    return take_override(m, AZ_IOT_MOCK_CALL_DISCONNECT);
}

static az_iot_result mock_subscribe(az_iot_mqtt_client* self, const char* topic_filter, az_iot_mqtt_qos qos, uint16_t* out_packet_id)
{
    az_iot_mock_mqtt_client* m = mock_self(self);
    az_iot_mock_call* c = push_call(m, AZ_IOT_MOCK_CALL_SUBSCRIBE);
    copy_str(c->topic, sizeof(c->topic), topic_filter);
    c->qos = qos;
    c->packet_id = alloc_packet_id(m);
    if (out_packet_id) *out_packet_id = c->packet_id;
    return take_override(m, AZ_IOT_MOCK_CALL_SUBSCRIBE);
}

static az_iot_result mock_unsubscribe(az_iot_mqtt_client* self, const char* topic_filter, uint16_t* out_packet_id)
{
    az_iot_mock_mqtt_client* m = mock_self(self);
    az_iot_mock_call* c = push_call(m, AZ_IOT_MOCK_CALL_UNSUBSCRIBE);
    copy_str(c->topic, sizeof(c->topic), topic_filter);
    c->packet_id = alloc_packet_id(m);
    if (out_packet_id) *out_packet_id = c->packet_id;
    return take_override(m, AZ_IOT_MOCK_CALL_UNSUBSCRIBE);
}

static az_iot_result mock_publish(az_iot_mqtt_client* self, const az_iot_mqtt_message* msg, uint16_t* out_packet_id)
{
    az_iot_mock_mqtt_client* m = mock_self(self);
    az_iot_mock_call* c = push_call(m, AZ_IOT_MOCK_CALL_PUBLISH);
    if (msg)
    {
        copy_str(c->topic, sizeof(c->topic), msg->topic);
        copy_bytes(c->payload, sizeof(c->payload), msg->payload, msg->payload_len, &c->payload_len);
        c->qos = msg->qos;
        c->retain = msg->retain;
        /* Capture the MQTT v5 correlation data + "type" property so presence/
         * birth tests can read the client's nonce and assert the message type. */
        copy_bytes(c->correlation_data, sizeof(c->correlation_data),
                   msg->correlation_data, msg->correlation_data_len, &c->correlation_data_len);
        for (size_t i = 0; i < msg->user_properties_count; ++i)
        {
            const az_iot_mqtt_user_property* up = &msg->user_properties[i];
            if (up->key && strcmp(up->key, "type") == 0)
            {
                copy_str(c->user_type, sizeof(c->user_type), up->value);
                break;
            }
        }
    }
    c->packet_id = alloc_packet_id(m);
    if (out_packet_id) *out_packet_id = c->packet_id;
    return take_override(m, AZ_IOT_MOCK_CALL_PUBLISH);
}

static az_iot_result mock_process_loop(az_iot_mqtt_client* self, uint32_t timeout_ms)
{
    az_iot_mock_mqtt_client* m = mock_self(self);
    az_iot_mock_call* c = push_call(m, AZ_IOT_MOCK_CALL_PROCESS_LOOP);
    c->timeout_ms = timeout_ms;

    /* deliver at most one queued event per call so the test can step deterministically. */
    if (m->pending_count > 0 && m->inbound_cb)
    {
        queued_event* q = &m->pending[m->pending_head];

        /* Re-bind message pointer to the queued backing storage in case the
         * caller's local az_iot_mqtt_message went out of scope. */
        if (q->has_message)
        {
            q->msg.topic = q->topic;
            q->msg.payload = q->payload_len ? q->payload : NULL;
            q->msg.payload_len = q->payload_len;
            q->evt.message = &q->msg;
        }

        m->inbound_cb(&q->evt, m->inbound_ctx);

        m->pending_head = (m->pending_head + 1) % AZ_IOT_MOCK_EVENT_QUEUE_MAX;
        m->pending_count--;
    }

    return take_override(m, AZ_IOT_MOCK_CALL_PROCESS_LOOP);
}

static void mock_set_inbound_cb(az_iot_mqtt_client* self, az_iot_mqtt_event_callback cb, void* user_ctx)
{
    az_iot_mock_mqtt_client* m = mock_self(self);
    m->inbound_cb = cb;
    m->inbound_ctx = user_ctx;
}

static void mock_destroy(az_iot_mqtt_client* self)
{
    if (!self) return;
    az_iot_mock_mqtt_client* m = mock_self(self);
    push_call(m, AZ_IOT_MOCK_CALL_DESTROY);
    /* Detach from owner's last_client cache so factory destroy doesn't double-free. */
    if (m->owner && m->owner->last_client == m)
    {
        m->owner->last_client = NULL;
    }
    free(m);
}

static const az_iot_mqtt_iface s_iface_template_v3 = {
    AZ_IOT_MQTT_VERSION_3_1_1,
    mock_connect, mock_disconnect, mock_subscribe, mock_unsubscribe,
    mock_publish, mock_process_loop, mock_set_inbound_cb, mock_destroy
};

static const az_iot_mqtt_iface s_iface_template_v5 = {
    AZ_IOT_MQTT_VERSION_5,
    mock_connect, mock_disconnect, mock_subscribe, mock_unsubscribe,
    mock_publish, mock_process_loop, mock_set_inbound_cb, mock_destroy
};

/* ------------------------------------------------------------------------- */
/* factory                                                                   */
/* ------------------------------------------------------------------------- */

static az_iot_mqtt_client* mock_factory_create(void* factory_ctx)
{
    az_iot_mock_mqtt_factory_state* st = (az_iot_mock_mqtt_factory_state*)factory_ctx;

    az_iot_mock_mqtt_client* m = (az_iot_mock_mqtt_client*)calloc(1, sizeof(*m));
    if (!m) return NULL;

    m->iface_storage = (st->public_.version == AZ_IOT_MQTT_VERSION_5) ? s_iface_template_v5 : s_iface_template_v3;
    m->base.iface = &m->iface_storage;
    m->next_packet_id = 0;
    m->owner = st;

    st->last_client = m;
    return &m->base;
}

static void mock_factory_cleanup(void* ctx) { free(ctx); }

az_iot_mqtt_factory* az_iot_mock_mqtt_factory_create(
    az_iot_mqtt_version version)
{
    az_iot_mock_mqtt_factory_state* st =
        (az_iot_mock_mqtt_factory_state*)calloc(1, sizeof(*st));
    if (!st) return NULL;
    st->public_.version = version;
    st->public_.create = mock_factory_create;
    st->public_.factory_ctx = st;
    st->public_.destroy = mock_factory_cleanup;
    return &st->public_;
}

void az_iot_mock_mqtt_factory_destroy(az_iot_mqtt_factory* factory)
{
    if (!factory) return;
    az_iot_mock_mqtt_factory_state* st =
        (az_iot_mock_mqtt_factory_state*)factory->factory_ctx;
    if (st->last_client)
    {
        mock_destroy(&st->last_client->base);
        st->last_client = NULL;
    }
    free(st);
}

az_iot_mock_mqtt_client* az_iot_mock_mqtt_factory_last_client(
    const az_iot_mqtt_factory* factory)
{
    if (!factory) return NULL;
    const az_iot_mock_mqtt_factory_state* st =
        (const az_iot_mock_mqtt_factory_state*)factory->factory_ctx;
    return st->last_client;
}

az_iot_mock_mqtt_client* az_iot_mock_mqtt_client_from(az_iot_mqtt_client* c)
{
    return mock_self(c);
}

/* ------------------------------------------------------------------------- */
/* introspection / scripting                                                 */
/* ------------------------------------------------------------------------- */

size_t az_iot_mock_mqtt_client_call_count(const az_iot_mock_mqtt_client* m)
{
    return m ? m->call_count : 0;
}

const az_iot_mock_call* az_iot_mock_mqtt_client_call_at(
    const az_iot_mock_mqtt_client* m, size_t i)
{
    if (!m || i >= m->call_count) return NULL;
    return &m->calls[i];
}

void az_iot_mock_mqtt_client_clear_calls(az_iot_mock_mqtt_client* m)
{
    if (!m) return;
    m->call_count = 0;
}

void az_iot_mock_mqtt_client_set_next_result(
    az_iot_mock_mqtt_client* m,
    az_iot_mock_call_kind kind,
    az_iot_result result)
{
    if (!m || kind >= AZ_IOT_MOCK_CALL_KIND_COUNT) return;
    m->has_override[kind] = true;
    m->override_result[kind] = result;
}

bool az_iot_mock_mqtt_client_inject_event(
    az_iot_mock_mqtt_client* m,
    const az_iot_mqtt_event* evt)
{
    if (!m || !evt) return false;
    if (m->pending_count >= AZ_IOT_MOCK_EVENT_QUEUE_MAX) return false;

    size_t tail = (m->pending_head + m->pending_count) % AZ_IOT_MOCK_EVENT_QUEUE_MAX;
    queued_event* q = &m->pending[tail];
    memset(q, 0, sizeof(*q));
    q->evt = *evt;
    q->evt.message = NULL;
    q->has_message = false;

    if (evt->kind == AZ_IOT_MQTT_EVT_MESSAGE && evt->message)
    {
        q->has_message = true;
        copy_str(q->topic, sizeof(q->topic), evt->message->topic);
        copy_bytes(q->payload, sizeof(q->payload), evt->message->payload, evt->message->payload_len, &q->payload_len);
        q->msg = *evt->message;
        q->msg.topic = q->topic;
        q->msg.payload = q->payload_len ? q->payload : NULL;
        q->msg.payload_len = q->payload_len;
        q->evt.message = &q->msg;
    }

    m->pending_count++;
    return true;
}

bool az_iot_mock_mqtt_client_inject_connected(
    az_iot_mock_mqtt_client* m,
    az_iot_result status)
{
    az_iot_mqtt_event evt;
    memset(&evt, 0, sizeof(evt));
    evt.kind = AZ_IOT_MQTT_EVT_CONNECTED;
    evt.status = status;
    return az_iot_mock_mqtt_client_inject_event(m, &evt);
}

bool az_iot_mock_mqtt_client_inject_message(
    az_iot_mock_mqtt_client* m,
    const char* topic,
    const uint8_t* payload,
    size_t payload_len,
    az_iot_mqtt_qos qos)
{
    az_iot_mqtt_event evt;
    az_iot_mqtt_message msg;
    memset(&evt, 0, sizeof(evt));
    memset(&msg, 0, sizeof(msg));
    msg.topic = topic;
    msg.payload = payload;
    msg.payload_len = payload_len;
    msg.qos = qos;
    evt.kind = AZ_IOT_MQTT_EVT_MESSAGE;
    evt.message = &msg;
    return az_iot_mock_mqtt_client_inject_event(m, &evt);
}
