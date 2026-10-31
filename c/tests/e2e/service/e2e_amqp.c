// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
#include "e2e_amqp.h"
#include "e2e_notify_time.h"

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
#define E2E_ERR_TELEMETRY_CONN_DURING_RECEIVER_ATTACH \
  E2E_AMQP_PFX_TELEMETRY "connection failed during receiver attach"
#define E2E_ERR_TELEMETRY_FILTER E2E_AMQP_PFX_TELEMETRY "enqueued-time filter encode failed"
#define E2E_ERR_TELEMETRY_SOURCE E2E_AMQP_PFX_TELEMETRY "partition source address too long"
#define E2E_ERR_TELEMETRY_OUT_OF_MEMORY E2E_AMQP_PFX_TELEMETRY "out of memory"
#define E2E_ERR_TELEMETRY_CAPTURE_FILTER E2E_AMQP_PFX_TELEMETRY "capture filter too long"
#define E2E_ERR_TELEMETRY_PARAMETER_TOO_LONG \
  E2E_AMQP_PFX_TELEMETRY "host name or SAS token too long"
#define E2E_ERR_TELEMETRY_SETUP_TIMEOUT E2E_AMQP_PFX_TELEMETRY "connection setup timed out"
#define E2E_ERR_TELEMETRY_CONNECTION_FAILED E2E_AMQP_PFX_TELEMETRY "connection failed"
#define E2E_ERR_TELEMETRY_NOT_CONNECTED E2E_AMQP_PFX_TELEMETRY "not connected"

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
  e2e_amqp_put_token_result* result = (e2e_amqp_put_token_result*)user_data;
  result->status = status_code;
  result->done = true;
}

/* Pump a connection once, waiting up to @p wait_ms for socket I/O unless @p transport is NULL. */
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
  if (transport != NULL)
  {
    az_amqp_sample_wait_for_io(transport, io.io_interest, wait);
  }
  return true;
}

/* --- telemetry receiver --------------------------------------------------- */

/* Longest printable form of one property value. */
#define E2E_AMQP_VALUE_TEXT_MAX 512

/** @brief Writes @p bytes as text: printable ASCII and UTF-8 kept, control bytes as '?'. */
static void bytes_to_text(const uint8_t* bytes, int32_t len, char* out, size_t cap)
{
  size_t n = 0;
  for (int32_t i = 0; i < len && n + 1 < cap; ++i)
  {
    const uint8_t b = bytes[i];
    out[n++] = (b < 0x20 || b == 0x7f) ? '?' : (char)b;
  }
  out[n] = '\0';
}

/** @brief Writes binary @p bytes as text when every byte is printable ASCII, else as hex. */
static void binary_to_text(az_span bytes, char* out, size_t cap)
{
  const uint8_t* p = az_span_ptr(bytes);
  const int32_t len = az_span_size(bytes);
  bool printable = true;
  for (int32_t i = 0; i < len; ++i)
  {
    printable = printable && p[i] >= 0x20 && p[i] < 0x7f;
  }
  if (printable)
  {
    bytes_to_text(p, len, out, cap);
    return;
  }
  size_t n = (size_t)snprintf(out, cap, "0x");
  for (int32_t i = 0; i < len && n + 3 <= cap; ++i)
  {
    n += (size_t)snprintf(out + n, cap - n, "%02x", p[i]);
  }
}

static void uuid_to_text(const uint8_t* u, char* out, size_t cap)
{
  snprintf(
      out,
      cap,
      "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
      u[0],
      u[1],
      u[2],
      u[3],
      u[4],
      u[5],
      u[6],
      u[7],
      u[8],
      u[9],
      u[10],
      u[11],
      u[12],
      u[13],
      u[14],
      u[15]);
}

