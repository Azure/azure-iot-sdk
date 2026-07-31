// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/* SPDX-License-Identifier: MIT */
#include "e2e_amqp.h"

#include "az_amqp_sample_wait.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* GCC flags these AZ_NODISCARD (warn_unused_result) az_amqp calls under -Werror even
   with a plain (void) cast, which GCC deliberately ignores. Route the intentional
   best-effort/teardown discards through an assignment so the result is observed. */
#define E2E_AMQP_DISCARD(expr)             \
    do                                     \
    {                                      \
        az_result _e2e_discard_r = (expr); \
        (void)_e2e_discard_r;              \
    } while (0)

/* --- shared callbacks / pump ---------------------------------------------- */

typedef struct put_token_result
{
    bool done;
    uint32_t status;
} put_token_result;

static void on_connection_state_changed(
    az_amqp_connection* connection,
    az_amqp_connection_state previous_state,
    az_amqp_connection_state current_state,
    az_amqp_error_detail const* error,
    void* user_data)
{
    (void)connection;
    (void)previous_state;
    if (current_state == AZ_AMQP_CONNECTION_STATE_ERROR)
    {
        if (error != NULL)
        {
            fprintf(
                stderr,
                "[e2e amqp] connection error: code=0x%08x transport_status=%d condition='%.*s' "
                "description='%.*s' message='%.*s'\n",
                (unsigned)error->code,
                (int)error->transport_status,
                (int)az_span_size(error->amqp.condition),
                (const char*)az_span_ptr(error->amqp.condition),
                (int)az_span_size(error->amqp.description),
                (const char*)az_span_ptr(error->amqp.description),
                (int)az_span_size(error->message),
                (const char*)az_span_ptr(error->message));
        }
        *(bool*)user_data = true; /* user_data points at a connection_failed flag */
    }
}

static void on_put_token_complete(
    az_amqp_cbs* cbs,
    uint32_t status_code,
    az_span status_description,
    void* user_data)
{
    (void)cbs;
    (void)status_description;
    put_token_result* result = (put_token_result*)user_data;
    result->status = status_code;
    result->done = true;
}

/* Pump a connection once, waiting up to @p wait_ms for socket I/O. */
static bool pump_connection(
    az_amqp_connection* connection,
    az_amqp_sample_transport* transport,
    bool* connection_failed,
    int32_t wait_ms)
{
    az_amqp_connection_process_result io;
    if (az_result_failed(az_amqp_connection_process(connection, &io)) || *connection_failed)
    {
        return false;
    }
    int32_t wait = (int32_t)io.next_activity_milliseconds;
    if (wait < 0 || wait > wait_ms)
    {
        wait = wait_ms;
    }
    az_amqp_sample_wait_for_io(transport, io.io_interest, wait);
    return true;
}

/* --- telemetry receiver --------------------------------------------------- */

static void on_message_received(
    az_amqp_link* link,
    az_amqp_message const* message,
    az_amqp_delivery const* delivery,
    void* user_data)
{
    e2e_amqp_telemetry* t = (e2e_amqp_telemetry*)user_data;

    az_amqp_message_body_kind body_kind;
    az_span body;
    if (az_result_succeeded(az_amqp_message_get_body(message, &body_kind, &body))
        && body_kind == AZ_AMQP_MESSAGE_BODY_KIND_DATA
        && t->captured_count < E2E_AMQP_CAPTURE_MAX)
    {
        int n = az_span_size(body);
        if (n > E2E_AMQP_CAPTURE_BODY_MAX - 1)
        {
            n = E2E_AMQP_CAPTURE_BODY_MAX - 1;
        }
        memcpy(t->captured[t->captured_count], az_span_ptr(body), (size_t)n);
        t->captured[t->captured_count][n] = '\0';
        t->captured_count++;
    }

    E2E_AMQP_DISCARD(az_amqp_link_accept(link, delivery->number));
}

