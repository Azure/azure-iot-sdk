// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
#include "e2e_amqp.h"

#include "az_amqp_sample_wait.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* GCC flags these AZ_NODISCARD (warn_unused_result) az_amqp calls under -Werror even
   with a plain (void) cast, which GCC deliberately ignores. Route the intentional
   best-effort/teardown discards through an assignment so the result is observed. */
#define E2E_AMQP_DISCARD(expr)         \
  do                                   \
  {                                    \
    az_result _e2e_discard_r = (expr); \
    (void)_e2e_discard_r;              \
  } while (0)

/* --- error messages -------------------------------------------------------
 * Reported through the *_begin / *_send `err_out` parameters and surfaced to a
 * test by az_iot_e2e_service_last_error(). Defined in one place so a message is
 * written once rather than repeated at each site, and so rewording one cannot
 * leave near-duplicates behind.
 *
 * The three watchers fail in the same sequence -- transport, connection,
 * session, CBS, then link -- so the subsystem prefix is factored out and the
 * shared reasons are spelled once. Adjacent string literals are concatenated by
 * the compiler, so each macro is still a single static string.
 */
#define E2E_AMQP_PFX_TELEMETRY "telemetry: "
#define E2E_AMQP_PFX_C2D "c2d: "
#define E2E_AMQP_PFX_FILENOTIFY "filenotify: "

#define E2E_AMQP_R_TRANSPORT_INIT "transport init failed"
#define E2E_AMQP_R_CONN_INIT "connection init failed"
#define E2E_AMQP_R_CONN_OPEN "connection open failed"
#define E2E_AMQP_R_CONN_DURING_OPEN "connection failed during open"
#define E2E_AMQP_R_SESSION_BEGIN "session begin failed"
#define E2E_AMQP_R_CONN_DURING_SESSION_BEGIN "connection failed during session begin"
#define E2E_AMQP_R_CBS_OPEN "cbs open failed"
#define E2E_AMQP_R_CONN_DURING_CBS_OPEN "connection failed during cbs open"
#define E2E_AMQP_R_PUT_TOKEN_QUEUE "put-token request failed to queue"
#define E2E_AMQP_R_CONN_DURING_CBS_AUTH "connection failed during cbs authorization"
#define E2E_AMQP_R_CBS_AUTH_REJECTED "cbs authorization rejected"
#define E2E_AMQP_R_RECEIVER_INIT "receiver init failed"
#define E2E_AMQP_R_RECEIVER_ATTACH "receiver attach failed"

/* telemetry */
#define E2E_ERR_TELEMETRY_TRANSPORT_INIT E2E_AMQP_PFX_TELEMETRY E2E_AMQP_R_TRANSPORT_INIT
#define E2E_ERR_TELEMETRY_CONN_INIT E2E_AMQP_PFX_TELEMETRY E2E_AMQP_R_CONN_INIT
#define E2E_ERR_TELEMETRY_CONN_OPEN E2E_AMQP_PFX_TELEMETRY E2E_AMQP_R_CONN_OPEN
#define E2E_ERR_TELEMETRY_CONN_DURING_OPEN E2E_AMQP_PFX_TELEMETRY E2E_AMQP_R_CONN_DURING_OPEN
#define E2E_ERR_TELEMETRY_SESSION_BEGIN E2E_AMQP_PFX_TELEMETRY E2E_AMQP_R_SESSION_BEGIN
#define E2E_ERR_TELEMETRY_CONN_DURING_SESSION_BEGIN \
  E2E_AMQP_PFX_TELEMETRY E2E_AMQP_R_CONN_DURING_SESSION_BEGIN
#define E2E_ERR_TELEMETRY_CBS_OPEN E2E_AMQP_PFX_TELEMETRY E2E_AMQP_R_CBS_OPEN
#define E2E_ERR_TELEMETRY_CONN_DURING_CBS_OPEN \
  E2E_AMQP_PFX_TELEMETRY E2E_AMQP_R_CONN_DURING_CBS_OPEN
#define E2E_ERR_TELEMETRY_PUT_TOKEN_QUEUE E2E_AMQP_PFX_TELEMETRY E2E_AMQP_R_PUT_TOKEN_QUEUE
#define E2E_ERR_TELEMETRY_CONN_DURING_CBS_AUTH \
  E2E_AMQP_PFX_TELEMETRY E2E_AMQP_R_CONN_DURING_CBS_AUTH
