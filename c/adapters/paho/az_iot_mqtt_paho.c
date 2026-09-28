// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Paho-C MQTT adapter implementation.
 *
 * Design notes:
 * - We use Paho's MQTTAsync API. It handles both MQTTv3.1.1 and v5 via
 *   MQTTAsync_createWithOptions() + MQTTAsync_connectOptions::MQTTVersion.
 * - Paho fires its callbacks from internal threads. We marshal those events
 *   into a thread-safe FIFO; process_loop() drains the FIFO on the calling
 *   thread and invokes the user's az_iot_mqtt_event_callback. This keeps the
 *   SDK's single-threaded contract intact without requiring callers to
 *   know about Paho's threading model.
 * - TLS is intentionally NOT wired in this phase (PAHO_WITH_SSL=OFF in the
 *   adapter CMake). The TLS path lands together with certificate_provider in a later
 *   phase; until then, connect() succeeds against tcp:// brokers only.
 */

#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "azure/iot/adapters/az_iot_adapter_paho.h"
#include "azure/iot/az_iot_log.h"
#include "azure/iot/az_iot_result.h"
#include "azure/iot/az_iot_mqtt_iface.h"

#include "az_iot_paho_key_custody.h"

#include <MQTTAsync.h>

#include <stdbool.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include <stdio.h>

#if defined(_WIN32)
#include <windows.h>
typedef CRITICAL_SECTION paho_mutex;
static void paho_mutex_init(paho_mutex* m) { InitializeCriticalSection(m); }
static void paho_mutex_destroy(paho_mutex* m) { DeleteCriticalSection(m); }
static void paho_mutex_lock(paho_mutex* m) { EnterCriticalSection(m); }
static void paho_mutex_unlock(paho_mutex* m) { LeaveCriticalSection(m); }
#else
#include <pthread.h>
#include <time.h>
typedef pthread_mutex_t paho_mutex;
static void paho_mutex_init(paho_mutex* m) { pthread_mutex_init(m, NULL); }
static void paho_mutex_destroy(paho_mutex* m) { pthread_mutex_destroy(m); }
static void paho_mutex_lock(paho_mutex* m) { pthread_mutex_lock(m); }
static void paho_mutex_unlock(paho_mutex* m) { pthread_mutex_unlock(m); }
#endif

/* ------------------------------------------------------------------------- */
/* event queue                                                               */
/* ------------------------------------------------------------------------- */

typedef struct queued_event
{
  az_iot_mqtt_event evt;
  /* Backing storage for AZ_IOT_MQTT_EVT_MESSAGE events. */
  az_iot_mqtt_message msg;
  char* topic;
  uint8_t* payload;
  size_t payload_len;
  bool has_message;
  /* v5 User Properties backing storage (heap-allocated). */
  az_iot_mqtt_user_property* user_props;
  size_t user_props_count;
  /* v5 Content Type (heap copy). */
  char* content_type;
  /* v5 Correlation Data (heap copy). */
  uint8_t* correlation_data;
  size_t correlation_data_len;
  struct queued_event* next;
} queued_event;

/* ------------------------------------------------------------------------- */
/* client state                                                              */
/* ------------------------------------------------------------------------- */

typedef struct paho_client
{
  az_iot_mqtt_client base; /* MUST be first */
  az_iot_mqtt_iface iface_storage;
  az_iot_mqtt_version version;

  MQTTAsync paho; /* Paho async handle, NULL until connect() */
  char* server_uri; /* "ssl://host:port", or "wss://host:port/path" */
  char* proxy_uri; /* "[user[:pass]@]host:port", or NULL for a direct connect */
  char* client_id;

  az_iot_mqtt_event_callback inbound_cb;
  void* inbound_ctx;

  /* Thread-safe event queue. Producer = Paho callback threads. Consumer =
   * process_loop() on the user thread. */
  paho_mutex q_mutex;
  queued_event* q_head;
  queued_event* q_tail;

  /* Non-extractable key custody (D8): engine/provider handles and the key
   * reference file handed to Paho, held for as long as the connection that
   * uses them. */
  az_iot_paho_key_custody key_custody;

  /* Reason code to put in the MQTT 5 DISCONNECT, taken from the connect
   * options of the session in progress. Held here because disconnect() takes
   * no options of its own; reset on every connect so a reason cannot outlive
   * the session that asked for it. Unused on v3.1.1, which has no reason
   * codes. */
  uint8_t disconnect_reason_code;
} paho_client;

static paho_client* paho_self(az_iot_mqtt_client* c) { return (paho_client*)c; }

/* An optional C string the caller may leave unset either way: NULL or "" both
 * mean "not supplied". Spelled once so the three option checks below cannot
 * drift apart, and stated positively so call sites read as a plain
 * "this was supplied" rather than a negated absence. */
#define is_nonempty_cstr(s) ((s) != NULL && (s)[0] != '\0')

/* ------------------------------------------------------------------------- */
/* event queue helpers                                                       */
/* ------------------------------------------------------------------------- */

static void q_push(paho_client* m, queued_event* node)
{
  node->next = NULL;
  paho_mutex_lock(&m->q_mutex);
  if (m->q_tail)
  {
    m->q_tail->next = node;
  }
  else
  {
    m->q_head = node;
  }
  m->q_tail = node;
  paho_mutex_unlock(&m->q_mutex);
}

static queued_event* q_pop(paho_client* m)
{
  paho_mutex_lock(&m->q_mutex);
  queued_event* n = m->q_head;
  if (n)
  {
    m->q_head = n->next;
    if (!m->q_head)
    {
      m->q_tail = NULL;
    }
  }
  paho_mutex_unlock(&m->q_mutex);
  if (n)
  {
    n->next = NULL;
  }
  return n;
}

static void q_free(queued_event* n)
{
  if (!n)
  {
    return;
  }
  free(n->topic);
  free(n->payload);
  /* v5 property backing storage */
  if (n->user_props)
  {
    for (size_t i = 0; i < n->user_props_count; ++i)
    {
      free((void*)n->user_props[i].key);
      free((void*)n->user_props[i].value);
    }
    free(n->user_props);
  }
  free(n->content_type);
  free(n->correlation_data);
  free(n);
}

static void q_drain_all(paho_client* m)
{
  queued_event* n;
  while ((n = q_pop(m)) != NULL)
  {
    q_free(n);
  }
}

/* Allocate + enqueue a simple status event (no message payload), carrying the
 * code that came off the wire. */
static void enqueue_status_codes(
    paho_client* m,
    az_iot_mqtt_event_kind kind,
    az_iot_result status,
    uint16_t packet_id,
    int32_t protocol_code,
    int32_t transport_code)
{
  queued_event* n = (queued_event*)calloc(1, sizeof(*n));
  if (!n)
  {
    return;
  }
  n->evt.kind = kind;
  n->evt.status = status;
  n->evt.packet_id = packet_id;
  n->evt.protocol_code = protocol_code;
  n->evt.transport_code = transport_code;
  q_push(m, n);
}

/* A code that came off the wire, with no transport code to report. */
static void enqueue_status_code(
    paho_client* m,
    az_iot_mqtt_event_kind kind,
    az_iot_result status,
    uint16_t packet_id,
    int32_t protocol_code)
{
  enqueue_status_codes(m, kind, status, packet_id, protocol_code, 0);
}

/* Only a code that actually came off the wire is reportable as one. Paho's own
 * failures are negative, and 0 is this field's "not applicable". */
static int32_t paho_wire_code(int code) { return (code > 0) ? (int32_t)code : 0; }

/* The other half of the same split: Paho's negative MQTTASYNC_* codes are
 * failures BELOW MQTT -- socket refused, TLS handshake, DNS -- and are what
 * makes those three distinguishable to an application. Reported as the
 * transport code rather than being discarded. */
static int32_t paho_transport_code(int code) { return (code < 0) ? (int32_t)code : 0; }

/* Allocate + enqueue a simple status event (no message payload). */
static void enqueue_status(
    paho_client* m,
    az_iot_mqtt_event_kind kind,
    az_iot_result status,
    uint16_t packet_id)
{
  enqueue_status_code(m, kind, status, packet_id, 0);
}

static void extract_v5_props(queued_event* n, MQTTAsync_message* msg);