bool e2e_amqp_telemetry_begin(
    e2e_amqp_telemetry* t,
    const char* eh_host,
    const char* entity_path,
    const char* sas_token,
    int partition_count,
    const char** err_out)
{
    const char* err = NULL;
    if (partition_count < 1)
    {
        partition_count = 1;
    }
    if (partition_count > E2E_AMQP_MAX_PARTITIONS)
    {
        partition_count = E2E_AMQP_MAX_PARTITIONS;
    }
    t->partition_count = partition_count;

    az_span fqdn = az_span_create_from_str((char*)(uintptr_t)eh_host);
    az_span token = az_span_create_from_str((char*)(uintptr_t)sas_token);

    int audience_length = snprintf(
        t->audience_buffer, sizeof(t->audience_buffer), "amqps://%s/%s", eh_host, entity_path);
    az_span audience = az_span_create((uint8_t*)t->audience_buffer, audience_length);

    /* 1. TLS transport to the AMQPS port. */
    az_amqp_transport_options transport_options = { 0 };
    transport_options.host_name = fqdn;
    transport_options.port = AZ_AMQP_PORT_AMQPS;
    transport_options.tls_enabled = true;
    if (az_result_failed(
            az_amqp_sample_transport_init(&t->transport, &t->transport_storage, &transport_options)))
    {
        err = "telemetry: transport init failed";
        goto error;
    }

    /* 2. Connection with SASL ANONYMOUS (Event Hubs authorizes per-entity via CBS). */
    t->session_slots[0] = NULL;
    az_amqp_connection_storage connection_storage = {
        .incoming_buffer = AZ_SPAN_FROM_BUFFER(t->incoming_buffer),
        .outgoing_buffer = AZ_SPAN_FROM_BUFFER(t->outgoing_buffer),
        .sessions = t->session_slots,
        .sessions_capacity = 1,
    };
    az_amqp_connection_options connection_options = az_amqp_connection_options_default();
    connection_options.container_id = AZ_SPAN_FROM_STR("az-iot-e2e-telemetry");
    connection_options.hostname = fqdn;
    connection_options.idle_timeout_milliseconds = 240000;
    connection_options.sasl.mechanism = AZ_AMQP_SASL_MECHANISM_ANONYMOUS;

    if (az_result_failed(az_amqp_connection_init(
            &t->connection, &t->transport, &connection_storage, &connection_options)))
    {
        err = "telemetry: connection init failed";
        goto error;
    }
    az_amqp_connection_set_state_callback(
        &t->connection, on_connection_state_changed, &t->connection_failed);

    if (az_result_failed(az_amqp_connection_open(&t->connection)))
    {
        err = "telemetry: connection open failed";
        goto error;
    }
    while (az_amqp_connection_get_state(&t->connection) == AZ_AMQP_CONNECTION_STATE_OPENING)
    {
        if (!pump_connection(&t->connection, &t->transport_storage, &t->connection_failed, 500))
        {
            err = "telemetry: connection failed during open";
            goto error;
        }
    }

    /* 3. Session sized for the CBS pair + one receiver per partition. */
    az_amqp_session_storage session_storage
        = { .links = t->link_slots, .links_capacity = partition_count + 2 };
    if (az_result_failed(az_amqp_session_init(&t->session, &t->connection, &session_storage, NULL))
        || az_result_failed(az_amqp_session_begin(&t->session)))
    {
        err = "telemetry: session begin failed";
        goto error;
    }
    while (az_amqp_session_get_state(&t->session) == AZ_AMQP_SESSION_STATE_BEGINNING)
    {
        if (!pump_connection(&t->connection, &t->transport_storage, &t->connection_failed, 500))
        {
            err = "telemetry: connection failed during session begin";
            goto error;
        }
    }

    /* 4. CBS authorize the Event Hub-compatible entity. */
    az_amqp_cbs_options cbs_options = az_amqp_cbs_options_default();
    cbs_options.reply_buffer = AZ_SPAN_FROM_BUFFER(t->cbs_reply_buffer);
    if (az_result_failed(az_amqp_cbs_init(&t->cbs, &t->session, &cbs_options))
        || az_result_failed(az_amqp_cbs_open(&t->cbs)))
    {
        err = "telemetry: cbs open failed";
        goto error;
    }
    while (az_amqp_cbs_get_state(&t->cbs) == AZ_AMQP_CBS_STATE_OPENING)
    {
        if (!pump_connection(&t->connection, &t->transport_storage, &t->connection_failed, 500))
        {
            err = "telemetry: connection failed during cbs open";
            goto error;
        }
    }

    put_token_result put_token = { 0 };
    if (az_result_failed(az_amqp_cbs_put_token(
            &t->cbs,
            AZ_SPAN_FROM_STR(AZ_AMQP_CBS_TOKEN_TYPE_SAS),
            audience,
            token,
            0,
            on_put_token_complete,
            &put_token)))
    {
        err = "telemetry: put-token request failed to queue";
        goto error;
    }
    while (!put_token.done)
    {
        if (!pump_connection(&t->connection, &t->transport_storage, &t->connection_failed, 500))
        {
            err = "telemetry: connection failed during cbs authorization";
            goto error;
        }
    }
    if (put_token.status / 100 != 2)
    {
        err = "telemetry: cbs authorization rejected";
        goto error;
    }

    /* 5. One earliest-position receiver per partition. */
    for (int p = 0; p < partition_count; p++)
    {
        int source_length = snprintf(
            t->source_addr[p],
            sizeof(t->source_addr[p]),
            "%s/ConsumerGroups/$Default/Partitions/%d",
            entity_path,
            p);
        int name_length = snprintf(t->link_name[p], sizeof(t->link_name[p]), "e2e-recv-%d", p);

        az_amqp_link_options receiver_options = az_amqp_link_receiver_options_default(
            az_span_create((uint8_t*)t->link_name[p], name_length),
            az_amqp_source_from_address(
                az_span_create((uint8_t*)t->source_addr[p], source_length)),
            AZ_AMQP_RECEIVER_SETTLE_MODE_FIRST,
            AZ_SPAN_FROM_BUFFER(t->recv_buffers[p]),
            50 /* prefetch credit */);

        if (az_result_failed(az_amqp_link_init(&t->receivers[p], &t->session, &receiver_options)))
        {
            err = "telemetry: receiver init failed";
            goto error;
        }
        az_amqp_link_set_message_callback(&t->receivers[p], on_message_received, t);
        if (az_result_failed(az_amqp_link_attach(&t->receivers[p])))
        {
            err = "telemetry: receiver attach failed";
            goto error;
        }
    }

    t->started = true;
    return true;

error:
    if (err_out != NULL)
    {
        *err_out = err;
    }
    e2e_amqp_telemetry_end(t);
    return false;
}