/** @brief Writes a printable form of any AMQP value; compound values are summarized. */
static void value_to_text(const az_amqp_value* v, char* out, size_t cap)
{
  switch (v->kind)
  {
    case AZ_AMQP_VALUE_KIND_NULL:
      snprintf(out, cap, "null");
      break;
    case AZ_AMQP_VALUE_KIND_BOOL:
      snprintf(out, cap, "%s", v->scalar.boolean ? "true" : "false");
      break;
    case AZ_AMQP_VALUE_KIND_UBYTE:
    case AZ_AMQP_VALUE_KIND_USHORT:
    case AZ_AMQP_VALUE_KIND_UINT:
    case AZ_AMQP_VALUE_KIND_ULONG:
      snprintf(out, cap, "%llu", (unsigned long long)v->scalar.u64);
      break;
    case AZ_AMQP_VALUE_KIND_BYTE:
    case AZ_AMQP_VALUE_KIND_SHORT:
    case AZ_AMQP_VALUE_KIND_INT:
    case AZ_AMQP_VALUE_KIND_LONG:
      snprintf(out, cap, "%lld", (long long)v->scalar.i64);
      break;
    case AZ_AMQP_VALUE_KIND_TIMESTAMP:
      snprintf(out, cap, "timestamp:%lld", (long long)v->scalar.i64);
      break;
    case AZ_AMQP_VALUE_KIND_FLOAT:
    case AZ_AMQP_VALUE_KIND_DOUBLE:
      snprintf(out, cap, "%g", v->scalar.f64);
      break;
    case AZ_AMQP_VALUE_KIND_CHAR:
      snprintf(out, cap, "U+%04llX", (unsigned long long)v->scalar.u64);
      break;
    case AZ_AMQP_VALUE_KIND_UUID:
      if (az_span_size(v->payload) == 16)
      {
        uuid_to_text(az_span_ptr(v->payload), out, cap);
      }
      else
      {
        snprintf(out, cap, "<uuid>");
      }
      break;
    case AZ_AMQP_VALUE_KIND_BINARY:
      binary_to_text(v->payload, out, cap);
      break;
    case AZ_AMQP_VALUE_KIND_STRING:
    case AZ_AMQP_VALUE_KIND_SYMBOL:
      bytes_to_text(az_span_ptr(v->payload), az_span_size(v->payload), out, cap);
      break;
    case AZ_AMQP_VALUE_KIND_LIST:
      snprintf(out, cap, "<list of %u>", (unsigned)v->count);
      break;
    case AZ_AMQP_VALUE_KIND_MAP:
      snprintf(out, cap, "<map of %u>", (unsigned)(v->count / 2));
      break;
    case AZ_AMQP_VALUE_KIND_ARRAY:
      snprintf(out, cap, "<array of %u>", (unsigned)v->count);
      break;
    case AZ_AMQP_VALUE_KIND_DESCRIBED:
    default:
      snprintf(out, cap, "<described>");
      break;
  }
}

static void message_id_to_text(const az_amqp_message_id* id, char* out, size_t cap)
{
  switch (id->kind)
  {
    case AZ_AMQP_MESSAGE_ID_KIND_ULONG:
      snprintf(out, cap, "%llu", (unsigned long long)id->u64);
      break;
    case AZ_AMQP_MESSAGE_ID_KIND_UUID:
      if (az_span_size(id->bytes) == 16)
      {
        uuid_to_text(az_span_ptr(id->bytes), out, cap);
      }
      else
      {
        snprintf(out, cap, "<uuid>");
      }
      break;
    case AZ_AMQP_MESSAGE_ID_KIND_BINARY:
      binary_to_text(id->bytes, out, cap);
      break;
    case AZ_AMQP_MESSAGE_ID_KIND_NULL:
    case AZ_AMQP_MESSAGE_ID_KIND_STRING:
    default:
      bytes_to_text(az_span_ptr(id->bytes), az_span_size(id->bytes), out, cap);
      break;
  }
}

/** @brief Appends one "name\0value\0" pair; marks the capture truncated if it does not fit. */
static void capture_add(e2e_amqp_capture* c, const char* name, const char* value)
{
  const size_t name_size = strlen(name) + 1;
  const size_t value_size = strlen(value) + 1;
  if (c->props_len + name_size + value_size > sizeof(c->props))
  {
    c->props_truncated = true;
    return;
  }
  memcpy(c->props + c->props_len, name, name_size);
  memcpy(c->props + c->props_len + name_size, value, value_size);
  c->props_len += name_size + value_size;
}

static void capture_add_span(e2e_amqp_capture* c, const char* name, az_span text)
{
  if (az_span_size(text) > 0)
  {
    char value[E2E_AMQP_VALUE_TEXT_MAX];
    bytes_to_text(az_span_ptr(text), az_span_size(text), value, sizeof(value));
    capture_add(c, name, value);
  }
}

static void capture_add_id(e2e_amqp_capture* c, const char* name, const az_amqp_message_id* id)
{
  if (id->kind != AZ_AMQP_MESSAGE_ID_KIND_NULL)
  {
    char value[E2E_AMQP_VALUE_TEXT_MAX];
    message_id_to_text(id, value, sizeof(value));
    capture_add(c, name, value);
  }
}

static void capture_add_i64(e2e_amqp_capture* c, const char* name, int64_t value)
{
  char text[32];
  snprintf(text, sizeof(text), "%lld", (long long)value);
  capture_add(c, name, text);
}