/* Allocate + enqueue an inbound MESSAGE event. Topic, payload and any v5
 * properties are deep-copied so the queued event is self-contained.
 *
 * `v5_src` carries the properties to extract, or NULL for a v3.1.1 session.
 * They are attached BEFORE the event is published to the queue: q_push() makes
 * the node visible to process_loop() on the user thread, which is free to
 * dispatch and free it immediately, so anything written to the node afterwards
 * races with that free. */
static void enqueue_message(
    paho_client* m,
    const char* topic,
    int topic_len,
    const void* payload,
    int payload_len,
    int qos,
    int retain,
    MQTTAsync_message* v5_src)
{
  queued_event* n = (queued_event*)calloc(1, sizeof(*n));
  if (!n)
  {
    return;
  }

  /* Paho passes topic_len == 0 to mean "C string". */
  size_t tlen = (topic_len > 0) ? (size_t)topic_len : (topic ? strlen(topic) : 0);
  n->topic = (char*)malloc(tlen + 1);
  if (!n->topic)
  {
    free(n);
    return;
  }
  if (tlen)
  {
    memcpy(n->topic, topic, tlen);
  }
  n->topic[tlen] = '\0';

  if (payload_len > 0 && payload)
  {
    n->payload = (uint8_t*)malloc((size_t)payload_len);
    if (!n->payload)
    {
      free(n->topic);
      free(n);
      return;
    }
    memcpy(n->payload, payload, (size_t)payload_len);
    n->payload_len = (size_t)payload_len;
  }

  n->has_message = true;
  n->msg.topic = n->topic;
  n->msg.payload = n->payload;
  n->msg.payload_len = n->payload_len;
  n->msg.qos = (az_iot_mqtt_qos)qos;
  n->msg.retain = (retain != 0);

  n->evt.kind = AZ_IOT_MQTT_EVT_MESSAGE;
  n->evt.message = &n->msg;

  if (v5_src)
  {
    extract_v5_props(n, v5_src);
  }

  q_push(m, n);
}

/* ------------------------------------------------------------------------- */
/* Paho callbacks (run on Paho threads - keep them tiny + queue-only)         */
/* ------------------------------------------------------------------------- */

/* Helper: copy `len` length-delimited bytes from Paho onto the heap as a C
 * string.
 *
 * Paho hands out MQTT UTF-8 strings as a pointer plus a length and does NOT
 * NUL-terminate them, so the terminator has to be added here before the bytes
 * can be exposed through az_iot_mqtt_message as a `const char*`.
 *
 * A zero-length string is a legal MQTT v5 property value and yields "", not
 * NULL: NULL is reserved for "allocation failed", and an application that
 * cannot tell an empty value from a failed one has no way to react to either.
 * Returns NULL only on allocation failure. */
static char* dup_str_n(const char* src, int len)
{
  size_t n = (src && len > 0) ? (size_t)len : 0u;
  char* d = (char*)malloc(n + 1);
  if (d)
  {
    if (n)
    {
      memcpy(d, src, n);
    }
    d[n] = '\0';
  }
  return d;
}

/* Extract MQTT v5 properties from a Paho MQTTAsync_message into the queued event. */
static void extract_v5_props(queued_event* n, MQTTAsync_message* msg)
{
  MQTTProperties* props = &msg->properties;
  if (!props || props->count == 0)
  {
    return;
  }

  /* Content Type */
  MQTTProperty* ct = MQTTProperties_getPropertyAt(props, MQTTPROPERTY_CODE_CONTENT_TYPE, 0);
  if (ct)
  {
    n->content_type = dup_str_n(ct->value.data.data, ct->value.data.len);
    n->msg.content_type = n->content_type;
  }

  /* Correlation Data */
  MQTTProperty* cd = MQTTProperties_getPropertyAt(props, MQTTPROPERTY_CODE_CORRELATION_DATA, 0);
  if (cd && cd->value.data.len > 0)
  {
    n->correlation_data = (uint8_t*)malloc((size_t)cd->value.data.len);
    if (n->correlation_data)
    {
      memcpy(n->correlation_data, cd->value.data.data, (size_t)cd->value.data.len);
      n->correlation_data_len = (size_t)cd->value.data.len;
      n->msg.correlation_data = n->correlation_data;
      n->msg.correlation_data_len = n->correlation_data_len;
    }
  }

  /* Message Expiry Interval */
  MQTTProperty* me
      = MQTTProperties_getPropertyAt(props, MQTTPROPERTY_CODE_MESSAGE_EXPIRY_INTERVAL, 0);
  if (me)
  {
    n->msg.message_expiry_seconds = (uint32_t)me->value.integer4;
  }

  /* User Properties — count then allocate */
  int up_count = MQTTProperties_propertyCount(props, MQTTPROPERTY_CODE_USER_PROPERTY);
  if (up_count > 0)
  {
    n->user_props
        = (az_iot_mqtt_user_property*)calloc((size_t)up_count, sizeof(az_iot_mqtt_user_property));
    if (n->user_props)
    {
      for (int i = 0; i < up_count; ++i)
      {
        MQTTProperty* up = MQTTProperties_getPropertyAt(props, MQTTPROPERTY_CODE_USER_PROPERTY, i);
        if (!up)
        {
          continue;
        }
        char* key = dup_str_n(up->value.data.data, up->value.data.len);
        char* value = dup_str_n(up->value.value.data, up->value.value.len);
        if (!key || !value)
        {
          /* Drop the pair whole rather than surface half of it: a NULL key or
           * value in an array the application is told has N entries is a
           * dereference waiting to happen. */
          free(key);
          free(value);
          continue;
        }
        n->user_props[n->user_props_count].key = key;
        n->user_props[n->user_props_count].value = value;
        n->user_props_count++;
      }
      n->msg.user_properties = n->user_props;
      n->msg.user_properties_count = n->user_props_count;
    }
  }
}

static int paho_msg_arrived(void* context, char* topic, int topic_len, MQTTAsync_message* msg)
{
  paho_client* m = (paho_client*)context;
  if (m && msg)
  {
    enqueue_message(
        m,
        topic,
        topic_len,
        msg->payload,
        msg->payloadlen,
        msg->qos,
        msg->retained,
        (m->version == AZ_IOT_MQTT_VERSION_5) ? msg : NULL);
  }
  MQTTAsync_freeMessage(&msg);
  MQTTAsync_free(topic);
  return 1; /* tell Paho we handled the message */
}

static void paho_connection_lost(void* context, char* cause)
{
  paho_client* m = (paho_client*)context;
  AZ_IOT_LOG_WARNF("paho: connection lost: %s", cause ? cause : "(unknown)");
  if (m)
  {
    enqueue_status(m, AZ_IOT_MQTT_EVT_DISCONNECTED, AZ_IOT_OK, 0);
  }
}

/* MQTT 5 server-initiated DISCONNECT. Paho routes these here rather than to the
 * connectionLost callback, so without this the app would never learn the server
 * tore the session down. Surface it as a DISCONNECTED event. */
static void paho_disconnected(
    void* context,
    MQTTProperties* properties,
    enum MQTTReasonCodes reasonCode)
{
  paho_client* m = (paho_client*)context;
  (void)properties;
  AZ_IOT_LOG_WARNF("paho: server sent DISCONNECT (reason %d)", (int)reasonCode);
  if (m)
  {
    /* Classified, not assumed clean. 0x00 is an ordinary close; anything from
     * 0x80 up is the server saying why it terminated the session, and
     * reporting that as AZ_IOT_OK left the core settling at IDLE with nothing
     * to explain it -- the reason code was the one thing that distinguished
     * "the hub closed us for quota exceeded" from a dropped socket. */
    enqueue_status_code(
        m,
        AZ_IOT_MQTT_EVT_DISCONNECTED,
        az_iot_mqtt_disconnect_result(m->version, (int)reasonCode),
        0,
        paho_wire_code((int)reasonCode));
  }
}

/* Paho's proxy-auth trace line, whose value is the credential.
 *
 * Paho logs "Setting http proxy auth to <base64>" (and the https variant) at
 * TRACE_PROTOCOL, where <base64> is the Basic credential -- reversible, so
 * forwarding it verbatim would write the proxy user name and password into the
 * application log of anyone who set AZ_IOT_PAHO_TRACE=protocol to debug a
 * connection. Match the text Paho uses and drop everything after it. */