bool e2e_amqp_telemetry_do_work(e2e_amqp_telemetry* t, int wait_ms)
{
    if (!t->started)
    {
        return false;
    }
    return pump_connection(&t->connection, &t->transport_storage, &t->connection_failed, wait_ms);
}

bool e2e_amqp_telemetry_seen(const e2e_amqp_telemetry* t, const char* needle)
{
    for (int i = 0; i < t->captured_count; i++)
    {
        if (strstr(t->captured[i], needle) != NULL)
        {
            return true;
        }
    }
    return false;
}

void e2e_amqp_telemetry_end(e2e_amqp_telemetry* t)
{
    if (t->started)
    {
        t->started = false;

        for (int p = 0; p < t->partition_count; p++)
        {
            E2E_AMQP_DISCARD(az_amqp_link_detach(&t->receivers[p], NULL));
        }
        E2E_AMQP_DISCARD(az_amqp_cbs_close(&t->cbs));
        E2E_AMQP_DISCARD(az_amqp_session_end(&t->session, NULL));
        E2E_AMQP_DISCARD(az_amqp_connection_close(&t->connection, NULL));

        for (int i = 0; i < 40; i++)
        {
            az_amqp_connection_state state = az_amqp_connection_get_state(&t->connection);
            if (state == AZ_AMQP_CONNECTION_STATE_CLOSED || state == AZ_AMQP_CONNECTION_STATE_ERROR)
            {
                break;
            }
            if (!pump_connection(&t->connection, &t->transport_storage, &t->connection_failed, 100))
            {
                break;
            }
        }
    }

    /* Always release the transport's TLS slot, even if we failed BEFORE fully
     * starting (t->started stays false until the very end of _begin). On Windows
     * the Schannel adapter uses a single global slot; a failed attempt that left
     * the slot open would force every subsequent retry -- and the c2d/method/twin
     * connections -- to reuse the dead session instead of performing a fresh TLS
     * handshake. az_amqp_connection_close tears down the AMQP session but leaves
     * the caller-owned transport open, so close it here explicitly. */
    if (t->transport.vtable != NULL && t->transport.vtable->close != NULL)
    {
        (void)t->transport.vtable->close(&t->transport);
    }
}

/* --- cloud-to-device sender ----------------------------------------------- */

typedef struct e2e_amqp_c2d_ctx
{
    az_amqp_sample_transport transport_storage;
    az_amqp_transport transport;
    az_amqp_connection connection;
    az_amqp_session session;
    az_amqp_cbs cbs;
    az_amqp_link sender;
    bool connection_failed;

    uint8_t incoming_buffer[AZ_AMQP_DEFAULT_MAX_FRAME_SIZE];
    uint8_t outgoing_buffer[AZ_AMQP_DEFAULT_MAX_FRAME_SIZE];
    az_amqp_session* session_slots[1];
    az_amqp_link* link_slots[3]; /* CBS pair (2) + c2d sender (1) */
    uint8_t cbs_reply_buffer[1024];
    az_amqp_link_unsettled unsettled[4];
    char audience_buffer[256];
    char to_buffer[256];
} e2e_amqp_c2d_ctx;