#define E2E_ERR_TELEMETRY_CBS_AUTH_REJECTED E2E_AMQP_PFX_TELEMETRY E2E_AMQP_R_CBS_AUTH_REJECTED
#define E2E_ERR_TELEMETRY_RECEIVER_INIT E2E_AMQP_PFX_TELEMETRY E2E_AMQP_R_RECEIVER_INIT
#define E2E_ERR_TELEMETRY_RECEIVER_ATTACH E2E_AMQP_PFX_TELEMETRY E2E_AMQP_R_RECEIVER_ATTACH
#define E2E_ERR_TELEMETRY_FILTER E2E_AMQP_PFX_TELEMETRY "enqueued-time filter encode failed"
#define E2E_ERR_TELEMETRY_SOURCE E2E_AMQP_PFX_TELEMETRY "partition source address too long"

/* c2d */
#define E2E_ERR_C2D_OUT_OF_MEMORY E2E_AMQP_PFX_C2D "out of memory"
#define E2E_ERR_C2D_TRANSPORT_INIT E2E_AMQP_PFX_C2D E2E_AMQP_R_TRANSPORT_INIT
#define E2E_ERR_C2D_CONN_INIT E2E_AMQP_PFX_C2D E2E_AMQP_R_CONN_INIT
#define E2E_ERR_C2D_CONN_OPEN E2E_AMQP_PFX_C2D E2E_AMQP_R_CONN_OPEN
#define E2E_ERR_C2D_CONN_DURING_OPEN E2E_AMQP_PFX_C2D E2E_AMQP_R_CONN_DURING_OPEN
#define E2E_ERR_C2D_SESSION_BEGIN E2E_AMQP_PFX_C2D E2E_AMQP_R_SESSION_BEGIN
#define E2E_ERR_C2D_CONN_DURING_SESSION_BEGIN E2E_AMQP_PFX_C2D E2E_AMQP_R_CONN_DURING_SESSION_BEGIN
#define E2E_ERR_C2D_CBS_OPEN E2E_AMQP_PFX_C2D E2E_AMQP_R_CBS_OPEN
#define E2E_ERR_C2D_CONN_DURING_CBS_OPEN E2E_AMQP_PFX_C2D E2E_AMQP_R_CONN_DURING_CBS_OPEN
#define E2E_ERR_C2D_PUT_TOKEN_QUEUE E2E_AMQP_PFX_C2D E2E_AMQP_R_PUT_TOKEN_QUEUE
#define E2E_ERR_C2D_CONN_DURING_CBS_AUTH E2E_AMQP_PFX_C2D E2E_AMQP_R_CONN_DURING_CBS_AUTH
#define E2E_ERR_C2D_CBS_AUTH_REJECTED E2E_AMQP_PFX_C2D E2E_AMQP_R_CBS_AUTH_REJECTED
#define E2E_ERR_C2D_SENDER_ATTACH E2E_AMQP_PFX_C2D "sender attach failed"
#define E2E_ERR_C2D_CONN_DURING_SENDER_ATTACH \
  E2E_AMQP_PFX_C2D "connection failed during sender attach"
#define E2E_ERR_C2D_SEND_QUEUE E2E_AMQP_PFX_C2D "send failed to queue"
#define E2E_ERR_C2D_CONN_DURING_SEND E2E_AMQP_PFX_C2D "connection failed during send"
#define E2E_ERR_C2D_NOT_ACCEPTED E2E_AMQP_PFX_C2D "message not accepted by IoT Hub"

/* filenotify */
#define E2E_ERR_FILENOTIFY_AUDIENCE_TOO_LONG \
  E2E_AMQP_PFX_FILENOTIFY "hub host too long for the CBS audience"