#define PAHO_TRACE_PROXY_AUTH_MARKER "proxy auth to "

static void paho_trace_callback(enum MQTTASYNC_TRACE_LEVELS level, char* message)
{
  if (message != NULL)
  {
    const char* marker = strstr(message, PAHO_TRACE_PROXY_AUTH_MARKER);
    if (marker != NULL)
    {
      size_t keep = (size_t)(marker - message) + sizeof(PAHO_TRACE_PROXY_AUTH_MARKER) - 1u;
      AZ_IOT_LOG_TRACEF("paho: (%d) %.*s<redacted>", (int)level, (int)keep, message);
      return;
    }
  }
  AZ_IOT_LOG_TRACEF("paho: (%d) %s", (int)level, message ? message : "");
}

/* OpenSSL error handler wired into MQTTAsync_SSLOptions.ssl_error_cb. On a failed
 * TLS handshake Paho routes each OpenSSL error-queue line here (bad certificate,
 * chain/verify failure, protocol or cipher mismatch, ...), giving a concrete
 * reason instead of a generic connect failure. Enabled whenever AZ_IOT_PAHO_TRACE
 * is set.
 *
 * Guarded because its only call site is: without SSL there is no
 * MQTTAsync_SSLOptions to attach it to, and an unused static is -Werror here. */
#ifdef AZ_IOT_PAHO_SSL
static int paho_ssl_error_callback(const char* str, size_t len, void* u)
{
  (void)u;
  /* ERROR, not TRACE. Paho calls this only from SSLSocket_error(), i.e. only
   * when a TLS operation has already failed, and these lines are the sole
   * explanation of WHY -- the caller otherwise sees Paho's "TCP/TLS connect
   * failure", which names nothing. Emitting them below the failure they explain
   * meant they were dropped by any sink not set to TRACE, so the diagnostic
   * existed but never reached a log. The callback is installed only when
   * AZ_IOT_PAHO_TRACE is set, so this cannot add noise to a default build.
   *
   * OpenSSL hands these over one line at a time, already newline-terminated;
   * the sink adds its own framing, so trim the trailing newline.
   *
   * Bounded by `len` rather than by a NUL. OpenSSL's own ERR_print_errors_cb
   * does terminate the buffer it passes, but that is its choice and not part of
   * the callback contract -- `len` is what the contract gives us, so a producer
   * that passes an unterminated slice cannot walk us off the end. */
  if (!str || len == 0)
  {
    return 1;
  }
  const char* nl = (const char*)memchr(str, '\n', len);
  size_t n = nl ? (size_t)(nl - str) : len;
  if (n > (size_t)INT_MAX)
  {
    n = (size_t)INT_MAX;
  }
  AZ_IOT_LOG_ERRORF("paho ssl: %.*s", (int)n, str);
  return 1; /* keep draining the remaining OpenSSL error-queue lines */
}
#endif

/* Maps AZ_IOT_PAHO_TRACE to a Paho trace level, or returns -1 when unset/empty
 * (tracing disabled). Case-insensitive keywords, least to most verbose:
 * "error", "protocol", "min"/"minimum", "medium", "max"/"maximum". Any other
 * non-empty value (e.g. "1", "on") defaults to MINIMUM for backward compat. */
static int paho_trace_level_from_env(void)
{
  char buf[16];
#if defined(_WIN32)
  size_t len = 0;
  if (getenv_s(&len, buf, sizeof(buf), "AZ_IOT_PAHO_TRACE") != 0 || len == 0)
  {
    return -1;
  }
#else
  const char* env = getenv("AZ_IOT_PAHO_TRACE");
  if (!is_nonempty_cstr(env))
  {
    return -1;
  }
  size_t i = 0;
  for (; env[i] && i + 1 < sizeof(buf); ++i)
  {
    buf[i] = env[i];
  }
  buf[i] = '\0';
#endif
  for (char* p = buf; *p; ++p)
  {
    if (*p >= 'A' && *p <= 'Z')
    {
      *p = (char)(*p - 'A' + 'a');
    }
  }

  if (strcmp(buf, "max") == 0 || strcmp(buf, "maximum") == 0)
  {
    return MQTTASYNC_TRACE_MAXIMUM;
  }
  if (strcmp(buf, "medium") == 0)
  {
    return MQTTASYNC_TRACE_MEDIUM;
  }
  if (strcmp(buf, "protocol") == 0)
  {
    return MQTTASYNC_TRACE_PROTOCOL;
  }
  if (strcmp(buf, "error") == 0)
  {
    return MQTTASYNC_TRACE_ERROR;
  }
  return MQTTASYNC_TRACE_MINIMUM; /* "min"/"minimum"/"1"/anything else */
}

/* Enable Paho's library-level trace logging when AZ_IOT_PAHO_TRACE is set. The
 * value selects verbosity (see paho_trace_level_from_env); the same variable also
 * turns on verbose OpenSSL TLS error reporting via ssl_error_cb in connect().
 * Useful for diagnosing "connection lost: (unknown)" - the trace reveals the
 * underlying cause (socket error, server DISCONNECT, keep-alive timeout, etc.).
 * Idempotent: the callback/level are only installed once per process. */
static void paho_maybe_enable_trace(void)
{
  static bool s_trace_initialized = false;
  if (s_trace_initialized)
  {
    return;
  }
  s_trace_initialized = true;

  int level = paho_trace_level_from_env();
  if (level < 0)
  {
    return;
  }

  MQTTAsync_setTraceCallback(paho_trace_callback);
  MQTTAsync_setTraceLevel((enum MQTTASYNC_TRACE_LEVELS)level);
}

/* Paho reports a broker-side CONNACK rejection through nextOrClose(), which
 * fills failureData::code with the CONNACK return code and sets the message to
 * "CONNACK return code". Every other connect failure it reports here is one of
 * its own negative MQTTASYNC_* codes (socket refused, TLS handshake, bad
 * argument), and az_iot_mqtt_connack_result already treats negatives as
 * transport failures -- so the code can be handed over unfiltered. */
static az_iot_result paho_connect_failure_result(
    const paho_client* m,
    const MQTTAsync_failureData* response)
{
  if (!response)
  {
    return AZ_IOT_ERR_MQTT;
  }
  return az_iot_mqtt_connack_result(m->version, response->code);
}

/* Same idea for v5, with one wrinkle: on the CONNACK path Paho puts the v5
 * reason code in `code` and leaves `reasonCode` at its initializer
 * (MQTTREASONCODE_SUCCESS), while failures raised elsewhere do populate
 * `reasonCode`. Prefer `reasonCode` only when it actually holds a refusal
 * (>= 0x80), otherwise trust `code`. */
static int paho_connect_failure5_code(const MQTTAsync_failureData5* response)
{
  if (!response)
  {
    return 0;
  }
  return ((int)response->reasonCode >= 0x80) ? (int)response->reasonCode : response->code;
}

static az_iot_result paho_connect_failure5_result(
    const paho_client* m,
    const MQTTAsync_failureData5* response)
{
  if (!response)
  {
    return AZ_IOT_ERR_MQTT;
  }
  return az_iot_mqtt_connack_result(m->version, paho_connect_failure5_code(response));
}

/* Completion of a CLIENT-initiated disconnect.
 *
 * Paho reports a disconnect the peer caused -- a dropped link through
 * connectionLost(), an MQTT 5 server DISCONNECT through the disconnected()
 * callback -- but it reports the one WE asked for only through these
 * completion callbacks. Without them the caller is told nothing, and a caller
 * that waits for the session to settle waits for an event that never arrives.
 *
 * Both outcomes end the session, so both report DISCONNECTED -- the failure
 * carrying AZ_IOT_ERR_MQTT rather than AZ_IOT_OK, because the core surfaces
 * evt->status as the reason the connection ended.
 *
 * Only the v3 callbacks are set, and that is deliberate rather than an
 * oversight: for a DISCONNECT command Paho calls onSuccess when it is set and
 * falls back to onSuccess5 only when it is not (MQTTAsync_checkDisconnect in
 * MQTTAsyncUtils.c), so these fire for a v5 client too. Covered both ways by
 * the adapter unit tests. */