typedef struct send_result
{
    bool done;
    az_amqp_delivery_outcome outcome;
} send_result;

static void on_send_complete(
    az_amqp_link* link,
    az_span delivery_tag,
    az_amqp_delivery_state const* delivery_state,
    void* user_data)
{
    (void)link;
    (void)delivery_tag;
    send_result* result = (send_result*)user_data;
    result->outcome = delivery_state->outcome;
    result->done = true;
}

bool e2e_amqp_send_c2d(
    const char* hub_host,
    const char* sas_token,
    const char* device_id,
    const uint8_t* payload,
    size_t payload_len,
    const char** err_out)
{
    const char* err = NULL;
    e2e_amqp_c2d_ctx* c = (e2e_amqp_c2d_ctx*)calloc(1, sizeof(*c));
    if (c == NULL)
    {
        if (err_out != NULL)
        {
            *err_out = "c2d: out of memory";
        }
        return false;
    }

    bool ok = false;
    bool connection_ok = false;
    bool session_ok = false;
    bool cbs_ok = false;
    bool sender_ok = false;
    az_span fqdn = az_span_create_from_str((char*)(uintptr_t)hub_host);
    az_span token = az_span_create_from_str((char*)(uintptr_t)sas_token);

    /* CBS audience for the IoT Hub service endpoint is the hub host. */
    int audience_length
        = snprintf(c->audience_buffer, sizeof(c->audience_buffer), "%s", hub_host);
    az_span audience = az_span_create((uint8_t*)c->audience_buffer, audience_length);

    /* 1. TLS transport. */
    az_amqp_transport_options transport_options = { 0 };
    transport_options.host_name = fqdn;
    transport_options.port = AZ_AMQP_PORT_AMQPS;
    transport_options.tls_enabled = true;
    if (az_result_failed(
            az_amqp_sample_transport_init(&c->transport, &c->transport_storage, &transport_options)))
    {
        err = "c2d: transport init failed";
        goto cleanup;
    }

    /* 2. Connection with SASL ANONYMOUS. */
    az_amqp_connection_storage connection_storage = {
        .incoming_buffer = AZ_SPAN_FROM_BUFFER(c->incoming_buffer),
        .outgoing_buffer = AZ_SPAN_FROM_BUFFER(c->outgoing_buffer),
        .sessions = c->session_slots,
        .sessions_capacity = 1,
    };
    az_amqp_connection_options connection_options = az_amqp_connection_options_default();
    connection_options.container_id = AZ_SPAN_FROM_STR("az-iot-e2e-c2d");
    connection_options.hostname = fqdn;
    connection_options.idle_timeout_milliseconds = 240000;
    connection_options.sasl.mechanism = AZ_AMQP_SASL_MECHANISM_ANONYMOUS;
    if (az_result_failed(az_amqp_connection_init(
            &c->connection, &c->transport, &connection_storage, &connection_options)))
    {
        err = "c2d: connection init failed";
        goto cleanup;
    }
    connection_ok = true;
    az_amqp_connection_set_state_callback(
        &c->connection, on_connection_state_changed, &c->connection_failed);
    if (az_result_failed(az_amqp_connection_open(&c->connection)))
    {
        err = "c2d: connection open failed";
        goto cleanup;
    }
    while (az_amqp_connection_get_state(&c->connection) == AZ_AMQP_CONNECTION_STATE_OPENING)
    {
        if (!pump_connection(&c->connection, &c->transport_storage, &c->connection_failed, 500))
        {
            err = "c2d: connection failed during open";
            goto cleanup;
        }
    }

    /* 3. Session (CBS pair + sender). */
    az_amqp_session_storage session_storage
        = { .links = c->link_slots, .links_capacity = 3 };
    if (az_result_failed(az_amqp_session_init(&c->session, &c->connection, &session_storage, NULL))
        || az_result_failed(az_amqp_session_begin(&c->session)))
    {
        err = "c2d: session begin failed";
        goto cleanup;
    }
    session_ok = true;
    while (az_amqp_session_get_state(&c->session) == AZ_AMQP_SESSION_STATE_BEGINNING)
    {
        if (!pump_connection(&c->connection, &c->transport_storage, &c->connection_failed, 500))
        {
            err = "c2d: connection failed during session begin";
            goto cleanup;
        }
    }

    /* 4. CBS authorize the hub host. */
    az_amqp_cbs_options cbs_options = az_amqp_cbs_options_default();
    cbs_options.reply_buffer = AZ_SPAN_FROM_BUFFER(c->cbs_reply_buffer);
    if (az_result_failed(az_amqp_cbs_init(&c->cbs, &c->session, &cbs_options))
        || az_result_failed(az_amqp_cbs_open(&c->cbs)))
    {
        err = "c2d: cbs open failed";
        goto cleanup;
    }
    cbs_ok = true;
    while (az_amqp_cbs_get_state(&c->cbs) == AZ_AMQP_CBS_STATE_OPENING)
    {
        if (!pump_connection(&c->connection, &c->transport_storage, &c->connection_failed, 500))
        {
            err = "c2d: connection failed during cbs open";
            goto cleanup;
        }
    }
    put_token_result put_token = { 0 };
    if (az_result_failed(az_amqp_cbs_put_token(
            &c->cbs,
            AZ_SPAN_FROM_STR(AZ_AMQP_CBS_TOKEN_TYPE_SAS),
            audience,
            token,
            0,
            on_put_token_complete,
            &put_token)))
    {
        err = "c2d: put-token request failed to queue";
        goto cleanup;
    }
    while (!put_token.done)
    {
        if (!pump_connection(&c->connection, &c->transport_storage, &c->connection_failed, 500))
        {
            err = "c2d: connection failed during cbs authorization";
            goto cleanup;
        }
    }
    if (put_token.status / 100 != 2)
    {
        err = "c2d: cbs authorization rejected";
        goto cleanup;
    }

    /* 5. Sender link to the service C2D node. */
    az_amqp_link_options sender_options = az_amqp_link_sender_options_default(
        AZ_SPAN_FROM_STR("e2e-c2d-sender"),
        az_amqp_target_from_address(AZ_SPAN_FROM_STR("/messages/devicebound")),
        AZ_AMQP_SENDER_SETTLE_MODE_UNSETTLED,
        c->unsettled,
        (int32_t)(sizeof(c->unsettled) / sizeof(c->unsettled[0])));
    if (az_result_failed(az_amqp_link_init(&c->sender, &c->session, &sender_options))
        || az_result_failed(az_amqp_link_attach(&c->sender)))
    {
        err = "c2d: sender attach failed";
        goto cleanup;
    }
    sender_ok = true;
    while (az_amqp_link_get_state(&c->sender) == AZ_AMQP_LINK_STATE_ATTACHING
           || az_amqp_link_get_credit(&c->sender) == 0)
    {
        if (!pump_connection(&c->connection, &c->transport_storage, &c->connection_failed, 500))
        {
            err = "c2d: connection failed during sender attach";
            goto cleanup;
        }
    }

    /* 6. Build and send the message addressed to the target device. */
    int to_length = snprintf(
        c->to_buffer, sizeof(c->to_buffer), "/devices/%s/messages/devicebound", device_id);

    az_amqp_message message;
    E2E_AMQP_DISCARD(az_amqp_message_init(&message));
    az_amqp_message_properties properties = { 0 };
    properties.to = az_span_create((uint8_t*)c->to_buffer, to_length);
    properties.content_type = AZ_SPAN_FROM_STR("application/octet-stream");
    E2E_AMQP_DISCARD(az_amqp_message_set_properties(&message, &properties));
    E2E_AMQP_DISCARD(az_amqp_message_set_body_data(
        &message, az_span_create((uint8_t*)(uintptr_t)payload, (int32_t)payload_len)));

    send_result send = { 0 };
    uint8_t delivery_tag_bytes[] = { 0x00, 0x00, 0x00, 0x01 };
    if (az_result_failed(az_amqp_link_send(
            &c->sender, &message, AZ_SPAN_FROM_BUFFER(delivery_tag_bytes), on_send_complete, &send)))
    {
        err = "c2d: send failed to queue";
        goto cleanup;
    }
    while (!send.done)
    {
        if (!pump_connection(&c->connection, &c->transport_storage, &c->connection_failed, 500))
        {
            err = "c2d: connection failed during send";
            goto cleanup;
        }
    }
    if (send.outcome != AZ_AMQP_DELIVERY_OUTCOME_ACCEPTED)
    {
        err = "c2d: message not accepted by IoT Hub";
        goto cleanup;
    }
    ok = true;

cleanup:
    if (sender_ok)
    {
        E2E_AMQP_DISCARD(az_amqp_link_detach(&c->sender, NULL));
    }
    if (cbs_ok)
    {
        E2E_AMQP_DISCARD(az_amqp_cbs_close(&c->cbs));
    }
    if (session_ok)
    {
        E2E_AMQP_DISCARD(az_amqp_session_end(&c->session, NULL));
    }
    if (connection_ok)
    {
        E2E_AMQP_DISCARD(az_amqp_connection_close(&c->connection, NULL));
        for (int i = 0; i < 40; i++)
        {
            az_amqp_connection_state state = az_amqp_connection_get_state(&c->connection);
            if (state == AZ_AMQP_CONNECTION_STATE_CLOSED
                || state == AZ_AMQP_CONNECTION_STATE_ERROR)
            {
                break;
            }
            if (!pump_connection(&c->connection, &c->transport_storage, &c->connection_failed, 100))
            {
                break;
            }
        }
    }

    /* Release the transport's TLS slot (see e2e_amqp_telemetry_end). */
    if (c->transport.vtable != NULL && c->transport.vtable->close != NULL)
    {
        (void)c->transport.vtable->close(&c->transport);
    }
    free(c);
    if (!ok && err_out != NULL)
    {
        *err_out = err;
    }
    return ok;
}