#define E2E_ERR_FILENOTIFY_TRANSPORT_INIT E2E_AMQP_PFX_FILENOTIFY E2E_AMQP_R_TRANSPORT_INIT
#define E2E_ERR_FILENOTIFY_CONN_INIT E2E_AMQP_PFX_FILENOTIFY E2E_AMQP_R_CONN_INIT
#define E2E_ERR_FILENOTIFY_CONN_OPEN E2E_AMQP_PFX_FILENOTIFY E2E_AMQP_R_CONN_OPEN
#define E2E_ERR_FILENOTIFY_CONN_DURING_OPEN E2E_AMQP_PFX_FILENOTIFY E2E_AMQP_R_CONN_DURING_OPEN
#define E2E_ERR_FILENOTIFY_SESSION_BEGIN E2E_AMQP_PFX_FILENOTIFY E2E_AMQP_R_SESSION_BEGIN
#define E2E_ERR_FILENOTIFY_CONN_DURING_SESSION_BEGIN \
  E2E_AMQP_PFX_FILENOTIFY E2E_AMQP_R_CONN_DURING_SESSION_BEGIN
#define E2E_ERR_FILENOTIFY_CBS_OPEN E2E_AMQP_PFX_FILENOTIFY E2E_AMQP_R_CBS_OPEN
#define E2E_ERR_FILENOTIFY_CONN_DURING_CBS_OPEN \
  E2E_AMQP_PFX_FILENOTIFY E2E_AMQP_R_CONN_DURING_CBS_OPEN
#define E2E_ERR_FILENOTIFY_PUT_TOKEN_QUEUE E2E_AMQP_PFX_FILENOTIFY E2E_AMQP_R_PUT_TOKEN_QUEUE
#define E2E_ERR_FILENOTIFY_CONN_DURING_CBS_AUTH \
  E2E_AMQP_PFX_FILENOTIFY E2E_AMQP_R_CONN_DURING_CBS_AUTH
#define E2E_ERR_FILENOTIFY_CBS_AUTH_REJECTED E2E_AMQP_PFX_FILENOTIFY E2E_AMQP_R_CBS_AUTH_REJECTED
#define E2E_ERR_FILENOTIFY_RECEIVER_INIT E2E_AMQP_PFX_FILENOTIFY E2E_AMQP_R_RECEIVER_INIT
#define E2E_ERR_FILENOTIFY_RECEIVER_ATTACH E2E_AMQP_PFX_FILENOTIFY E2E_AMQP_R_RECEIVER_ATTACH
#define E2E_ERR_FILENOTIFY_CONN_DURING_RECEIVER_ATTACH \
  E2E_AMQP_PFX_FILENOTIFY "connection failed during receiver attach"
#define E2E_ERR_FILENOTIFY_RECEIVER_ATTACH_REFUSED \
  E2E_AMQP_PFX_FILENOTIFY "receiver attach refused by the hub"

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
      && body_kind == AZ_AMQP_MESSAGE_BODY_KIND_DATA)
  {
    int n = az_span_size(body);
    if (n > E2E_AMQP_CAPTURE_BODY_MAX - 1)
    {
      n = E2E_AMQP_CAPTURE_BODY_MAX - 1;
    }
    char* slot = t->captured[t->captured_next];
    memcpy(slot, az_span_ptr(body), (size_t)n);
    slot[n] = '\0';
    t->captured_next = (t->captured_next + 1) % E2E_AMQP_CAPTURE_MAX;
    if (t->captured_count < E2E_AMQP_CAPTURE_MAX)
    {
      t->captured_count++;
    }
  }

  E2E_AMQP_DISCARD(az_amqp_link_accept(link, delivery->number));
}

/**
 * @brief Encodes an Event Hubs filter-set selecting messages enqueued after @p after_ms.
 *
 * @return true with the encoded map in @p out_filter; false if @p buffer is too small.
 */
static bool encode_enqueued_after_filter(az_span buffer, int64_t after_ms, az_span* out_filter)
{
  static const az_span selector = AZ_SPAN_LITERAL_FROM_STR("apache.org:selector-filter:string");
  char expression[96];
  int length = snprintf(
      expression,
      sizeof(expression),
      "amqp.annotation.x-opt-enqueued-time > '%lld'",
      (long long)after_ms);
  if (length <= 0 || length >= (int)sizeof(expression))
  {
    return false;
  }

  az_amqp_encoder encoder;
  if (az_result_failed(az_amqp_encoder_init(&encoder, buffer))
      || az_result_failed(az_amqp_encoder_begin_map(&encoder))
      || az_result_failed(az_amqp_encoder_append_symbol(&encoder, selector))
      || az_result_failed(az_amqp_encoder_append_descriptor_symbol(&encoder, selector))
      || az_result_failed(
          az_amqp_encoder_append_string(&encoder, az_span_create((uint8_t*)expression, length)))
      || az_result_failed(az_amqp_encoder_end_map(&encoder)))
  {
    return false;
  }
  *out_filter = az_amqp_encoder_get_bytes(&encoder);
  return true;
}