static void paho_disconnect_success(void* context, MQTTAsync_successData* response)
{
  (void)response;
  paho_client* m = (paho_client*)context;
  if (m)
  {
    enqueue_status(m, AZ_IOT_MQTT_EVT_DISCONNECTED, AZ_IOT_OK, 0);
  }
}

static void paho_disconnect_failure(void* context, MQTTAsync_failureData* response)
{
  paho_client* m = (paho_client*)context;
  int code = response ? response->code : 0;
  AZ_IOT_LOG_WARNF(
      "paho: disconnect failed: rc=%d msg=%s",
      code,
      (response && response->message) ? response->message : "(none)");
  if (m)
  {
    /* The session is over either way, so this is still DISCONNECTED -- but it
     * carries an error, not AZ_IOT_OK. The core reports evt->status as the
     * reason the connection ended, and calling a failed teardown a clean one
     * would tell the application the opposite of what happened.
     *
     * The code travels as the TRANSPORT code: this is Paho refusing the
     * teardown, not a verdict off the wire. */
    enqueue_status_codes(
        m, AZ_IOT_MQTT_EVT_DISCONNECTED, AZ_IOT_ERR_MQTT, 0, 0, paho_transport_code(code));
  }
}

static void paho_connect_success(void* context, MQTTAsync_successData* response)
{
  paho_client* m = (paho_client*)context;
  if (!m)
  {
    return;
  }
  /* Report Session Present from the CONNACK, as the v5 path does. MQTT 3.1.1
   * carries the flag too, and a caller that connects with Clean Session 0 --
   * which is what an MQTTv3 hub session does, so its queued cloud-to-device
   * messages survive a reconnect -- has no other way to learn whether the
   * broker actually resumed the session or quietly started a fresh one. */
  queued_event* n = (queued_event*)calloc(1, sizeof(*n));
  if (!n)
  {
    return;
  }
  n->evt.kind = AZ_IOT_MQTT_EVT_CONNECTED;
  n->evt.status = AZ_IOT_OK;
  n->evt.session_present = (response && response->alt.connect.sessionPresent) ? true : false;
  q_push(m, n);
}

static void paho_connect_failure(void* context, MQTTAsync_failureData* response)
{
  paho_client* m = (paho_client*)context;
  if (response)
  {
    AZ_IOT_LOG_ERRORF(
        "paho: connect failed: rc=%d msg=%s",
        response->code,
        response->message ? response->message : "(none)");
  }
  else
  {
    AZ_IOT_LOG_ERROR("paho: connect failed: (no response data)");
  }
  if (m)
  {
    int code = response ? response->code : 0;
    enqueue_status_codes(
        m,
        AZ_IOT_MQTT_EVT_CONNECTED,
        paho_connect_failure_result(m, response),
        0,
        paho_wire_code(code),
        paho_transport_code(code));
  }
}

static void paho_subscribe_success(void* context, MQTTAsync_successData* response)
{
  paho_client* m = (paho_client*)context;
  uint16_t pid = response ? (uint16_t)response->token : 0;
  if (m)
  {
    /* alt.qos is the granted QoS the server returned. 0x80 Failure is the only
     * refusal MQTT 3.1.1 can express, and it arrives here rather than on the
     * failure callback, so a SUBACK that denied the filter would otherwise be
     * reported to the core as a successful subscription. */
    int code = response ? response->alt.qos : 0;
    enqueue_status_code(
        m,
        AZ_IOT_MQTT_EVT_SUBSCRIBE_ACK,
        az_iot_mqtt_suback_result(m->version, code),
        pid,
        paho_wire_code(code));
  }
}

static void paho_subscribe_failure(void* context, MQTTAsync_failureData* response)
{
  paho_client* m = (paho_client*)context;
  uint16_t pid = response ? (uint16_t)response->token : 0;
  if (m)
  {
    int code = response ? response->code : -1;
    enqueue_status_codes(
        m,
        AZ_IOT_MQTT_EVT_SUBSCRIBE_ACK,
        az_iot_mqtt_suback_result(m->version, code),
        pid,
        paho_wire_code(code),
        paho_transport_code(code));
  }
}

static void paho_publish_success(void* context, MQTTAsync_successData* response)
{
  paho_client* m = (paho_client*)context;
  uint16_t pid = response ? (uint16_t)response->token : 0;
  if (m)
  {
    enqueue_status(m, AZ_IOT_MQTT_EVT_PUBLISH_ACK, AZ_IOT_OK, pid);
  }
}

static void paho_publish_failure(void* context, MQTTAsync_failureData* response)
{
  paho_client* m = (paho_client*)context;
  uint16_t pid = response ? (uint16_t)response->token : 0;
  if (m)
  {
    enqueue_status(m, AZ_IOT_MQTT_EVT_PUBLISH_ACK, AZ_IOT_ERR_MQTT, pid);
  }
}

/* MQTTv5 variants. Paho dispatches to onSuccess5/onFailure5 (not the v3
 * callbacks) when the underlying connection is v5, so adapters that talk v5
 * MUST register these. */
static void paho_connect_success5(void* context, MQTTAsync_successData5* response)
{
  paho_client* m = (paho_client*)context;
  if (!m)
  {
    return;
  }
  /* Enqueue CONNECTED event with session_present from CONNACK. */
  queued_event* n = (queued_event*)calloc(1, sizeof(*n));
  if (!n)
  {
    return;
  }
  n->evt.kind = AZ_IOT_MQTT_EVT_CONNECTED;
  n->evt.status = AZ_IOT_OK;
  n->evt.session_present = (response && response->alt.connect.sessionPresent) ? true : false;
  q_push(m, n);
}

static void paho_connect_failure5(void* context, MQTTAsync_failureData5* response)
{
  paho_client* m = (paho_client*)context;
  if (response)
  {
    AZ_IOT_LOG_ERRORF(
        "paho: connect5 failed: rc=%d reason_code=%d msg=%s",
        response->code,
        (int)response->reasonCode,
        response->message ? response->message : "(none)");
  }
  else
  {
    AZ_IOT_LOG_ERROR("paho: connect5 failed: (no response data)");
  }
  if (m)
  {
    int code5 = paho_connect_failure5_code(response);
    enqueue_status_codes(
        m,
        AZ_IOT_MQTT_EVT_CONNECTED,
        paho_connect_failure5_result(m, response),
        0,
        paho_wire_code(code5),
        /* A v5 CONNACK reason is >= 0 and lands in protocol_code; anything
         * negative is Paho's own failure and belongs here. */
        paho_transport_code(code5));
  }
}

static void paho_subscribe_success5(void* context, MQTTAsync_successData5* response)
{
  paho_client* m = (paho_client*)context;
  if (!m)
  {
    return;
  }
  uint16_t pid = response ? (uint16_t)response->token : 0;
  /* Paho invokes onSuccess5 whenever a SUBACK arrives, whatever it says, so the
   * reason code is the only thing separating a grant from a refusal here. */
  int code = response ? (int)response->reasonCode : 0;
  az_iot_result status = az_iot_mqtt_suback_result(m->version, code);
  if (status != AZ_IOT_OK)
  {
    AZ_IOT_LOG_WARNF("paho: SUBACK refused: reason_code=%d", code);
  }
  enqueue_status_code(m, AZ_IOT_MQTT_EVT_SUBSCRIBE_ACK, status, pid, paho_wire_code(code));
}

static void paho_subscribe_failure5(void* context, MQTTAsync_failureData5* response)
{
  paho_client* m = (paho_client*)context;
  uint16_t pid = response ? (uint16_t)response->token : 0;
  if (m)
  {
    int code = response
        ? (((int)response->reasonCode >= 0x80) ? (int)response->reasonCode : response->code)
        : -1;
    enqueue_status_codes(
        m,
        AZ_IOT_MQTT_EVT_SUBSCRIBE_ACK,
        az_iot_mqtt_suback_result(m->version, code),
        pid,
        paho_wire_code(code),
        paho_transport_code(code));
  }
}

static void paho_publish_success5(void* context, MQTTAsync_successData5* response)
{
  paho_client* m = (paho_client*)context;
  uint16_t pid = response ? (uint16_t)response->token : 0;
  if (m)
  {
    enqueue_status(m, AZ_IOT_MQTT_EVT_PUBLISH_ACK, AZ_IOT_OK, pid);
  }
}