/* --- file-upload notification receiver ------------------------------------ */

static void on_filenotify_received(
    az_amqp_link* link,
    az_amqp_message const* message,
    az_amqp_delivery const* delivery,
    void* user_data)
{
    e2e_amqp_filenotify* f = (e2e_amqp_filenotify*)user_data;
    f->delivered_count++;

    az_amqp_message_body_kind body_kind;
    az_span body;
    if (az_result_failed(az_amqp_message_get_body(message, &body_kind, &body))
        || body_kind != AZ_AMQP_MESSAGE_BODY_KIND_DATA
        || az_span_size(body) <= 0)
    {
        f->unparsed_count++;
        E2E_AMQP_DISCARD(az_amqp_link_accept(link, delivery->number));
        return;
    }

    char text[E2E_AMQP_NOTIFY_BODY_MAX];
    int n = az_span_size(body);
    if (n > (int)sizeof(text) - 1)
    {
        n = (int)sizeof(text) - 1;
    }
    memcpy(text, az_span_ptr(body), (size_t)n);
    text[n] = '\0';

    /* The notification node is hub-wide. A notification for someone else is
     * RELEASED so the hub redelivers it to the leg that is waiting for it. */
    if (f->match[0] != '\0' && strstr(text, f->match) == NULL)
    {
        f->released_count++;
        E2E_AMQP_DISCARD(az_amqp_link_release(link, delivery->number));
        return;
    }

    if (f->captured_count < E2E_AMQP_NOTIFY_CAPTURE_MAX)
    {
        memcpy(f->captured[f->captured_count], text, (size_t)n + 1);
        f->captured_count++;
    }

    /* Settle ours so the hub does not redeliver it to a later run. */
    E2E_AMQP_DISCARD(az_amqp_link_accept(link, delivery->number));
}