bool e2e_amqp_telemetry_begin(
    e2e_amqp_telemetry* t,
    const char* eh_host,
    const char* entity_path,
    const char* consumer_group,
    int64_t enqueued_after_ms,
    const char* sas_token,
    int partition_count,
    const char** err_out)
{
  const char* err = NULL;
  if (consumer_group == NULL || consumer_group[0] == '\0')
  {
    consumer_group = "$Default";
  }
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
    err = E2E_ERR_TELEMETRY_TRANSPORT_INIT;
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
    err = E2E_ERR_TELEMETRY_CONN_INIT;
    goto error;
  }
  az_amqp_connection_set_state_callback(
      &t->connection, on_connection_state_changed, &t->connection_failed);

  if (az_result_failed(az_amqp_connection_open(&t->connection)))
  {
    err = E2E_ERR_TELEMETRY_CONN_OPEN;
    goto error;
  }
  while (az_amqp_connection_get_state(&t->connection) == AZ_AMQP_CONNECTION_STATE_OPENING)
  {
    if (!pump_connection(&t->connection, &t->transport_storage, &t->connection_failed, 500))
    {
      err = E2E_ERR_TELEMETRY_CONN_DURING_OPEN;
      goto error;
    }
  }

  /* 3. Session sized for the CBS pair + one receiver per partition. */
  az_amqp_session_storage session_storage
      = { .links = t->link_slots, .links_capacity = partition_count + 2 };
  if (az_result_failed(az_amqp_session_init(&t->session, &t->connection, &session_storage, NULL))
      || az_result_failed(az_amqp_session_begin(&t->session)))
  {
    err = E2E_ERR_TELEMETRY_SESSION_BEGIN;
    goto error;
  }
  while (az_amqp_session_get_state(&t->session) == AZ_AMQP_SESSION_STATE_BEGINNING)
  {
    if (!pump_connection(&t->connection, &t->transport_storage, &t->connection_failed, 500))
    {
      err = E2E_ERR_TELEMETRY_CONN_DURING_SESSION_BEGIN;
      goto error;
    }
  }

  /* 4. CBS authorize the Event Hub-compatible entity. */
  az_amqp_cbs_options cbs_options = az_amqp_cbs_options_default();
  cbs_options.reply_buffer = AZ_SPAN_FROM_BUFFER(t->cbs_reply_buffer);
  if (az_result_failed(az_amqp_cbs_init(&t->cbs, &t->session, &cbs_options))
      || az_result_failed(az_amqp_cbs_open(&t->cbs)))
  {
    err = E2E_ERR_TELEMETRY_CBS_OPEN;
    goto error;
  }
  while (az_amqp_cbs_get_state(&t->cbs) == AZ_AMQP_CBS_STATE_OPENING)
  {
    if (!pump_connection(&t->connection, &t->transport_storage, &t->connection_failed, 500))
    {
      err = E2E_ERR_TELEMETRY_CONN_DURING_CBS_OPEN;
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
    err = E2E_ERR_TELEMETRY_PUT_TOKEN_QUEUE;
    goto error;
  }
  while (!put_token.done)
  {
    if (!pump_connection(&t->connection, &t->transport_storage, &t->connection_failed, 500))
    {
      err = E2E_ERR_TELEMETRY_CONN_DURING_CBS_AUTH;
      goto error;
    }
  }
  if (put_token.status / 100 != 2)
  {
    err = E2E_ERR_TELEMETRY_CBS_AUTH_REJECTED;
    goto error;
  }

  /* 5. One receiver per partition, from enqueued_after_ms when set. */
  az_span filter = AZ_SPAN_EMPTY;
  if (enqueued_after_ms > 0
      && !encode_enqueued_after_filter(
          AZ_SPAN_FROM_BUFFER(t->filter_buffer), enqueued_after_ms, &filter))
  {
    err = E2E_ERR_TELEMETRY_FILTER;
    goto error;
  }

  for (int p = 0; p < partition_count; p++)
  {
    int source_length = snprintf(
        t->source_addr[p],
        sizeof(t->source_addr[p]),
        "%s/ConsumerGroups/%s/Partitions/%d",
        entity_path,
        consumer_group,
        p);
    if (source_length <= 0 || source_length >= (int)sizeof(t->source_addr[p]))
    {
      err = E2E_ERR_TELEMETRY_SOURCE;
      goto error;
    }
    int name_length = snprintf(t->link_name[p], sizeof(t->link_name[p]), "e2e-recv-%d", p);

    az_amqp_source source
        = az_amqp_source_from_address(az_span_create((uint8_t*)t->source_addr[p], source_length));
    source.filter = filter;
    az_amqp_link_options receiver_options = az_amqp_link_receiver_options_default(
        az_span_create((uint8_t*)t->link_name[p], name_length),
        source,
        AZ_AMQP_RECEIVER_SETTLE_MODE_FIRST,
        AZ_SPAN_FROM_BUFFER(t->recv_buffers[p]),
        50 /* prefetch credit */);

    if (az_result_failed(az_amqp_link_init(&t->receivers[p], &t->session, &receiver_options)))
    {
      err = E2E_ERR_TELEMETRY_RECEIVER_INIT;
      goto error;
    }
    az_amqp_link_set_message_callback(&t->receivers[p], on_message_received, t);
    if (az_result_failed(az_amqp_link_attach(&t->receivers[p])))
    {
      err = E2E_ERR_TELEMETRY_RECEIVER_ATTACH;
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
      *err_out = E2E_ERR_C2D_OUT_OF_MEMORY;
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
  int audience_length = snprintf(c->audience_buffer, sizeof(c->audience_buffer), "%s", hub_host);
  az_span audience = az_span_create((uint8_t*)c->audience_buffer, audience_length);

  /* 1. TLS transport. */
  az_amqp_transport_options transport_options = { 0 };
  transport_options.host_name = fqdn;
  transport_options.port = AZ_AMQP_PORT_AMQPS;
  transport_options.tls_enabled = true;
  if (az_result_failed(
          az_amqp_sample_transport_init(&c->transport, &c->transport_storage, &transport_options)))
  {
    err = E2E_ERR_C2D_TRANSPORT_INIT;
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
    err = E2E_ERR_C2D_CONN_INIT;
    goto cleanup;
  }
  connection_ok = true;
  az_amqp_connection_set_state_callback(
      &c->connection, on_connection_state_changed, &c->connection_failed);
  if (az_result_failed(az_amqp_connection_open(&c->connection)))
  {
    err = E2E_ERR_C2D_CONN_OPEN;
    goto cleanup;
  }
  while (az_amqp_connection_get_state(&c->connection) == AZ_AMQP_CONNECTION_STATE_OPENING)
  {
    if (!pump_connection(&c->connection, &c->transport_storage, &c->connection_failed, 500))
    {
      err = E2E_ERR_C2D_CONN_DURING_OPEN;
      goto cleanup;
    }
  }

  /* 3. Session (CBS pair + sender). */
  az_amqp_session_storage session_storage = { .links = c->link_slots, .links_capacity = 3 };
  if (az_result_failed(az_amqp_session_init(&c->session, &c->connection, &session_storage, NULL))
      || az_result_failed(az_amqp_session_begin(&c->session)))
  {
    err = E2E_ERR_C2D_SESSION_BEGIN;
    goto cleanup;
  }
  session_ok = true;
  while (az_amqp_session_get_state(&c->session) == AZ_AMQP_SESSION_STATE_BEGINNING)
  {
    if (!pump_connection(&c->connection, &c->transport_storage, &c->connection_failed, 500))
    {
      err = E2E_ERR_C2D_CONN_DURING_SESSION_BEGIN;
      goto cleanup;
    }
  }

  /* 4. CBS authorize the hub host. */
  az_amqp_cbs_options cbs_options = az_amqp_cbs_options_default();
  cbs_options.reply_buffer = AZ_SPAN_FROM_BUFFER(c->cbs_reply_buffer);
  if (az_result_failed(az_amqp_cbs_init(&c->cbs, &c->session, &cbs_options))
      || az_result_failed(az_amqp_cbs_open(&c->cbs)))
  {
    err = E2E_ERR_C2D_CBS_OPEN;
    goto cleanup;
  }
  cbs_ok = true;
  while (az_amqp_cbs_get_state(&c->cbs) == AZ_AMQP_CBS_STATE_OPENING)
  {
    if (!pump_connection(&c->connection, &c->transport_storage, &c->connection_failed, 500))
    {
      err = E2E_ERR_C2D_CONN_DURING_CBS_OPEN;
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
    err = E2E_ERR_C2D_PUT_TOKEN_QUEUE;
    goto cleanup;
  }
  while (!put_token.done)
  {
    if (!pump_connection(&c->connection, &c->transport_storage, &c->connection_failed, 500))
    {
      err = E2E_ERR_C2D_CONN_DURING_CBS_AUTH;
      goto cleanup;
    }
  }
  if (put_token.status / 100 != 2)
  {
    err = E2E_ERR_C2D_CBS_AUTH_REJECTED;
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
    err = E2E_ERR_C2D_SENDER_ATTACH;
    goto cleanup;
  }
  sender_ok = true;
  while (az_amqp_link_get_state(&c->sender) == AZ_AMQP_LINK_STATE_ATTACHING
         || az_amqp_link_get_credit(&c->sender) == 0)
  {
    if (!pump_connection(&c->connection, &c->transport_storage, &c->connection_failed, 500))
    {
      err = E2E_ERR_C2D_CONN_DURING_SENDER_ATTACH;
      goto cleanup;
    }
  }

  /* 6. Build and send the message addressed to the target device. */
  int to_length
      = snprintf(c->to_buffer, sizeof(c->to_buffer), "/devices/%s/messages/devicebound", device_id);

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
    err = E2E_ERR_C2D_SEND_QUEUE;
    goto cleanup;
  }
  while (!send.done)
  {
    if (!pump_connection(&c->connection, &c->transport_storage, &c->connection_failed, 500))
    {
      err = E2E_ERR_C2D_CONN_DURING_SEND;
      goto cleanup;
    }
  }
  if (send.outcome != AZ_AMQP_DELIVERY_OUTCOME_ACCEPTED)
  {
    err = E2E_ERR_C2D_NOT_ACCEPTED;
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
      if (state == AZ_AMQP_CONNECTION_STATE_CLOSED || state == AZ_AMQP_CONNECTION_STATE_ERROR)
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

/* Age past which another device's notification has no waiting watcher: a test
 * process is killed at CTest's 900 s timeout. The rest is clock-skew margin. */
#define E2E_AMQP_NOTIFY_STALE_S (20 * 60)

/* How long another live watcher's notification is held before it is released.
 * With time()'s 1 s resolution this is 1-2 s. */
#define E2E_AMQP_NOTIFY_HOLD_S 2

/**
 * @brief Releases held notifications; all of them if @p all, else those held
 * for at least #E2E_AMQP_NOTIFY_HOLD_S.
 */
static void release_held(e2e_amqp_filenotify* f, bool all)
{
  int64_t const now = (int64_t)time(NULL);
  int kept = 0;
  for (int i = 0; i < f->held_count; i++)
  {
    if (all || now - f->held[i].held_at_s >= E2E_AMQP_NOTIFY_HOLD_S)
    {
      f->released_count++;
      E2E_AMQP_DISCARD(az_amqp_link_release(&f->receiver, f->held[i].delivery_number));
    }
    else
    {
      f->held[kept++] = f->held[i];
    }
  }
  f->held_count = kept;
}

/**
 * @brief Parses @p count decimal digits at @p p.
 *
 * @return true with the value in @p out; false if any of them is not a digit.
 */
static bool parse_digits(const char* p, int count, int* out)
{
  int value = 0;
  for (int i = 0; i < count; i++)
  {
    if (p[i] < '0' || p[i] > '9')
    {
      return false;
    }
    value = value * 10 + (p[i] - '0');
  }
  *out = value;
  return true;
}

/**
 * @brief Converts a proleptic Gregorian UTC date to days since 1970-01-01.
 */
static int64_t days_from_civil(int year, int month, int day)
{
  year -= (month <= 2) ? 1 : 0;
  int64_t const era = (year >= 0 ? year : year - 399) / 400;
  int64_t const yoe = year - era * 400;
  int64_t const doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  int64_t const doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

/**
 * @brief Number of days in @p month (1-12) of @p year.
 */
static int days_in_month(int year, int month)
{
  static const int days[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
  bool const leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
  return (month == 2 && leap) ? 29 : days[month - 1];
}

/**
 * @brief Reads the `enqueuedTimeUtc` field ("YYYY-MM-DDTHH:MM:SS...") of a
 * file-upload notification body.
 *
 * @return true with seconds since the Unix epoch in @p out_epoch_s; false if the
 * field is missing or malformed.
 */
static bool notification_enqueued_time(const char* body, int64_t* out_epoch_s)
{
  static const char key[] = "\"enqueuedTimeUtc\"";
  const char* p = strstr(body, key);
  if (p == NULL)
  {
    return false;
  }
  p += sizeof(key) - 1;
  while (*p == ' ' || *p == ':')
  {
    p++;
  }
  if (*p != '"')
  {
    return false;
  }
  p++;

  int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
  if (!parse_digits(p, 4, &year) || p[4] != '-' || !parse_digits(p + 5, 2, &month) || p[7] != '-'
      || !parse_digits(p + 8, 2, &day) || p[10] != 'T' || !parse_digits(p + 11, 2, &hour)
      || p[13] != ':' || !parse_digits(p + 14, 2, &minute) || p[16] != ':'
      || !parse_digits(p + 17, 2, &second) || month < 1 || month > 12 || day < 1
      || day > days_in_month(year, month) || hour > 23 || minute > 59 || second > 59)
  {
    return false;
  }
  *out_epoch_s = days_from_civil(year, month, day) * 86400 + hour * 3600 + minute * 60 + second;
  return true;
}

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
      || body_kind != AZ_AMQP_MESSAGE_BODY_KIND_DATA || az_span_size(body) <= 0)
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
   * RELEASED so the hub redelivers it to the leg that is waiting for it --
   * unless it is too old for any leg to be waiting. Nobody settles those, so on
   * a long-lived hub they pile up and are redelivered to every watcher ahead of
   * its own; accept them instead. Unparseable times are released, as before. */
  if (f->match[0] != '\0' && strstr(text, f->match) == NULL)
  {
    int64_t enqueued_s;
    if (notification_enqueued_time(text, &enqueued_s)
        && (int64_t)time(NULL) - enqueued_s > E2E_AMQP_NOTIFY_STALE_S)
    {
      f->stale_count++;
      E2E_AMQP_DISCARD(az_amqp_link_accept(link, delivery->number));
      return;
    }
    /* Released at once, it would come straight back to this link in a tight
     * loop; hold it briefly (see e2e_amqp_filenotify_do_work). */
    if (f->held_count < E2E_AMQP_NOTIFY_HOLD_MAX)
    {
      f->held[f->held_count].delivery_number = delivery->number;
      f->held[f->held_count].held_at_s = (int64_t)time(NULL);
      f->held_count++;
      return;
    }
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
    const char** err_out,
    bool* attach_refused_out)
{
  const char* err = NULL;
  bool attach_refused = false;
  az_span fqdn = az_span_create_from_str((char*)(uintptr_t)hub_host);
  az_span token = az_span_create_from_str((char*)(uintptr_t)sas_token);

  snprintf(f->match, sizeof(f->match), "%s", (match != NULL) ? match : "");

  /* CBS audience for the IoT Hub service endpoint is the hub host. snprintf
   * reports what it WOULD have written, so a longer host must not be turned
   * into a span that runs past the buffer. */
  int audience_length = snprintf(f->audience_buffer, sizeof(f->audience_buffer), "%s", hub_host);
  if (audience_length < 0 || (size_t)audience_length >= sizeof(f->audience_buffer))
  {
    err = E2E_ERR_FILENOTIFY_AUDIENCE_TOO_LONG;
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
    err = E2E_ERR_FILENOTIFY_TRANSPORT_INIT;
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
    err = E2E_ERR_FILENOTIFY_CONN_INIT;
    goto error;
  }
  az_amqp_connection_set_state_callback(
      &f->connection, on_connection_state_changed, &f->connection_failed);
  if (az_result_failed(az_amqp_connection_open(&f->connection)))
  {
    err = E2E_ERR_FILENOTIFY_CONN_OPEN;
    goto error;
  }
  while (az_amqp_connection_get_state(&f->connection) == AZ_AMQP_CONNECTION_STATE_OPENING)
  {
    if (!pump_connection(&f->connection, &f->transport_storage, &f->connection_failed, 500))
    {
      err = E2E_ERR_FILENOTIFY_CONN_DURING_OPEN;
      goto error;
    }
  }

  /* 3. Session (CBS pair + receiver). */
  az_amqp_session_storage session_storage = { .links = f->link_slots, .links_capacity = 3 };
  if (az_result_failed(az_amqp_session_init(&f->session, &f->connection, &session_storage, NULL))
      || az_result_failed(az_amqp_session_begin(&f->session)))
  {
    err = E2E_ERR_FILENOTIFY_SESSION_BEGIN;
    goto error;
  }
  while (az_amqp_session_get_state(&f->session) == AZ_AMQP_SESSION_STATE_BEGINNING)
  {
    if (!pump_connection(&f->connection, &f->transport_storage, &f->connection_failed, 500))
    {
      err = E2E_ERR_FILENOTIFY_CONN_DURING_SESSION_BEGIN;
      goto error;
    }
  }

  /* 4. CBS authorize the hub host. */
  az_amqp_cbs_options cbs_options = az_amqp_cbs_options_default();
  cbs_options.reply_buffer = AZ_SPAN_FROM_BUFFER(f->cbs_reply_buffer);
  if (az_result_failed(az_amqp_cbs_init(&f->cbs, &f->session, &cbs_options))
      || az_result_failed(az_amqp_cbs_open(&f->cbs)))
  {
    err = E2E_ERR_FILENOTIFY_CBS_OPEN;
    goto error;
  }
  while (az_amqp_cbs_get_state(&f->cbs) == AZ_AMQP_CBS_STATE_OPENING)
  {
    if (!pump_connection(&f->connection, &f->transport_storage, &f->connection_failed, 500))
    {
      err = E2E_ERR_FILENOTIFY_CONN_DURING_CBS_OPEN;
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
    err = E2E_ERR_FILENOTIFY_PUT_TOKEN_QUEUE;
    goto error;
  }
  while (!put_token.done)
  {
    if (!pump_connection(&f->connection, &f->transport_storage, &f->connection_failed, 500))
    {
      err = E2E_ERR_FILENOTIFY_CONN_DURING_CBS_AUTH;
      goto error;
    }
  }
  if (put_token.status / 100 != 2)
  {
    err = E2E_ERR_FILENOTIFY_CBS_AUTH_REJECTED;
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
    err = E2E_ERR_FILENOTIFY_RECEIVER_INIT;
    goto error;
  }
  az_amqp_link_set_message_callback(&f->receiver, on_filenotify_received, f);
  if (az_result_failed(az_amqp_link_attach(&f->receiver)))
  {
    err = E2E_ERR_FILENOTIFY_RECEIVER_ATTACH;
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
      err = E2E_ERR_FILENOTIFY_CONN_DURING_RECEIVER_ATTACH;
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
    err = E2E_ERR_FILENOTIFY_RECEIVER_ATTACH_REFUSED;
    attach_refused = true;
    goto error;
  }

  f->started = true;
  return true;

error:
  if (err_out != NULL)
  {
    *err_out = err;
  }
  if (attach_refused_out != NULL)
  {
    *attach_refused_out = attach_refused;
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
  bool ok = pump_connection(&f->connection, &f->transport_storage, &f->connection_failed, wait_ms);
  if (ok)
  {
    release_held(f, false);
  }
  return ok;
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
    int* out_stale,
    int* out_unparsed)
{
  if (out_delivered != NULL)
  {
    *out_delivered = f->delivered_count;
  }
  if (out_captured != NULL)
  {
    *out_captured = f->captured_count;
  }
  if (out_released != NULL)
  {
    *out_released = f->released_count;
  }
  if (out_stale != NULL)
  {
    *out_stale = f->stale_count;
  }
  if (out_unparsed != NULL)
  {
    *out_unparsed = f->unparsed_count;
  }
}

void e2e_amqp_filenotify_end(e2e_amqp_filenotify* f)
{
  if (f->started)
  {
    f->started = false;
    release_held(f, true);
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