static void paho_publish_failure5(void* context, MQTTAsync_failureData5* response)
{
  paho_client* m = (paho_client*)context;
  uint16_t pid = response ? (uint16_t)response->token : 0;
  if (m)
  {
    enqueue_status(m, AZ_IOT_MQTT_EVT_PUBLISH_ACK, AZ_IOT_ERR_MQTT, pid);
  }
}

/* ------------------------------------------------------------------------- */
/* iface vtable                                                              */
/* ------------------------------------------------------------------------- */

static char* dup_str(const char* s)
{
  if (!s)
  {
    return NULL;
  }
  size_t n = strlen(s) + 1;
  char* p = (char*)malloc(n);
  if (p)
  {
    memcpy(p, s, n);
  }
  return p;
}

/* Default broker port for a transport + TLS combination. */
static uint16_t default_port_for(bool ws, bool use_ssl)
{
  if (ws)
  {
    return use_ssl ? (uint16_t)AZ_IOT_MQTT_DEFAULT_PORT_WEBSOCKET_TLS
                   : (uint16_t)AZ_IOT_MQTT_DEFAULT_PORT_WEBSOCKET_PLAIN;
  }
  return use_ssl ? (uint16_t)AZ_IOT_MQTT_DEFAULT_PORT_TCP_TLS
                 : (uint16_t)AZ_IOT_MQTT_DEFAULT_PORT_TCP_PLAIN;
}

/* Build the serverURI Paho connects to. Reports why it failed rather than
 * collapsing every cause into NULL: a truncated URI and a failed allocation
 * need different answers, and a silently truncated host would be a connection
 * to the wrong endpoint. */
static az_iot_result build_server_uri(
    const char* host,
    uint16_t port,
    bool use_ssl,
    az_iot_mqtt_transport transport,
    const char* websocket_path,
    char** out_uri)
{
  *out_uri = NULL;
  if (!host)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  /* Compared against the one value that is not TCP rather than tested for
   * inequality with TCP: an unknown enum value must not become a WebSocket URI
   * either. The caller rejects it before reaching here; this stays defensive. */
  bool ws = (transport == AZ_IOT_MQTT_TRANSPORT_WEBSOCKET);
  const char* scheme = ws ? (use_ssl ? "wss://" : "ws://") : (use_ssl ? "ssl://" : "tcp://");
  const char* path = "";
  if (ws)
  {
    path = is_nonempty_cstr(websocket_path) ? websocket_path : AZ_IOT_MQTT_DEFAULT_WEBSOCKET_PATH;
  }
  if (!port)
  {
    port = default_port_for(ws, use_ssl);
  }
  /* scheme + host + ":" + 5-digit port + path + NUL */
  size_t n = strlen(scheme) + strlen(host) + 1 + 5 + strlen(path) + 1;
  char* uri = (char*)malloc(n);
  if (!uri)
  {
    return AZ_IOT_ERR_OUT_OF_MEMORY;
  }
  int written = snprintf(uri, n, "%s%s:%u%s", scheme, host, (unsigned)port, path);
  if (written < 0 || (size_t)written >= n)
  {
    free(uri);
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }
  *out_uri = uri;
  return AZ_IOT_OK;
}

/* How many bytes `s` needs once the characters Paho's proxy-credential parser
 * treats as syntax are percent-encoded. */
static size_t proxy_cred_encoded_len(const char* s)
{
  size_t n = 0;
  for (; *s != '\0'; ++s)
  {
    n += (*s == '%' || *s == '@') ? 3u : 1u;
  }
  return n;
}

/* Percent-encode '%' and '@' into `dst`, returning the end of what was written.
 *
 * Those two characters and no others, because those two are what Paho's parser
 * acts on (MQTTProtocol_setHTTPProxy / MQTTProtocol_specialChars in 1.3.13):
 * it splits the proxy string at the FIRST '@', so an '@' anywhere in a
 * credential silently moves the host; and it decodes %XX back to a byte, so a
 * literal '%' would be eaten or -- when the next two characters are not hex
 * digits -- leave that decoder advancing over nothing, hanging the connect.
 * Encoding both means Paho decodes exactly the credential that was configured. */
static char* proxy_cred_encode(char* dst, const char* s)
{
  static const char hex[] = "0123456789ABCDEF";
  for (; *s != '\0'; ++s)
  {
    unsigned char c = (unsigned char)*s;
    if (c == '%' || c == '@')
    {
      *dst++ = '%';
      *dst++ = hex[(c >> 4) & 0x0Fu];
      *dst++ = hex[c & 0x0Fu];
    }
    else
    {
      *dst++ = (char)c;
    }
  }
  return dst;
}

/* Render the proxy as the "[user:password@]host:port" string Paho expects.
 *
 * *out_uri is NULL when no proxy is configured, which is not an error. */
static az_iot_result build_proxy_uri(const az_iot_mqtt_proxy_options* proxy, char** out_uri)
{
  *out_uri = NULL;
  if (!proxy || !is_nonempty_cstr(proxy->host))
  {
    return AZ_IOT_OK;
  }
  const char* user = is_nonempty_cstr(proxy->username) ? proxy->username : NULL;
  const char* pass = (user != NULL && proxy->password != NULL) ? proxy->password : NULL;

  /* HTTP Basic splits the decoded credential at its first colon, so a colon in
   * the username is not representable -- percent-encoding it would not help,
   * because it is the DECODED byte the proxy splits on. Refuse it instead of
   * authenticating as some silently shorter user name. */
  if (user != NULL && strchr(user, ':') != NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }

  size_t n = strlen(proxy->host) + 1 + 5 + 1; /* host + ':' + port + NUL */
  if (user != NULL)
  {
    /* user + ':' + password + '@'. The colon is unconditional: Basic auth with
     * an empty password is "user:", and sending a bare "user" would make the
     * proxy read the whole string as the user name with no password at all. */
    n += proxy_cred_encoded_len(user) + 1 + 1;
    if (pass != NULL)
    {
      n += proxy_cred_encoded_len(pass);
    }
  }

  char* uri = (char*)malloc(n);
  if (!uri)
  {
    return AZ_IOT_ERR_OUT_OF_MEMORY;
  }

  char* p = uri;
  if (user != NULL)
  {
    p = proxy_cred_encode(p, user);
    *p++ = ':';
    if (pass != NULL)
    {
      p = proxy_cred_encode(p, pass);
    }
    *p++ = '@';
  }
  size_t used = (size_t)(p - uri);
  unsigned port = proxy->port ? (unsigned)proxy->port : (unsigned)AZ_IOT_MQTT_DEFAULT_PROXY_PORT;
  int written = snprintf(p, n - used, "%s:%u", proxy->host, port);
  if (written < 0 || (size_t)written >= n - used)
  {
    free(uri);
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }
  *out_uri = uri;
  return AZ_IOT_OK;
}

/* Point one set of Paho connect options at the proxy, on both the plaintext and
 * the TLS slot.
 *
 * Both, because Paho picks the slot by scheme -- httpProxy for tcp:// and ws://,
 * httpsProxy for ssl:// and wss:// -- while this SDK has a single proxy for the
 * connection, whatever the scheme ends up being. */
static void paho_apply_proxy(MQTTAsync_connectOptions* conn, const char* proxy_uri)
{
  if (!proxy_uri)
  {
    return;
  }
  conn->httpProxy = proxy_uri;
  conn->httpsProxy = proxy_uri;
}