bool e2e_amqp_filenotify_begin(
    e2e_amqp_filenotify* f,
    const char* hub_host,
    const char* sas_token,
    const char* match,
    const char** err_out)
{
    const char* err = NULL;
    az_span fqdn = az_span_create_from_str((char*)(uintptr_t)hub_host);
    az_span token = az_span_create_from_str((char*)(uintptr_t)sas_token);

    snprintf(f->match, sizeof(f->match), "%s", (match != NULL) ? match : "");

    /* CBS audience for the IoT Hub service endpoint is the hub host. snprintf
     * reports what it WOULD have written, so a longer host must not be turned
     * into a span that runs past the buffer. */
    int audience_length
        = snprintf(f->audience_buffer, sizeof(f->audience_buffer), "%s", hub_host);
    if (audience_length < 0 || (size_t)audience_length >= sizeof(f->audience_buffer))
    {
        err = "filenotify: hub host too long for the CBS audience";
        goto error;
    }
    az_span audience = az_span_create((uint8_t*)f->audience_buffer, audience_length);

    /* 1. TLS transport. */
    az_amqp_transport_options transport_options = { 0 };
    transport_options.host_name = fqdn;
    transport_options.port = AZ_AMQP_PORT_AMQPS;
    transport_options.tls_enabled = true;
    if (az_result_failed(
            az_amqp_sample_transport_init(&f->transport, &f->transport_storage, &transport_options)))
    {
        err = "filenotify: transport init failed";
        goto error;
    }

    /* 2. Connection with SASL ANONYMOUS (authorization happens over CBS). */
    f->session_slots[0] = NULL;
    az_amqp_connection_storage connection_storage = {
        .incoming_buffer = AZ_SPAN_FROM_BUFFER(f->incoming_buffer),
        .outgoing_buffer = AZ_SPAN_FROM_BUFFER(f->outgoing_buffer),
        .sessions = f->session_slots,
        .sessions_capacity = 1,
    };
    az_amqp_connection_options connection_options = az_amqp_connection_options_default();
    connection_options.container_id = AZ_SPAN_FROM_STR("az-iot-e2e-filenotify");
    connection_options.hostname = fqdn;
    connection_options.idle_timeout_milliseconds = 240000;
    connection_options.sasl.mechanism = AZ_AMQP_SASL_MECHANISM_ANONYMOUS;
    if (az_result_failed(az_amqp_connection_init(
            &f->connection, &f->transport, &connection_storage, &connection_options)))
    {
        err = "filenotify: connection init failed";
        goto error;
    }
    az_amqp_connection_set_state_callback(
        &f->connection, on_connection_state_changed, &f->connection_failed);
    if (az_result_failed(az_amqp_connection_open(&f->connection)))
    {
        err = "filenotify: connection open failed";
        goto error;
    }
    while (az_amqp_connection_get_state(&f->connection) == AZ_AMQP_CONNECTION_STATE_OPENING)
    {
        if (!pump_connection(&f->connection, &f->transport_storage, &f->connection_failed, 500))
        {
            err = "filenotify: connection failed during open";
            goto error;
        }
    }

    /* 3. Session (CBS pair + receiver). */
    az_amqp_session_storage session_storage = { .links = f->link_slots, .links_capacity = 3 };
    if (az_result_failed(az_amqp_session_init(&f->session, &f->connection, &session_storage, NULL))
        || az_result_failed(az_amqp_session_begin(&f->session)))
    {
        err = "filenotify: session begin failed";
        goto error;
    }
    while (az_amqp_session_get_state(&f->session) == AZ_AMQP_SESSION_STATE_BEGINNING)
    {
        if (!pump_connection(&f->connection, &f->transport_storage, &f->connection_failed, 500))
        {
            err = "filenotify: connection failed during session begin";
            goto error;
        }
    }

    /* 4. CBS authorize the hub host. */
    az_amqp_cbs_options cbs_options = az_amqp_cbs_options_default();
    cbs_options.reply_buffer = AZ_SPAN_FROM_BUFFER(f->cbs_reply_buffer);
    if (az_result_failed(az_amqp_cbs_init(&f->cbs, &f->session, &cbs_options))
        || az_result_failed(az_amqp_cbs_open(&f->cbs)))
    {
        err = "filenotify: cbs open failed";
        goto error;
    }
    while (az_amqp_cbs_get_state(&f->cbs) == AZ_AMQP_CBS_STATE_OPENING)
    {
        if (!pump_connection(&f->connection, &f->transport_storage, &f->connection_failed, 500))
        {
            err = "filenotify: connection failed during cbs open";
            goto error;
        }
    }
    put_token_result put_token = { 0 };
    if (az_result_failed(az_amqp_cbs_put_token(
            &f->cbs,
            AZ_SPAN_FROM_STR(AZ_AMQP_CBS_TOKEN_TYPE_SAS),
            audience,
            token,
            0,
            on_put_token_complete,
            &put_token)))
    {
        err = "filenotify: put-token request failed to queue";
        goto error;
    }
    while (!put_token.done)
    {
        if (!pump_connection(&f->connection, &f->transport_storage, &f->connection_failed, 500))
        {
            err = "filenotify: connection failed during cbs authorization";
            goto error;
        }
    }
    if (put_token.status / 100 != 2)
    {
        err = "filenotify: cbs authorization rejected";
        goto error;
    }

    /* 5. Receiver on the file-notification node. */
    az_amqp_link_options receiver_options = az_amqp_link_receiver_options_default(
        AZ_SPAN_FROM_STR("e2e-filenotify-recv"),
        az_amqp_source_from_address(AZ_SPAN_FROM_STR("/messages/serviceBound/filenotifications")),
        AZ_AMQP_RECEIVER_SETTLE_MODE_FIRST,
        AZ_SPAN_FROM_BUFFER(f->recv_buffer),
        10 /* prefetch credit */);
    if (az_result_failed(az_amqp_link_init(&f->receiver, &f->session, &receiver_options)))
    {
        err = "filenotify: receiver init failed";
        goto error;
    }
    az_amqp_link_set_message_callback(&f->receiver, on_filenotify_received, f);
    if (az_result_failed(az_amqp_link_attach(&f->receiver)))
    {
        err = "filenotify: receiver attach failed";
        goto error;
    }

    /* Wait for the peer's attach, exactly as steps 3 and 4 wait for theirs.
     * Returning as soon as `attach` is QUEUED would report success for a link the
     * hub is about to refuse -- and it does refuse, transiently, while the
     * enableFileUploadNotifications flag set during provisioning propagates. The
     * caller would then watch a link that does not exist: nothing is ever
     * delivered, no traffic keeps the connection alive, and the only symptom is
     * an idle disconnect ~240s later followed by a notification timeout that
     * blames the hub for publishing nothing. Fail here instead, with the reason. */
    while (az_amqp_link_get_state(&f->receiver) == AZ_AMQP_LINK_STATE_ATTACHING)
    {
        if (!pump_connection(&f->connection, &f->transport_storage, &f->connection_failed, 500))
        {
            err = "filenotify: connection failed during receiver attach";
            goto error;
        }
    }
    if (az_amqp_link_get_state(&f->receiver) != AZ_AMQP_LINK_STATE_ATTACHED)
    {
        az_amqp_error_detail detail = az_amqp_link_get_last_error(&f->receiver);
        fprintf(
            stderr,
            "[e2e amqp] filenotify receiver attach refused: state=%d code=0x%08x "
            "condition='%.*s' description='%.*s'\n",
            (int)az_amqp_link_get_state(&f->receiver),
            (unsigned)detail.code,
            (int)az_span_size(detail.amqp.condition),
            (const char*)az_span_ptr(detail.amqp.condition),
            (int)az_span_size(detail.amqp.description),
            (const char*)az_span_ptr(detail.amqp.description));
        err = "filenotify: receiver attach refused by the hub";
        goto error;
    }

    f->started = true;
    return true;

error:
    if (err_out != NULL)
    {
        *err_out = err;
    }
    e2e_amqp_filenotify_end(f);
    return false;
}