/** @brief Captures the properties section as "amqp:<field>". */
static void capture_properties(e2e_amqp_capture* c, const az_amqp_message* message)
{
  az_amqp_message_properties p;
  if (az_result_failed(az_amqp_message_get_properties(message, &p)))
  {
    return;
  }
  capture_add_id(c, "amqp:message-id", &p.message_id);
  if (az_span_size(p.user_id) > 0)
  {
    char value[E2E_AMQP_VALUE_TEXT_MAX];
    binary_to_text(p.user_id, value, sizeof(value));
    capture_add(c, "amqp:user-id", value);
  }
  capture_add_span(c, "amqp:to", p.to);
  capture_add_span(c, "amqp:subject", p.subject);
  capture_add_span(c, "amqp:reply-to", p.reply_to);
  capture_add_id(c, "amqp:correlation-id", &p.correlation_id);
  capture_add_span(c, "amqp:content-type", p.content_type);
  capture_add_span(c, "amqp:content-encoding", p.content_encoding);
  if (p.has_absolute_expiry_time)
  {
    capture_add_i64(c, "amqp:absolute-expiry-time", p.absolute_expiry_time_ms);
  }
  if (p.has_creation_time)
  {
    capture_add_i64(c, "amqp:creation-time", p.creation_time_ms);
  }
  capture_add_span(c, "amqp:group-id", p.group_id);
  if (p.has_group_sequence)
  {
    capture_add_i64(c, "amqp:group-sequence", (int64_t)p.group_sequence);
  }
  capture_add_span(c, "amqp:reply-to-group-id", p.reply_to_group_id);
}

/** @brief Captures every pair of the encoded map @p encoded, keys prefixed with @p prefix. */
static void capture_map(e2e_amqp_capture* c, az_span encoded, const char* prefix)
{
  az_amqp_decoder decoder;
  az_amqp_value map;
  az_amqp_decoder pairs;
  if (az_span_size(encoded) == 0 || az_result_failed(az_amqp_decoder_init(&decoder, encoded))
      || az_result_failed(az_amqp_decoder_decode(&decoder, &map))
      || az_result_failed(az_amqp_value_get_map_decoder(&map, &pairs, NULL)))
  {
    return;
  }
  while (az_amqp_decoder_has_next(&pairs))
  {
    az_amqp_value key;
    az_amqp_value value;
    if (az_result_failed(az_amqp_decoder_decode(&pairs, &key))
        || az_result_failed(az_amqp_decoder_decode(&pairs, &value)))
    {
      capture_add(c, prefix, "<undecodable>");
      return;
    }
    char key_text[256];
    const size_t prefix_len = strlen(prefix);
    snprintf(key_text, sizeof(key_text), "%s", prefix);
    if (prefix_len < sizeof(key_text))
    {
      value_to_text(&key, key_text + prefix_len, sizeof(key_text) - prefix_len);
    }
    char value_text[E2E_AMQP_VALUE_TEXT_MAX];
    value_to_text(&value, value_text, sizeof(value_text));
    capture_add(c, key_text, value_text);
  }
}

/** @brief True if the @p size bytes at @p data contain the NUL-terminated @p needle. */
static bool bytes_contain(const uint8_t* data, size_t size, const char* needle)
{
  const size_t n = strlen(needle);
  if (n == 0 || n > size)
  {
    return false;
  }
  for (size_t i = 0; i + n <= size; i++)
  {
    if (data[i] == (uint8_t)needle[0] && memcmp(data + i, needle, n) == 0)
    {
      return true;
    }
  }
  return false;
}

/** @brief The Event Hubs sequence number of @p message within its partition, or -1 if absent. */
static int64_t message_sequence_number(az_amqp_message const* message)
{
  static const char key_name[] = "x-opt-sequence-number";
  az_span annotations;
  az_amqp_decoder decoder;
  az_amqp_value map;
  az_amqp_decoder pairs;
  if (az_result_failed(az_amqp_message_get_message_annotations(message, &annotations))
      || az_result_failed(az_amqp_decoder_init(&decoder, annotations))
      || az_result_failed(az_amqp_decoder_decode(&decoder, &map))
      || az_result_failed(az_amqp_value_get_map_decoder(&map, &pairs, NULL)))
  {
    return -1;
  }
  while (az_amqp_decoder_has_next(&pairs))
  {
    az_amqp_value key;
    az_amqp_value value;
    if (az_result_failed(az_amqp_decoder_decode(&pairs, &key))
        || az_result_failed(az_amqp_decoder_decode(&pairs, &value)))
    {
      return -1;
    }
    if (key.kind == AZ_AMQP_VALUE_KIND_SYMBOL
        && az_span_size(key.payload) == (int32_t)(sizeof(key_name) - 1)
        && memcmp(az_span_ptr(key.payload), key_name, sizeof(key_name) - 1) == 0)
    {
      return value.kind == AZ_AMQP_VALUE_KIND_LONG ? value.scalar.i64 : -1;
    }
  }
  return -1;
}