static az_iot_result paho_iface_connect(
    az_iot_mqtt_client* self,
    const az_iot_mqtt_connect_options* opts)
{
  if (!self || !opts || !opts->host || !opts->client_id)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }

  /* Refuse a transport this adapter does not implement rather than falling back
   * to TCP. An unknown value here is a caller built against a newer header that
   * added a transport; connecting it straight out on 8883 would bypass whatever
   * egress restriction made it ask for something else, which is the one outcome
   * the fail-closed rule on az_iot_mqtt_connect_options exists to prevent. */
  if (opts->transport != AZ_IOT_MQTT_TRANSPORT_TCP
      && opts->transport != AZ_IOT_MQTT_TRANSPORT_WEBSOCKET)
  {
    AZ_IOT_LOG_ERRORF("paho: unsupported transport %d", (int)opts->transport);
    return AZ_IOT_ERR_NOT_SUPPORTED;
  }
  paho_client* m = paho_self(self);

  /* The DISCONNECT reason belongs to this session, so it is captured with the
   * rest of the connect options; a session that does not ask for one is closed
   * normally, which is what a zero yields. */
  m->disconnect_reason_code = opts->disconnect_reason_code;

  /* Use TLS when any TLS material or server verification is requested: a
   * client identity (cert), a server trust anchor (CA), their in-memory PEM
   * variants, an explicit use_tls, or a non-extractable key reference.
   * Keying off client_cert_path alone would wrongly fall back to plaintext for
   * server-auth-only connections, and omitting the key reference would let a
   * URI-only credential connect with no client key at all.
   *
   * Whether the session is TLS is the only choice here. Whether the server is
   * VALIDATED is not a choice: see the ssl_opts assignments below. */
  bool use_ssl = opts->tls.client_cert_path != NULL || opts->tls.client_cert_pem != NULL
      || opts->tls.trusted_ca_path != NULL || opts->tls.trusted_ca_pem != NULL || opts->tls.use_tls
      || az_iot_paho_key_custody_requested(&opts->tls);

  /* Resolve the client private key before anything else is built. A credential
   * that cannot possibly sign -- an unreachable HSM key, an engine that is not
   * installed -- fails here with a result that names the cause, instead of
   * dying inside the TLS handshake where the only evidence is an OpenSSL
   * alert. */
  az_iot_paho_key_custody_release(&m->key_custody);
  const char* private_key_path = NULL;
  az_iot_result key_rc
      = az_iot_paho_key_custody_prepare(&m->key_custody, &opts->tls, &private_key_path);
  if (key_rc != AZ_IOT_OK)
  {
    return key_rc;
  }

  /* (Re)build the underlying Paho handle. */
  if (m->paho)
  {
    MQTTAsync_destroy(&m->paho);
    m->paho = NULL;
  }
  free(m->server_uri);
  m->server_uri = NULL;
  free(m->proxy_uri);
  m->proxy_uri = NULL;
  free(m->client_id);
  m->client_id = NULL;

  az_iot_result uri_rc = build_server_uri(
      opts->host, opts->port, use_ssl, opts->transport, opts->websocket_path, &m->server_uri);
  if (uri_rc != AZ_IOT_OK)
  {
    az_iot_paho_key_custody_release(&m->key_custody);
    return uri_rc;
  }
  m->client_id = dup_str(opts->client_id);
  if (!m->client_id)
  {
    az_iot_paho_key_custody_release(&m->key_custody);
    return AZ_IOT_ERR_OUT_OF_MEMORY;
  }

  /* HTTP CONNECT proxy. Paho tunnels every scheme through it, not only
   * WebSockets, so this covers plain MQTT over TCP too.
   *
   * Owned by the client for the same reason server_uri is: it is rebuilt on
   * every connect and released on destroy, so no failure path in between has to
   * remember to free it.
   *
   * Fail closed: if a proxy was asked for and the string cannot be built, the
   * connect must not proceed, because Paho would then reach the broker
   * directly -- exactly what the caller ruled out. */
  az_iot_result proxy_rc = build_proxy_uri(&opts->proxy, &m->proxy_uri);
  if (proxy_rc != AZ_IOT_OK)
  {
    az_iot_paho_key_custody_release(&m->key_custody);
    return proxy_rc;
  }

  AZ_IOT_LOG_INFOF(
      /* proxy.host, never m->proxy_uri: the URI carries the Basic credential,
       * and this line is emitted at INFO on every connect. */
      "paho: connecting to %s as '%s' (proxy=%s)",
      m->server_uri,
      m->client_id,
      is_nonempty_cstr(opts->proxy.host) ? opts->proxy.host : "none");

  paho_maybe_enable_trace();

  MQTTAsync_createOptions create_opts = MQTTAsync_createOptions_initializer;
  create_opts.MQTTVersion
      = (m->version == AZ_IOT_MQTT_VERSION_5) ? MQTTVERSION_5 : MQTTVERSION_3_1_1;
  create_opts.sendWhileDisconnected = 0;

  int rc = MQTTAsync_createWithOptions(
      &m->paho, m->server_uri, m->client_id, MQTTCLIENT_PERSISTENCE_NONE, NULL, &create_opts);
  if (rc != MQTTASYNC_SUCCESS)
  {
    az_iot_paho_key_custody_release(&m->key_custody);
    return AZ_IOT_ERR_MQTT;
  }

  rc = MQTTAsync_setCallbacks(m->paho, m, paho_connection_lost, paho_msg_arrived, NULL);
  if (rc != MQTTASYNC_SUCCESS)
  {
    az_iot_paho_key_custody_release(&m->key_custody);
    return AZ_IOT_ERR_MQTT;
  }

  /* Surface a v5 server-initiated DISCONNECT (Paho routes these to the
   * disconnected callback, not connectionLost) as a DISCONNECTED event. */
  rc = MQTTAsync_setDisconnected(m->paho, m, paho_disconnected);
  if (rc != MQTTASYNC_SUCCESS)
  {
    az_iot_paho_key_custody_release(&m->key_custody);
    return AZ_IOT_ERR_MQTT;
  }

#ifdef AZ_IOT_PAHO_SSL
  MQTTAsync_SSLOptions ssl_opts = MQTTAsync_SSLOptions_initializer;
  if (use_ssl)
  {
    ssl_opts.trustStore = opts->tls.trusted_ca_path;
    ssl_opts.keyStore = opts->tls.client_cert_path;
    ssl_opts.privateKey = private_key_path;
    ssl_opts.privateKeyPassword = opts->tls.client_key_password;
    /* Unconditional, and there is no option that could make it otherwise: the
     * TLS options carry no "don't verify" flag. An unverified TLS session
     * authenticates nothing, and this SDK connects to Azure endpoints. The
     * slot that used to hold verify_server now only selects TLS (use_tls). */
    ssl_opts.enableServerCertAuth = 1;
    /* Verify the server hostname against the certificate too, not just the
     * chain: a chain-valid certificate issued for the wrong host must be
     * rejected. Paho checks X509_check_host and falls back to
     * X509_check_ip_asc for IP-literal peers. */
    ssl_opts.verify = 1;
    /* AZ_IOT_PAHO_TRACE also enables detailed OpenSSL handshake error output. */
    if (paho_trace_level_from_env() >= 0)
    {
      ssl_opts.ssl_error_cb = paho_ssl_error_callback;
    }
    AZ_IOT_LOG_DEBUGF(
        "paho: SSL trustStore=%s keyStore=%s privateKey=%s verboseErrors=%s keyCustody=%s",
        ssl_opts.trustStore ? ssl_opts.trustStore : "(none)",
        ssl_opts.keyStore ? ssl_opts.keyStore : "(none)",
        ssl_opts.privateKey ? ssl_opts.privateKey : "(none)",
        ssl_opts.ssl_error_cb ? "on" : "off",
        az_iot_paho_key_custody_requested(&opts->tls) ? "on" : "off");
  }
#else
  (void)private_key_path;
  if (use_ssl)
  {
    AZ_IOT_LOG_ERROR("paho: TLS requested but the adapter was built without SSL support");
    az_iot_paho_key_custody_release(&m->key_custody);
    return AZ_IOT_ERR_NOT_SUPPORTED;
  }