bool e2e_amqp_filenotify_do_work(e2e_amqp_filenotify* f, int wait_ms)
{
    if (!f->started)
    {
        return false;
    }
    return pump_connection(&f->connection, &f->transport_storage, &f->connection_failed, wait_ms);
}

bool e2e_amqp_filenotify_seen(const e2e_amqp_filenotify* f, const char* needle)
{
    for (int i = 0; i < f->captured_count; i++)
    {
        if (strstr(f->captured[i], needle) != NULL)
        {
            return true;
        }
    }
    return false;
}

void e2e_amqp_filenotify_stats(
    const e2e_amqp_filenotify* f,
    int* out_delivered,
    int* out_captured,
    int* out_released,
    int* out_unparsed)
{
    if (out_delivered != NULL) *out_delivered = f->delivered_count;
    if (out_captured != NULL) *out_captured = f->captured_count;
    if (out_released != NULL) *out_released = f->released_count;
    if (out_unparsed != NULL) *out_unparsed = f->unparsed_count;
}

void e2e_amqp_filenotify_end(e2e_amqp_filenotify* f)
{
    if (f->started)
    {
        f->started = false;
        E2E_AMQP_DISCARD(az_amqp_link_detach(&f->receiver, NULL));
        E2E_AMQP_DISCARD(az_amqp_cbs_close(&f->cbs));
        E2E_AMQP_DISCARD(az_amqp_session_end(&f->session, NULL));
        E2E_AMQP_DISCARD(az_amqp_connection_close(&f->connection, NULL));

        for (int i = 0; i < 40; i++)
        {
            az_amqp_connection_state state = az_amqp_connection_get_state(&f->connection);
            if (state == AZ_AMQP_CONNECTION_STATE_CLOSED || state == AZ_AMQP_CONNECTION_STATE_ERROR)
            {
                break;
            }
            if (!pump_connection(&f->connection, &f->transport_storage, &f->connection_failed, 100))
            {
                break;
            }
        }
    }

    /* Always release the transport's TLS slot (see e2e_amqp_telemetry_end). */
    if (f->transport.vtable != NULL && f->transport.vtable->close != NULL)
    {
        (void)f->transport.vtable->close(&f->transport);
    }
}