static void on_message_received(
    az_amqp_link* link,
    az_amqp_message const* message,
    az_amqp_delivery const* delivery,
    void* user_data)
{
  e2e_amqp_telemetry* t = (e2e_amqp_telemetry*)user_data;
  if (!e2e_amqp_telemetry_record_sequence_number(
          t, (int)(link - t->receivers), message_sequence_number(message)))
  {
    E2E_AMQP_DISCARD(az_amqp_link_accept(link, delivery->number));
    return;
  }

  az_amqp_message_body_kind body_kind;
  az_span body;
  const bool has_body = az_result_succeeded(az_amqp_message_get_body(message, &body_kind, &body));
  /* Before ring insertion, so a later eviction cannot hide a watched needle. */
  for (int i = 0; has_body && i < t->watched_count; i++)
  {
    if (!t->watched_seen[i]
        && bytes_contain(az_span_ptr(body), (size_t)az_span_size(body), t->watched[i]))
    {
      t->watched_seen[i] = true;
      printf("[e2e amqp] watched needle '%s' received\n", t->watched[i]);
    }
  }
  e2e_amqp_capture* slot = NULL;
  if (has_body && body_kind == AZ_AMQP_MESSAGE_BODY_KIND_DATA)
  {
    slot = e2e_amqp_telemetry_capture_body(t, az_span_ptr(body), (size_t)az_span_size(body));
  }
  if (slot != NULL)
  {
    capture_properties(slot, message);

    az_span map;
    if (az_result_succeeded(az_amqp_message_get_application_properties(message, &map)))
    {
      capture_map(slot, map, "app:");
    }
    if (az_result_succeeded(az_amqp_message_get_message_annotations(message, &map)))
    {
      capture_map(slot, map, "annotation:");
    }
  }

  E2E_AMQP_DISCARD(az_amqp_link_accept(link, delivery->number));
}

bool e2e_amqp_telemetry_set_capture_filter(e2e_amqp_telemetry* t, const char* capture_filter)
{
  const size_t n = capture_filter != NULL ? strlen(capture_filter) : 0;
  if (n >= sizeof(t->capture_filter))
  {
    return false;
  }
  if (n > 0)
  {
    memcpy(t->capture_filter, capture_filter, n);
  }
  t->capture_filter[n] = '\0';
  return true;
}