#endif

  if (m->version == AZ_IOT_MQTT_VERSION_5)
  {
    MQTTAsync_connectOptions conn = MQTTAsync_connectOptions_initializer5;
    conn.context = m;
    conn.onSuccess5 = paho_connect_success5;
    conn.onFailure5 = paho_connect_failure5;
    paho_apply_proxy(&conn, m->proxy_uri);
    conn.keepAliveInterval = opts->keep_alive_seconds ? opts->keep_alive_seconds : 60;
    conn.cleansession = 0;
    conn.cleanstart = opts->clean_start ? 1 : 0;
    conn.MQTTVersion = MQTTVERSION_5;
    if (opts->username)
    {
      conn.username = opts->username;
    }
    if (opts->password)
    {
      conn.password = opts->password;
    }

    /* Session Expiry Interval (v5 connect property). */
    MQTTProperties connect_props = MQTTProperties_initializer;
    if (opts->session_expiry_seconds > 0)
    {
      MQTTProperty sep;
      sep.identifier = MQTTPROPERTY_CODE_SESSION_EXPIRY_INTERVAL;
      sep.value.integer4 = (int)opts->session_expiry_seconds;
      MQTTProperties_add(&connect_props, &sep);
    }
    conn.connectProperties = &connect_props;

    /* LWT (Last Will and Testament). */
    MQTTAsync_willOptions will_opts = MQTTAsync_willOptions_initializer;
    MQTTProperties will_props = MQTTProperties_initializer;
    if (is_nonempty_cstr(opts->lwt.topic))
    {
      will_opts.topicName = opts->lwt.topic;
      will_opts.message = NULL; /* use struct payload */
      will_opts.qos = (int)opts->lwt.qos;
      will_opts.retained = opts->lwt.retain ? 1 : 0;
      will_opts.payload.data = (char*)(uintptr_t)opts->lwt.payload;
      will_opts.payload.len = (int)opts->lwt.payload_len;
      conn.will = &will_opts;

      /* Will Delay Interval (v5 will property). */
      if (opts->lwt.will_delay_seconds > 0)
      {
        MQTTProperty wdp;
        wdp.identifier = MQTTPROPERTY_CODE_WILL_DELAY_INTERVAL;
        wdp.value.integer4 = (int)opts->lwt.will_delay_seconds;
        MQTTProperties_add(&will_props, &wdp);
        conn.willProperties = &will_props;
      }
    }

#ifdef AZ_IOT_PAHO_SSL
    if (use_ssl)
    {
      conn.ssl = &ssl_opts;
    }
#endif
    rc = MQTTAsync_connect(m->paho, &conn);
    MQTTProperties_free(&connect_props);
    MQTTProperties_free(&will_props);
  }
  else
  {
    MQTTAsync_connectOptions conn = MQTTAsync_connectOptions_initializer;
    conn.context = m;
    conn.onSuccess = paho_connect_success;
    conn.onFailure = paho_connect_failure;
    paho_apply_proxy(&conn, m->proxy_uri);
    conn.keepAliveInterval = opts->keep_alive_seconds ? opts->keep_alive_seconds : 60;
    /* v3.1.1 has no separate Clean Start: the caller's clean_start maps onto
     * the Clean Session flag. This used to be hardcoded to 1, which quietly
     * defeated the whole point of the option -- IoT Hub only holds a device's
     * subscriptions and anything it queued while the device was away when the
     * session is NOT clean, so every reconnect started deaf and lost whatever
     * had arrived in the meantime. */
    conn.cleansession = opts->clean_start ? 1 : 0;
    conn.MQTTVersion = MQTTVERSION_3_1_1;
    if (opts->username)
    {
      conn.username = opts->username;
    }
    if (opts->password)
    {
      conn.password = opts->password;
    }

    /* LWT. v3.1.1 has no Will Delay Interval, so will_delay_seconds is
     * ignored here; the will fires as soon as the broker notices the drop. */
    MQTTAsync_willOptions will_opts = MQTTAsync_willOptions_initializer;
    if (is_nonempty_cstr(opts->lwt.topic))
    {
      will_opts.topicName = opts->lwt.topic;
      will_opts.message = NULL; /* use struct payload */
      will_opts.qos = (int)opts->lwt.qos;
      will_opts.retained = opts->lwt.retain ? 1 : 0;
      will_opts.payload.data = (char*)(uintptr_t)opts->lwt.payload;
      will_opts.payload.len = (int)opts->lwt.payload_len;
      conn.will = &will_opts;
    }
#ifdef AZ_IOT_PAHO_SSL
    if (use_ssl)
    {
      conn.ssl = &ssl_opts;
    }
#endif
    rc = MQTTAsync_connect(m->paho, &conn);
  }

  if (rc != MQTTASYNC_SUCCESS)
  {
    /* Only on failure. A connect that was accepted has NOT read the key yet:
     * MQTTAsync_connect is asynchronous and Paho opens the TLS session on its
     * own thread, so the reference file has to outlive this call. It is
     * released at the next connect and at destroy. */
    az_iot_paho_key_custody_release(&m->key_custody);
    return AZ_IOT_ERR_MQTT;
  }
  return AZ_IOT_OK;
}

static az_iot_result paho_iface_disconnect(az_iot_mqtt_client* self)
{
  if (!self)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  paho_client* m = paho_self(self);
  if (!m->paho)
  {
    return AZ_IOT_ERR_NOT_CONNECTED;
  }

  MQTTAsync_disconnectOptions opts = MQTTAsync_disconnectOptions_initializer;
  if (m->version == AZ_IOT_MQTT_VERSION_5)
  {
    /* The v5 initializer is a different struct_version: it is what makes Paho
     * write the reason code and the property field into the DISCONNECT. Taking
     * the v3.1.1 one and setting reasonCode would leave the code unsent. */
    MQTTAsync_disconnectOptions v5_opts = MQTTAsync_disconnectOptions_initializer5;
    opts = v5_opts;
    opts.reasonCode = (enum MQTTReasonCodes)m->disconnect_reason_code;
  }
  opts.timeout = 1000;
  /* Without these the disconnect completes silently: Paho raises
   * connectionLost() and disconnected() only for a disconnect the PEER caused,
   * so a client-initiated one produced no event at all and a caller waiting for
   * the session to settle waited forever. */
  opts.context = m;
  opts.onSuccess = paho_disconnect_success;
  opts.onFailure = paho_disconnect_failure;
  int rc = MQTTAsync_disconnect(m->paho, &opts);
  if (rc != MQTTASYNC_SUCCESS)
  {
    /* The call was refused, so neither callback will run. Report the end of the
     * session here instead, or the caller is left waiting on an event that can
     * no longer arrive -- with the error, for the reason above, and with the
     * refusal code as the transport code. */
    enqueue_status_codes(
        m, AZ_IOT_MQTT_EVT_DISCONNECTED, AZ_IOT_ERR_MQTT, 0, 0, paho_transport_code(rc));
    return AZ_IOT_ERR_MQTT;
  }
  return AZ_IOT_OK;
}

static az_iot_result paho_iface_subscribe(
    az_iot_mqtt_client* self,
    const char* topic_filter,
    az_iot_mqtt_qos qos,
    uint16_t* out_packet_id)
{
  if (!self || !topic_filter)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  paho_client* m = paho_self(self);
  if (!m->paho)
  {
    return AZ_IOT_ERR_NOT_CONNECTED;
  }

  MQTTAsync_responseOptions resp = MQTTAsync_responseOptions_initializer;
  resp.context = m;
  if (m->version == AZ_IOT_MQTT_VERSION_5)
  {
    resp.onSuccess5 = paho_subscribe_success5;
    resp.onFailure5 = paho_subscribe_failure5;
  }
  else
  {
    resp.onSuccess = paho_subscribe_success;
    resp.onFailure = paho_subscribe_failure;
  }
  int rc = MQTTAsync_subscribe(m->paho, topic_filter, (int)qos, &resp);
  if (out_packet_id)
  {
    *out_packet_id = (uint16_t)resp.token;
  }
  return (rc == MQTTASYNC_SUCCESS) ? AZ_IOT_OK : AZ_IOT_ERR_MQTT;
}

static az_iot_result paho_iface_unsubscribe(
    az_iot_mqtt_client* self,
    const char* topic_filter,
    uint16_t* out_packet_id)
{
  if (!self || !topic_filter)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  paho_client* m = paho_self(self);
  if (!m->paho)
  {
    return AZ_IOT_ERR_NOT_CONNECTED;
  }

  MQTTAsync_responseOptions resp = MQTTAsync_responseOptions_initializer;
  resp.context = m;
  int rc = MQTTAsync_unsubscribe(m->paho, topic_filter, &resp);
  if (out_packet_id)
  {
    *out_packet_id = (uint16_t)resp.token;
  }
  return (rc == MQTTASYNC_SUCCESS) ? AZ_IOT_OK : AZ_IOT_ERR_MQTT;
}

static az_iot_result paho_iface_publish(
    az_iot_mqtt_client* self,
    const az_iot_mqtt_message* msg,
    uint16_t* out_packet_id)
{
  if (!self || !msg || !msg->topic)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  paho_client* m = paho_self(self);
  if (!m->paho)
  {
    return AZ_IOT_ERR_NOT_CONNECTED;
  }

  MQTTAsync_message paho_msg = MQTTAsync_message_initializer;
  paho_msg.payload = (void*)msg->payload;
  paho_msg.payloadlen = (int)msg->payload_len;
  paho_msg.qos = (int)msg->qos;
  paho_msg.retained = msg->retain ? 1 : 0;

  /* MQTT v5: attach User Properties, Content Type, Correlation Data,
   * Message Expiry from the typed message fields. */
  MQTTProperties props = MQTTProperties_initializer;
  if (m->version == AZ_IOT_MQTT_VERSION_5)
  {
    if (is_nonempty_cstr(msg->content_type))
    {
      MQTTProperty p;
      p.identifier = MQTTPROPERTY_CODE_CONTENT_TYPE;
      p.value.data.data = (char*)(uintptr_t)msg->content_type;
      p.value.data.len = (int)strlen(msg->content_type);
      MQTTProperties_add(&props, &p);
    }
    if (msg->correlation_data && msg->correlation_data_len > 0)
    {
      MQTTProperty p;
      p.identifier = MQTTPROPERTY_CODE_CORRELATION_DATA;
      p.value.data.data = (char*)(uintptr_t)msg->correlation_data;
      p.value.data.len = (int)msg->correlation_data_len;
      MQTTProperties_add(&props, &p);
    }
    if (msg->message_expiry_seconds > 0)
    {
      MQTTProperty p;
      p.identifier = MQTTPROPERTY_CODE_MESSAGE_EXPIRY_INTERVAL;
      p.value.integer4 = (int)msg->message_expiry_seconds;
      MQTTProperties_add(&props, &p);
    }
    for (size_t i = 0; i < msg->user_properties_count; ++i)
    {
      const az_iot_mqtt_user_property* up = &msg->user_properties[i];
      if (!up->key)
      {
        continue;
      }
      MQTTProperty p;
      p.identifier = MQTTPROPERTY_CODE_USER_PROPERTY;
      p.value.data.data = (char*)(uintptr_t)up->key;
      p.value.data.len = (int)strlen(up->key);
      p.value.value.data = (char*)(uintptr_t)(up->value ? up->value : "");
      p.value.value.len = (int)(up->value ? strlen(up->value) : 0);
      MQTTProperties_add(&props, &p);
    }
    paho_msg.properties = props;
  }

  MQTTAsync_responseOptions resp = MQTTAsync_responseOptions_initializer;
  resp.context = m;
  if (m->version == AZ_IOT_MQTT_VERSION_5)
  {
    resp.onSuccess5 = paho_publish_success5;
    resp.onFailure5 = paho_publish_failure5;
  }
  else
  {
    resp.onSuccess = paho_publish_success;
    resp.onFailure = paho_publish_failure;
  }

  int rc = MQTTAsync_sendMessage(m->paho, msg->topic, &paho_msg, &resp);
  if (m->version == AZ_IOT_MQTT_VERSION_5)
  {
    MQTTProperties_free(&props);
  }
  if (out_packet_id)
  {
    *out_packet_id = (uint16_t)resp.token;
  }
  return (rc == MQTTASYNC_SUCCESS) ? AZ_IOT_OK : AZ_IOT_ERR_MQTT;
}

static az_iot_result paho_iface_process_loop(az_iot_mqtt_client* self, uint32_t timeout_ms)
{
  if (!self)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  paho_client* m = paho_self(self);

  /* Drain the queue. We dispatch in FIFO order; the inbound callback runs on
   * the caller's thread, satisfying the SDK's single-thread contract. */
  queued_event* n;
  bool dispatched = false;
  while ((n = q_pop(m)) != NULL)
  {
    dispatched = true;
    if (m->inbound_cb)
    {
      m->inbound_cb(&n->evt, m->inbound_ctx);
    }
    q_free(n);
  }

  /* If nothing was dispatched, sleep for the requested timeout so the caller's
   * do_work loop doesn't spin-wait while Paho's background thread handles I/O. */
  if (!dispatched && timeout_ms > 0)
  {
#if defined(_WIN32)
    Sleep(timeout_ms);
#else
    struct timespec ts
        = { .tv_sec = timeout_ms / 1000, .tv_nsec = (long)(timeout_ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
#endif
  }
  return AZ_IOT_OK;
}

static void paho_iface_set_inbound_cb(
    az_iot_mqtt_client* self,
    az_iot_mqtt_event_callback cb,
    void* user_ctx)
{
  if (!self)
  {
    return;
  }
  paho_client* m = paho_self(self);
  m->inbound_cb = cb;
  m->inbound_ctx = user_ctx;
}

static void paho_iface_destroy(az_iot_mqtt_client* self)
{
  if (!self)
  {
    return;
  }
  paho_client* m = paho_self(self);
  if (m->paho)
  {
    if (MQTTAsync_isConnected(m->paho))
    {
      MQTTAsync_disconnectOptions opts = MQTTAsync_disconnectOptions_initializer;
      opts.timeout = 100;
      (void)MQTTAsync_disconnect(m->paho, &opts);
    }
    MQTTAsync_destroy(&m->paho);
    m->paho = NULL;
  }
  az_iot_paho_key_custody_release(&m->key_custody);
  q_drain_all(m);
  paho_mutex_destroy(&m->q_mutex);
  free(m->server_uri);
  free(m->proxy_uri);
  free(m->client_id);
  free(m);
}

static const az_iot_mqtt_iface s_iface_template_v3
    = { AZ_IOT_MQTT_VERSION_3_1_1, paho_iface_connect,        paho_iface_disconnect,
        paho_iface_subscribe,      paho_iface_unsubscribe,    paho_iface_publish,
        paho_iface_process_loop,   paho_iface_set_inbound_cb, paho_iface_destroy };

static const az_iot_mqtt_iface s_iface_template_v5
    = { AZ_IOT_MQTT_VERSION_5,   paho_iface_connect,        paho_iface_disconnect,
        paho_iface_subscribe,    paho_iface_unsubscribe,    paho_iface_publish,
        paho_iface_process_loop, paho_iface_set_inbound_cb, paho_iface_destroy };

/* ------------------------------------------------------------------------- */
/* factory                                                                   */
/* ------------------------------------------------------------------------- */

typedef struct paho_factory_state
{
  az_iot_mqtt_factory public_;
} paho_factory_state;

static az_iot_mqtt_client* paho_factory_create(void* factory_ctx)
{
  paho_factory_state* st = (paho_factory_state*)factory_ctx;

  paho_client* m = (paho_client*)calloc(1, sizeof(*m));
  if (!m)
  {
    return NULL;
  }

  m->version = st->public_.version;
  m->iface_storage
      = (st->public_.version == AZ_IOT_MQTT_VERSION_5) ? s_iface_template_v5 : s_iface_template_v3;
  m->base.iface = &m->iface_storage;
  paho_mutex_init(&m->q_mutex);
  return &m->base;
}

static void paho_factory_cleanup(void* ctx) { free(ctx); }

static az_iot_mqtt_factory* build_factory(az_iot_mqtt_version v)
{
  paho_factory_state* st = (paho_factory_state*)calloc(1, sizeof(*st));
  if (!st)
  {
    return NULL;
  }
  st->public_.version = v;
  st->public_.create = paho_factory_create;
  st->public_.factory_ctx = st;
  st->public_.destroy = paho_factory_cleanup;
  return &st->public_;
}

az_iot_mqtt_factory* az_iot_paho_factory_create_v3_1_1(void)
{
  return build_factory(AZ_IOT_MQTT_VERSION_3_1_1);
}

az_iot_mqtt_factory* az_iot_paho_factory_create_v5(void)
{
  return build_factory(AZ_IOT_MQTT_VERSION_5);
}

void az_iot_paho_factory_destroy(az_iot_mqtt_factory* factory)
{
  if (!factory)
  {
    return;
  }
  paho_factory_state* st = (paho_factory_state*)factory->factory_ctx;
  free(st);
}