e2e_amqp_capture* e2e_amqp_telemetry_capture_body(
    e2e_amqp_telemetry* t,
    const uint8_t* body,
    size_t body_size)
{
  const uint32_t seq = t->received_count++;
  if (t->capture_filter[0] != '\0' && !bytes_contain(body, body_size, t->capture_filter))
  {
    return NULL;
  }
  e2e_amqp_capture* slot = &t->captured[t->captured_next];
  if (t->captured_count == E2E_AMQP_CAPTURE_MAX && !t->eviction_reported)
  {
    t->eviction_reported = true;
    printf(
        "[e2e amqp] capture ring full: message #%u evicts #%u\n",
        (unsigned)seq,
        (unsigned)slot->seq);
  }
  const size_t n
      = body_size < E2E_AMQP_CAPTURE_BODY_MAX - 1 ? body_size : E2E_AMQP_CAPTURE_BODY_MAX - 1;
  if (n > 0)
  {
    memcpy(slot->body, body, n);
  }
  slot->body[n] = '\0';
  slot->body_size = body_size;
  slot->props_len = 0;
  slot->props_truncated = false;
  slot->seq = seq;
  t->captured_next = (t->captured_next + 1) % E2E_AMQP_CAPTURE_MAX;
  if (t->captured_count < E2E_AMQP_CAPTURE_MAX)
  {
    t->captured_count++;
  }
  return slot;
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

/**
 * @brief Keeps what the setup steps and e2e_amqp_telemetry_restart() need: host, SAS token,
 * audience, receive buffers, enqueued-time filter and partition source addresses.
 *
 * @return NULL, or the static reason of the failure.
 */
static const char* telemetry_prepare(
    e2e_amqp_telemetry* t,
    const char* eh_host,
    const char* entity_path,
    const char* consumer_group,
    int64_t enqueued_after_ms,
    const char* sas_token,
    int partition_count,
    const char* capture_filter)
{
  if (!e2e_amqp_telemetry_set_capture_filter(t, capture_filter))
  {
    return E2E_ERR_TELEMETRY_CAPTURE_FILTER;
  }
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

  const int host_length = snprintf(t->host, sizeof(t->host), "%s", eh_host);
  const int token_length = snprintf(t->sas_token, sizeof(t->sas_token), "%s", sas_token);
  if (host_length < 0 || host_length >= (int)sizeof(t->host) || token_length < 0
      || token_length >= (int)sizeof(t->sas_token))
  {
    return E2E_ERR_TELEMETRY_PARAMETER_TOO_LONG;
  }
  (void)snprintf(
      t->audience_buffer, sizeof(t->audience_buffer), "amqps://%s/%s", eh_host, entity_path);

  t->recv_buffers = (uint8_t*)malloc((size_t)partition_count * E2E_AMQP_RECV_MESSAGE_MAX);
  if (t->recv_buffers == NULL)
  {
    return E2E_ERR_TELEMETRY_OUT_OF_MEMORY;
  }
  t->filter = AZ_SPAN_EMPTY;
  if (enqueued_after_ms > 0
      && !encode_enqueued_after_filter(
          AZ_SPAN_FROM_BUFFER(t->filter_buffer), enqueued_after_ms, &t->filter))
  {
    return E2E_ERR_TELEMETRY_FILTER;
  }
  for (int p = 0; p < partition_count; p++)
  {
    const int source_length = snprintf(
        t->source_addr[p],
        sizeof(t->source_addr[p]),
        "%s/ConsumerGroups/%s/Partitions/%d",
        entity_path,
        consumer_group,
        p);
    if (source_length <= 0 || source_length >= (int)sizeof(t->source_addr[p]))
    {
      return E2E_ERR_TELEMETRY_SOURCE;
    }
    (void)snprintf(t->link_name[p], sizeof(t->link_name[p]), "e2e-recv-%d", p);
  }
  return NULL;
}

/**
 * @brief Opens a TLS transport to the AMQPS port and an AMQP connection on it, starting the
 * setup deadline; e2e_amqp_telemetry_do_work() takes the remaining steps.
 *
 * @return NULL, or the static reason of the failure.
 */
static const char* telemetry_open_connection(e2e_amqp_telemetry* t)
{
  az_span fqdn = az_span_create_from_str(t->host);
  az_amqp_transport_options transport_options = { 0 };
  transport_options.host_name = fqdn;
  transport_options.port = AZ_AMQP_PORT_AMQPS;
  transport_options.tls_enabled = true;
  const az_result transport_result = t->transport_override != NULL
      ? az_amqp_transport_init(&t->transport, t->transport_override, t->transport_override_impl)
      : az_amqp_sample_transport_init(&t->transport, &t->transport_storage, &transport_options);
  if (az_result_failed(transport_result))
  {
    return E2E_ERR_TELEMETRY_TRANSPORT_INIT;
  }

  /* SASL ANONYMOUS: Event Hubs authorizes per entity via CBS. */
  t->connection_failed = false;
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
    return E2E_ERR_TELEMETRY_CONN_INIT;
  }
  az_amqp_connection_set_state_callback(
      &t->connection, on_connection_state_changed, &t->connection_failed);
  if (az_result_failed(az_amqp_connection_open(&t->connection)))
  {
    return E2E_ERR_TELEMETRY_CONN_OPEN;
  }
  t->phase = E2E_AMQP_TELEMETRY_PHASE_OPENING_CONNECTION;
  t->setup_deadline = time(NULL) + E2E_AMQP_SETUP_TIMEOUT_S;
  return NULL;
}

/** @brief The transport to wait on for socket I/O, or NULL for a unit test's transport. */
static az_amqp_sample_transport* telemetry_socket_transport(e2e_amqp_telemetry* t)
{
  return t->transport_override != NULL ? NULL : &t->transport_storage;
}

/**
 * @brief Starts attaching one receiver per partition, from the enqueued-time filter when set.
 *
 * @return NULL, or the static reason of the failure.
 */
static const char* telemetry_attach_receivers(e2e_amqp_telemetry* t)
{
  for (int p = 0; p < t->partition_count; p++)
  {
    az_amqp_source source = az_amqp_source_from_address(az_span_create_from_str(t->source_addr[p]));
    source.filter = t->filter;
    az_amqp_link_options receiver_options = az_amqp_link_receiver_options_default(
        az_span_create_from_str(t->link_name[p]),
        source,
        AZ_AMQP_RECEIVER_SETTLE_MODE_FIRST,
        az_span_create(
            t->recv_buffers + (size_t)p * E2E_AMQP_RECV_MESSAGE_MAX, E2E_AMQP_RECV_MESSAGE_MAX),
        50 /* prefetch credit */);
    if (az_result_failed(az_amqp_link_init(&t->receivers[p], &t->session, &receiver_options)))
    {
      return E2E_ERR_TELEMETRY_RECEIVER_INIT;
    }
    az_amqp_link_set_message_callback(&t->receivers[p], on_message_received, t);
    if (az_result_failed(az_amqp_link_attach(&t->receivers[p])))
    {
      return E2E_ERR_TELEMETRY_RECEIVER_ATTACH;
    }
  }
  t->phase = E2E_AMQP_TELEMETRY_PHASE_ATTACHING_RECEIVERS;
  return NULL;
}

/**
 * @brief Starts the next setup step once the service completed the current one: session, CBS
 * link, SAS authorization of the entity, then the receivers. The setup is done once the service
 * answered every receiver's attach; one it detached is then found and reconnected.
 *
 * @return NULL, or the static reason of the failure.
 */
static const char* telemetry_advance_setup(e2e_amqp_telemetry* t)
{
  switch (t->phase)
  {
    case E2E_AMQP_TELEMETRY_PHASE_OPENING_CONNECTION:
    {
      if (az_amqp_connection_get_state(&t->connection) == AZ_AMQP_CONNECTION_STATE_OPENING)
      {
        return NULL;
      }
      /* Sized for the CBS pair + one receiver per partition. */
      az_amqp_session_storage session_storage
          = { .links = t->link_slots, .links_capacity = t->partition_count + 2 };
      if (az_result_failed(
              az_amqp_session_init(&t->session, &t->connection, &session_storage, NULL))
          || az_result_failed(az_amqp_session_begin(&t->session)))
      {
        return E2E_ERR_TELEMETRY_SESSION_BEGIN;
      }
      t->phase = E2E_AMQP_TELEMETRY_PHASE_BEGINNING_SESSION;
      return NULL;
    }
    case E2E_AMQP_TELEMETRY_PHASE_BEGINNING_SESSION:
    {
      if (az_amqp_session_get_state(&t->session) == AZ_AMQP_SESSION_STATE_BEGINNING)
      {
        return NULL;
      }
      az_amqp_cbs_options cbs_options = az_amqp_cbs_options_default();
      cbs_options.reply_buffer = AZ_SPAN_FROM_BUFFER(t->cbs_reply_buffer);
      if (az_result_failed(az_amqp_cbs_init(&t->cbs, &t->session, &cbs_options))
          || az_result_failed(az_amqp_cbs_open(&t->cbs)))
      {
        return E2E_ERR_TELEMETRY_CBS_OPEN;
      }
      t->phase = E2E_AMQP_TELEMETRY_PHASE_OPENING_CBS;
      return NULL;
    }
    case E2E_AMQP_TELEMETRY_PHASE_OPENING_CBS:
    {
      if (az_amqp_cbs_get_state(&t->cbs) == AZ_AMQP_CBS_STATE_OPENING)
      {
        return NULL;
      }
      t->put_token = (e2e_amqp_put_token_result){ 0 };
      if (az_result_failed(az_amqp_cbs_put_token(
              &t->cbs,
              AZ_SPAN_FROM_STR(AZ_AMQP_CBS_TOKEN_TYPE_SAS),
              az_span_create_from_str(t->audience_buffer),
              az_span_create_from_str(t->sas_token),
              0,
              on_put_token_complete,
              &t->put_token)))
      {
        return E2E_ERR_TELEMETRY_PUT_TOKEN_QUEUE;
      }
      t->phase = E2E_AMQP_TELEMETRY_PHASE_AUTHORIZING;
      return NULL;
    }
    case E2E_AMQP_TELEMETRY_PHASE_AUTHORIZING:
      if (!t->put_token.done)
      {
        return NULL;
      }
      if (t->put_token.status / 100 != 2)
      {
        return E2E_ERR_TELEMETRY_CBS_AUTH_REJECTED;
      }
      return telemetry_attach_receivers(t);
    case E2E_AMQP_TELEMETRY_PHASE_ATTACHING_RECEIVERS:
      for (int p = 0; p < t->partition_count; p++)
      {
        if (az_amqp_link_get_state(&t->receivers[p]) == AZ_AMQP_LINK_STATE_ATTACHING)
        {
          return NULL;
        }
      }
      t->phase = E2E_AMQP_TELEMETRY_PHASE_RECEIVING;
      return NULL;
    case E2E_AMQP_TELEMETRY_PHASE_IDLE:
    case E2E_AMQP_TELEMETRY_PHASE_RECEIVING:
    default:
      return NULL;
  }
}

/** @brief The reason to report when the connection fails during @p phase. */
static const char* telemetry_connection_failure(e2e_amqp_telemetry_phase phase)
{
  switch (phase)
  {
    case E2E_AMQP_TELEMETRY_PHASE_OPENING_CONNECTION:
      return E2E_ERR_TELEMETRY_CONN_DURING_OPEN;
    case E2E_AMQP_TELEMETRY_PHASE_BEGINNING_SESSION:
      return E2E_ERR_TELEMETRY_CONN_DURING_SESSION_BEGIN;
    case E2E_AMQP_TELEMETRY_PHASE_OPENING_CBS:
      return E2E_ERR_TELEMETRY_CONN_DURING_CBS_OPEN;
    case E2E_AMQP_TELEMETRY_PHASE_AUTHORIZING:
      return E2E_ERR_TELEMETRY_CONN_DURING_CBS_AUTH;
    case E2E_AMQP_TELEMETRY_PHASE_ATTACHING_RECEIVERS:
      return E2E_ERR_TELEMETRY_CONN_DURING_RECEIVER_ATTACH;
    case E2E_AMQP_TELEMETRY_PHASE_IDLE:
    case E2E_AMQP_TELEMETRY_PHASE_RECEIVING:
    default:
      return E2E_ERR_TELEMETRY_CONNECTION_FAILED;
  }
}

/** @brief Closes the transport, and with it the connection, without waiting for the service. */
static void telemetry_close_transport(e2e_amqp_telemetry* t)
{
  if (t->transport.vtable != NULL && t->transport.vtable->close != NULL)
  {
    (void)t->transport.vtable->close(&t->transport);
  }
  memset(&t->transport, 0, sizeof(t->transport));
  t->phase = E2E_AMQP_TELEMETRY_PHASE_IDLE;
}

bool e2e_amqp_telemetry_begin(
    e2e_amqp_telemetry* t,
    const char* eh_host,
    const char* entity_path,
    const char* consumer_group,
    int64_t enqueued_after_ms,
    const char* sas_token,
    int partition_count,
    const char* capture_filter,
    const char** err_out)
{
  const char* err = telemetry_prepare(
      t,
      eh_host,
      entity_path,
      consumer_group,
      enqueued_after_ms,
      sas_token,
      partition_count,
      capture_filter);
  if (err == NULL)
  {
    err = telemetry_open_connection(t);
  }
  while (err == NULL && t->phase != E2E_AMQP_TELEMETRY_PHASE_RECEIVING)
  {
    if (!e2e_amqp_telemetry_do_work(t, 500))
    {
      err = t->error;
    }
  }
  if (err == NULL)
  {
    return true;
  }
  if (err_out != NULL)
  {
    *err_out = err;
  }
  e2e_amqp_telemetry_end(t);
  return false;
}

bool e2e_amqp_telemetry_restart(e2e_amqp_telemetry* t)
{
  telemetry_close_transport(t);
  const char* err = telemetry_open_connection(t);
  if (err != NULL)
  {
    t->error = err;
    telemetry_close_transport(t);
    return false;
  }
  return true;
}

bool e2e_amqp_telemetry_record_sequence_number(
    e2e_amqp_telemetry* t,
    int partition,
    int64_t sequence_number)
{
  if (sequence_number < 0 || partition < 0 || partition >= E2E_AMQP_MAX_PARTITIONS)
  {
    return true;
  }
  if (sequence_number < t->next_sequence_number[partition])
  {
    return false;
  }
  t->next_sequence_number[partition] = sequence_number + 1;
  return true;
}

bool e2e_amqp_telemetry_do_work(e2e_amqp_telemetry* t, int wait_ms)
{
  const e2e_amqp_telemetry_phase phase = t->phase;
  const char* err = NULL;
  if (phase == E2E_AMQP_TELEMETRY_PHASE_IDLE)
  {
    err = E2E_ERR_TELEMETRY_NOT_CONNECTED;
  }
  else if (!pump_connection(
               &t->connection, telemetry_socket_transport(t), &t->connection_failed, wait_ms))
  {
    err = telemetry_connection_failure(phase);
  }
  else if (phase != E2E_AMQP_TELEMETRY_PHASE_RECEIVING)
  {
    err = time(NULL) > t->setup_deadline ? E2E_ERR_TELEMETRY_SETUP_TIMEOUT
                                         : telemetry_advance_setup(t);
  }
  if (err != NULL)
  {
    t->error = err;
    return false;
  }
  return true;
}

int e2e_amqp_telemetry_find_detached_receiver(const e2e_amqp_telemetry* t)
{
  for (int p = 0; t->phase == E2E_AMQP_TELEMETRY_PHASE_RECEIVING && p < t->partition_count; p++)
  {
    const az_amqp_link_state state = az_amqp_link_get_state(&t->receivers[p]);
    if (state == AZ_AMQP_LINK_STATE_DETACHED || state == AZ_AMQP_LINK_STATE_ERROR)
    {
      return p;
    }
  }
  return -1;
}

bool e2e_amqp_telemetry_seen(const e2e_amqp_telemetry* t, const char* needle)
{
  return e2e_amqp_telemetry_find(t, needle) != NULL;
}

bool e2e_amqp_telemetry_watch_needle(e2e_amqp_telemetry* t, const char* needle)
{
  const size_t n = strlen(needle);
  if (n == 0 || n >= E2E_AMQP_WATCHED_NEEDLE_MAX || t->watched_count >= E2E_AMQP_WATCHED_MAX)
  {
    return false;
  }
  memcpy(t->watched[t->watched_count], needle, n + 1);
  t->watched_seen[t->watched_count] = false;
  t->watched_count++;
  return true;
}

bool e2e_amqp_telemetry_watched_seen(const e2e_amqp_telemetry* t, const char* needle)
{
  for (int i = 0; i < t->watched_count; i++)
  {
    if (t->watched_seen[i] && strcmp(t->watched[i], needle) == 0)
    {
      return true;
    }
  }
  return false;
}

const e2e_amqp_capture* e2e_amqp_telemetry_find(const e2e_amqp_telemetry* t, const char* needle)
{
  const e2e_amqp_capture* found = NULL;
  for (int i = 0; i < t->captured_count; i++)
  {
    const e2e_amqp_capture* c = &t->captured[i];
    if (strstr(c->body, needle) != NULL && (found == NULL || c->seq < found->seq))
    {
      found = c;
    }
  }
  return found;
}

bool e2e_amqp_capture_property(
    const e2e_amqp_capture* c,
    const char* name,
    char* out,
    size_t out_size)
{
  for (size_t i = 0; i < c->props_len;)
  {
    const char* key = c->props + i;
    const char* value = key + strlen(key) + 1;
    if (strcmp(key, name) == 0)
    {
      if (out_size > 0)
      {
        snprintf(out, out_size, "%s", value);
      }
      return true;
    }
    i = (size_t)(value - c->props) + strlen(value) + 1;
  }
  return false;
}

bool e2e_amqp_capture_find_value(
    const e2e_amqp_capture* c,
    const char* value,
    char* name_out,
    size_t name_out_size)
{
  for (size_t i = 0; i < c->props_len;)
  {
    const char* key = c->props + i;
    const char* v = key + strlen(key) + 1;
    if (strcmp(v, value) == 0)
    {
      if (name_out_size > 0)
      {
        snprintf(name_out, name_out_size, "%s", key);
      }
      return true;
    }
    i = (size_t)(v - c->props) + strlen(v) + 1;
  }
  return false;
}

void e2e_amqp_capture_dump(const e2e_amqp_capture* c)
{
  printf(
      "[e2e amqp] message #%u, body %zu bytes '%.64s'%s\n",
      (unsigned)c->seq,
      c->body_size,
      c->body,
      c->props_truncated ? " (properties truncated)" : "");
  for (size_t i = 0; i < c->props_len;)
  {
    const char* key = c->props + i;
    const char* value = key + strlen(key) + 1;
    printf("[e2e amqp]   %s = '%s'\n", key, value);
    i = (size_t)(value - c->props) + strlen(value) + 1;
  }
}

void e2e_amqp_telemetry_end(e2e_amqp_telemetry* t)
{
  if (t->phase == E2E_AMQP_TELEMETRY_PHASE_ATTACHING_RECEIVERS
      || t->phase == E2E_AMQP_TELEMETRY_PHASE_RECEIVING)
  {
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
      if (!pump_connection(
              &t->connection, telemetry_socket_transport(t), &t->connection_failed, 100))
      {
        break;
      }
    }
  }

  /* Also when the setup did not finish, so the socket and TLS state are released:
   * az_amqp_connection_close leaves the caller-owned transport open. */
  telemetry_close_transport(t);
  free(t->recv_buffers);
  t->recv_buffers = NULL;
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
    const az_iot_e2e_c2d_properties* c2d_properties,
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
  e2e_amqp_put_token_result put_token = { 0 };
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
  if (c2d_properties != NULL)
  {
    if (c2d_properties->message_id != NULL)
    {
      properties.message_id = az_amqp_message_id_from_string(
          az_span_create_from_str((char*)(uintptr_t)c2d_properties->message_id));
    }
    if (c2d_properties->correlation_id != NULL)
    {
      properties.correlation_id = az_amqp_message_id_from_string(
          az_span_create_from_str((char*)(uintptr_t)c2d_properties->correlation_id));
    }
    if (c2d_properties->content_type != NULL)
    {
      properties.content_type
          = az_span_create_from_str((char*)(uintptr_t)c2d_properties->content_type);
    }
    if (c2d_properties->content_encoding != NULL)
    {
      properties.content_encoding
          = az_span_create_from_str((char*)(uintptr_t)c2d_properties->content_encoding);
    }
    if (c2d_properties->absolute_expiry_time_ms != 0)
    {
      properties.absolute_expiry_time_ms = c2d_properties->absolute_expiry_time_ms;
      properties.has_absolute_expiry_time = true;
    }
  }
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

  /* Close the transport (see e2e_amqp_telemetry_end). */
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
    if (e2e_notify_enqueued_time(text, &enqueued_s)
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
  e2e_amqp_put_token_result put_token = { 0 };
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

  /* Always close the transport (see e2e_amqp_telemetry_end). */
  if (f->transport.vtable != NULL && f->transport.vtable->close != NULL)
  {
    (void)f->transport.vtable->close(&f->transport);
  }
}
