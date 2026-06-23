// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/* SPDX-License-Identifier: MIT */
/* Paho-C MQTT adapter implementation.
 *
 * Design notes:
 * - We use Paho's MQTTAsync API. It handles both MQTTv3.1.1 and v5 via
 *   MQTTAsync_createWithOptions() + MQTTAsync_connectOptions::MQTTVersion.
 * - Paho fires its callbacks from internal threads. We marshal those events
 *   into a thread-safe FIFO; process_loop() drains the FIFO on the calling
 *   thread and invokes the user's az_iot_mqtt_event_cb. This keeps the
 *   single-threaded contract of API A intact without requiring callers to
 *   know about Paho's threading model.
 * - TLS is intentionally NOT wired in this phase (PAHO_WITH_SSL=OFF in the
 *   adapter CMake). The TLS path lands together with certificate_provider in a later
 *   phase; until then, connect() succeeds against tcp:// brokers only.
 */

#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#endif

#include "azure/iot/adapters/az_iot_adapter_paho.h"
#include "azure/iot/az_iot_result.h"
#include "azure/iot/az_iot_mqtt_iface.h"

#include <MQTTAsync.h>

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#if defined(_WIN32)
#  include <windows.h>
typedef CRITICAL_SECTION paho_mutex_t;
static void paho_mutex_init(paho_mutex_t* m)    { InitializeCriticalSection(m); }
static void paho_mutex_destroy(paho_mutex_t* m) { DeleteCriticalSection(m); }
static void paho_mutex_lock(paho_mutex_t* m)    { EnterCriticalSection(m); }
static void paho_mutex_unlock(paho_mutex_t* m)  { LeaveCriticalSection(m); }
#else
#  include <pthread.h>
#  include <time.h>
typedef pthread_mutex_t paho_mutex_t;
static void paho_mutex_init(paho_mutex_t* m)    { pthread_mutex_init(m, NULL); }
static void paho_mutex_destroy(paho_mutex_t* m) { pthread_mutex_destroy(m); }
static void paho_mutex_lock(paho_mutex_t* m)    { pthread_mutex_lock(m); }
static void paho_mutex_unlock(paho_mutex_t* m)  { pthread_mutex_unlock(m); }
#endif

/* ------------------------------------------------------------------------- */
/* event queue                                                               */
/* ------------------------------------------------------------------------- */

typedef struct queued_event_tag
{
    az_iot_mqtt_event_t evt;
    /* Backing storage for AZ_IOT_MQTT_EVT_MESSAGE events. */
    az_iot_mqtt_message_t msg;
    char*    topic;
    uint8_t* payload;
    size_t   payload_len;
    bool     has_message;
    /* v5 User Properties backing storage (heap-allocated). */
    az_iot_mqtt_user_property_t* user_props;
    size_t user_props_count;
    /* v5 Content Type (heap copy). */
    char* content_type;
    /* v5 Correlation Data (heap copy). */
    uint8_t* correlation_data;
    size_t   correlation_data_len;
    struct queued_event_tag* next;
} queued_event_t;

/* ------------------------------------------------------------------------- */
/* client state                                                              */
/* ------------------------------------------------------------------------- */

typedef struct paho_client_tag
{
    az_iot_mqtt_client_t base;          /* MUST be first */
    az_iot_mqtt_iface_t  iface_storage;
    az_iot_mqtt_version_t version;

    MQTTAsync paho;                  /* Paho async handle, NULL until connect() */
    char*     server_uri;            /* "tcp://host:port" */
    char*     client_id;

    az_iot_mqtt_event_cb inbound_cb;
    void*                    inbound_ctx;

    /* Thread-safe event queue. Producer = Paho callback threads. Consumer =
     * process_loop() on the user thread. */
    paho_mutex_t    q_mutex;
    queued_event_t* q_head;
    queued_event_t* q_tail;
} paho_client_t;

static paho_client_t* paho_self(az_iot_mqtt_client_t* c) { return (paho_client_t*)c; }

/* ------------------------------------------------------------------------- */
/* event queue helpers                                                       */
/* ------------------------------------------------------------------------- */

static void q_push(paho_client_t* m, queued_event_t* node)
{
    node->next = NULL;
    paho_mutex_lock(&m->q_mutex);
    if (m->q_tail) m->q_tail->next = node;
    else           m->q_head = node;
    m->q_tail = node;
    paho_mutex_unlock(&m->q_mutex);
}

static queued_event_t* q_pop(paho_client_t* m)
{
    paho_mutex_lock(&m->q_mutex);
    queued_event_t* n = m->q_head;
    if (n)
    {
        m->q_head = n->next;
        if (!m->q_head) m->q_tail = NULL;
    }
    paho_mutex_unlock(&m->q_mutex);
    if (n) n->next = NULL;
    return n;
}

static void q_free(queued_event_t* n)
{
    if (!n) return;
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

static void q_drain_all(paho_client_t* m)
{
    queued_event_t* n;
    while ((n = q_pop(m)) != NULL) q_free(n);
}

/* Allocate + enqueue a simple status event (no message payload). */
static void enqueue_status(paho_client_t* m,
                           az_iot_mqtt_event_kind_t kind,
                           az_iot_result_t status,
                           uint16_t packet_id)
{
    queued_event_t* n = (queued_event_t*)calloc(1, sizeof(*n));
    if (!n) return;
    n->evt.kind = kind;
    n->evt.status = status;
    n->evt.packet_id = packet_id;
    q_push(m, n);
}

/* Allocate + enqueue an inbound MESSAGE event. Topic + payload are deep-copied
 * so the queued event is self-contained. */
static void enqueue_message(paho_client_t* m,
                            const char* topic,
                            int topic_len,
                            const void* payload,
                            int payload_len,
                            int qos,
                            int retain)
{
    queued_event_t* n = (queued_event_t*)calloc(1, sizeof(*n));
    if (!n) return;

    /* Paho passes topic_len == 0 to mean "C string". */
    size_t tlen = (topic_len > 0) ? (size_t)topic_len : (topic ? strlen(topic) : 0);
    n->topic = (char*)malloc(tlen + 1);
    if (!n->topic) { free(n); return; }
    if (tlen) memcpy(n->topic, topic, tlen);
    n->topic[tlen] = '\0';

    if (payload_len > 0 && payload)
    {
        n->payload = (uint8_t*)malloc((size_t)payload_len);
        if (!n->payload) { free(n->topic); free(n); return; }
        memcpy(n->payload, payload, (size_t)payload_len);
        n->payload_len = (size_t)payload_len;
    }

    n->has_message = true;
    n->msg.topic = n->topic;
    n->msg.payload = n->payload;
    n->msg.payload_len = n->payload_len;
    n->msg.qos = (az_iot_mqtt_qos_t)qos;
    n->msg.retain = (retain != 0);

    n->evt.kind = AZ_IOT_MQTT_EVT_MESSAGE;
    n->evt.message = &n->msg;
    q_push(m, n);
}

/* ------------------------------------------------------------------------- */
/* Paho callbacks (run on Paho threads - keep them tiny + queue-only)         */
/* ------------------------------------------------------------------------- */

/* Helper: duplicate a binary blob (len bytes from src) onto the heap. */
static char* dup_str_n(const char* src, int len)
{
    if (!src || len <= 0) return NULL;
    char* d = (char*)malloc((size_t)len + 1);
    if (d) { memcpy(d, src, (size_t)len); d[len] = '\0'; }
    return d;
}

/* Extract MQTT v5 properties from a Paho MQTTAsync_message into the queued event. */
static void extract_v5_props(queued_event_t* n, MQTTAsync_message* msg)
{
    MQTTProperties* props = &msg->properties;
    if (!props || props->count == 0) return;

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
    MQTTProperty* me = MQTTProperties_getPropertyAt(props, MQTTPROPERTY_CODE_MESSAGE_EXPIRY_INTERVAL, 0);
    if (me)
    {
        n->msg.message_expiry_seconds = (uint32_t)me->value.integer4;
    }

    /* User Properties — count then allocate */
    int up_count = MQTTProperties_propertyCount(props, MQTTPROPERTY_CODE_USER_PROPERTY);
    if (up_count > 0)
    {
        n->user_props = (az_iot_mqtt_user_property_t*)calloc((size_t)up_count, sizeof(az_iot_mqtt_user_property_t));
        if (n->user_props)
        {
            for (int i = 0; i < up_count; ++i)
            {
                MQTTProperty* up = MQTTProperties_getPropertyAt(props, MQTTPROPERTY_CODE_USER_PROPERTY, i);
                if (!up) continue;
                n->user_props[n->user_props_count].key = dup_str_n(up->value.data.data, up->value.data.len);
                n->user_props[n->user_props_count].value = dup_str_n(up->value.value.data, up->value.value.len);
                n->user_props_count++;
            }
            n->msg.user_properties = n->user_props;
            n->msg.user_properties_count = n->user_props_count;
        }
    }
}

static int paho_msg_arrived(void* context, char* topic, int topic_len, MQTTAsync_message* msg)
{
    paho_client_t* m = (paho_client_t*)context;
    if (m && msg)
    {
        enqueue_message(m, topic, topic_len, msg->payload, msg->payloadlen, msg->qos, msg->retained);
        /* For v5: extract properties from the last enqueued event */
        if (m->version == AZ_IOT_MQTT_VERSION_5)
        {
            paho_mutex_lock(&m->q_mutex);
            queued_event_t* tail = m->q_tail;
            paho_mutex_unlock(&m->q_mutex);
            if (tail && tail->has_message)
            {
                extract_v5_props(tail, msg);
            }
        }
    }
    MQTTAsync_freeMessage(&msg);
    MQTTAsync_free(topic);
    return 1; /* tell Paho we handled the message */
}

static void paho_connection_lost(void* context, char* cause)
{
    paho_client_t* m = (paho_client_t*)context;
    fprintf(stderr, "[paho] connection lost: %s\n", cause ? cause : "(unknown)");
    if (m) enqueue_status(m, AZ_IOT_MQTT_EVT_DISCONNECTED, AZ_IOT_OK, 0);
}

static void paho_trace_callback(enum MQTTASYNC_TRACE_LEVELS level, char* message)
{
    fprintf(stderr, "[paho-trace] (%d) %s\n", (int)level, message ? message : "");
}

/* Enable Paho's library-level trace logging when AZ_IOT_PAHO_TRACE is set.
 * Useful for diagnosing "connection lost: (unknown)" - the trace reveals the
 * underlying cause (socket error, server DISCONNECT, keep-alive timeout, etc.).
 * Idempotent: the callback/level are only installed once per process. */
static void paho_maybe_enable_trace(void)
{
    static bool s_trace_initialized = false;
    if (s_trace_initialized) return;
    s_trace_initialized = true;

#if defined(_WIN32)
    char buf[16];
    size_t len = 0;
    if (getenv_s(&len, buf, sizeof(buf), "AZ_IOT_PAHO_TRACE") != 0 || len == 0) return;
#else
    const char* env = getenv("AZ_IOT_PAHO_TRACE");
    if (!env || !env[0]) return;
#endif

    MQTTAsync_setTraceCallback(paho_trace_callback);
    MQTTAsync_setTraceLevel(MQTTASYNC_TRACE_MINIMUM);
}

static void paho_connect_success(void* context, MQTTAsync_successData* response)
{
    (void)response;
    paho_client_t* m = (paho_client_t*)context;
    if (m) enqueue_status(m, AZ_IOT_MQTT_EVT_CONNECTED, AZ_IOT_OK, 0);
}

static void paho_connect_failure(void* context, MQTTAsync_failureData* response)
{
    paho_client_t* m = (paho_client_t*)context;
    if (response)
        fprintf(stderr, "[paho] connect failed: rc=%d msg=%s\n", response->code, response->message ? response->message : "(null)");
    else
        fprintf(stderr, "[paho] connect failed: (no response data)\n");
    if (m) enqueue_status(m, AZ_IOT_MQTT_EVT_CONNECTED, AZ_IOT_ERR_MQTT, 0);
}

static void paho_subscribe_success(void* context, MQTTAsync_successData* response)
{
    paho_client_t* m = (paho_client_t*)context;
    uint16_t pid = response ? (uint16_t)response->token : 0;
    if (m) enqueue_status(m, AZ_IOT_MQTT_EVT_SUBSCRIBE_ACK, AZ_IOT_OK, pid);
}

static void paho_subscribe_failure(void* context, MQTTAsync_failureData* response)
{
    paho_client_t* m = (paho_client_t*)context;
    uint16_t pid = response ? (uint16_t)response->token : 0;
    if (m) enqueue_status(m, AZ_IOT_MQTT_EVT_SUBSCRIBE_ACK, AZ_IOT_ERR_MQTT, pid);
}

static void paho_publish_success(void* context, MQTTAsync_successData* response)
{
    paho_client_t* m = (paho_client_t*)context;
    uint16_t pid = response ? (uint16_t)response->token : 0;
    if (m) enqueue_status(m, AZ_IOT_MQTT_EVT_PUBLISH_ACK, AZ_IOT_OK, pid);
}

static void paho_publish_failure(void* context, MQTTAsync_failureData* response)
{
    paho_client_t* m = (paho_client_t*)context;
    uint16_t pid = response ? (uint16_t)response->token : 0;
    if (m) enqueue_status(m, AZ_IOT_MQTT_EVT_PUBLISH_ACK, AZ_IOT_ERR_MQTT, pid);
}

/* MQTTv5 variants. Paho dispatches to onSuccess5/onFailure5 (not the v3
 * callbacks) when the underlying connection is v5, so adapters that talk v5
 * MUST register these. */
static void paho_connect_success5(void* context, MQTTAsync_successData5* response)
{
    paho_client_t* m = (paho_client_t*)context;
    if (!m) return;
    /* Enqueue CONNECTED event with session_present from CONNACK. */
    queued_event_t* n = (queued_event_t*)calloc(1, sizeof(*n));
    if (!n) return;
    n->evt.kind = AZ_IOT_MQTT_EVT_CONNECTED;
    n->evt.status = AZ_IOT_OK;
    n->evt.session_present = (response && response->alt.connect.sessionPresent) ? true : false;
    q_push(m, n);
}

static void paho_connect_failure5(void* context, MQTTAsync_failureData5* response)
{
    paho_client_t* m = (paho_client_t*)context;
    if (response)
        fprintf(stderr, "[paho] connect5 failed: rc=%d reason_code=%d msg=%s\n", response->code, (int)response->reasonCode, response->message ? response->message : "(null)");
    else
        fprintf(stderr, "[paho] connect5 failed: (no response data)\n");
    if (m) enqueue_status(m, AZ_IOT_MQTT_EVT_CONNECTED, AZ_IOT_ERR_MQTT, 0);
}

static void paho_subscribe_success5(void* context, MQTTAsync_successData5* response)
{
    paho_client_t* m = (paho_client_t*)context;
    uint16_t pid = response ? (uint16_t)response->token : 0;
    if (m) enqueue_status(m, AZ_IOT_MQTT_EVT_SUBSCRIBE_ACK, AZ_IOT_OK, pid);
}

static void paho_subscribe_failure5(void* context, MQTTAsync_failureData5* response)
{
    paho_client_t* m = (paho_client_t*)context;
    uint16_t pid = response ? (uint16_t)response->token : 0;
    if (m) enqueue_status(m, AZ_IOT_MQTT_EVT_SUBSCRIBE_ACK, AZ_IOT_ERR_MQTT, pid);
}

static void paho_publish_success5(void* context, MQTTAsync_successData5* response)
{
    paho_client_t* m = (paho_client_t*)context;
    uint16_t pid = response ? (uint16_t)response->token : 0;
    if (m) enqueue_status(m, AZ_IOT_MQTT_EVT_PUBLISH_ACK, AZ_IOT_OK, pid);
}

static void paho_publish_failure5(void* context, MQTTAsync_failureData5* response)
{
    paho_client_t* m = (paho_client_t*)context;
    uint16_t pid = response ? (uint16_t)response->token : 0;
    if (m) enqueue_status(m, AZ_IOT_MQTT_EVT_PUBLISH_ACK, AZ_IOT_ERR_MQTT, pid);
}

/* ------------------------------------------------------------------------- */
/* iface vtable                                                              */
/* ------------------------------------------------------------------------- */

static char* dup_str(const char* s)
{
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char* p = (char*)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

static char* build_server_uri(const char* host, uint16_t port, bool use_ssl)
{
    if (!host) return NULL;
    const char* scheme = use_ssl ? "ssl://" : "tcp://";
    /* "ssl://" or "tcp://" + host + ":" + 5-digit port + NUL */
    size_t n = strlen(scheme) + strlen(host) + 1 + 5 + 1;
    char* uri = (char*)malloc(n);
    if (!uri) return NULL;
    snprintf(uri, n, "%s%s:%u", scheme, host, (unsigned)(port ? port : (use_ssl ? 8883u : 1883u)));
    return uri;
}

static az_iot_result_t paho_iface_connect(az_iot_mqtt_client_t* self, const az_iot_mqtt_connect_options_t* opts)
{
    if (!self || !opts || !opts->host || !opts->client_id) return AZ_IOT_ERR_INVALID_ARG;
    paho_client_t* m = paho_self(self);

    /* Determine whether to use SSL based on TLS options being populated. */
    bool use_ssl = (opts->tls.client_cert_path != NULL);

    /* (Re)build the underlying Paho handle. */
    if (m->paho)
    {
        MQTTAsync_destroy(&m->paho);
        m->paho = NULL;
    }
    free(m->server_uri); m->server_uri = NULL;
    free(m->client_id);  m->client_id  = NULL;

    m->server_uri = build_server_uri(opts->host, opts->port, use_ssl);
    m->client_id  = dup_str(opts->client_id);
    if (!m->server_uri || !m->client_id) return AZ_IOT_ERR_OUT_OF_MEMORY;

    fprintf(stderr, "[paho] connecting to %s as '%s'\n", m->server_uri, m->client_id);

    paho_maybe_enable_trace();

    MQTTAsync_createOptions create_opts = MQTTAsync_createOptions_initializer;
    create_opts.MQTTVersion = (m->version == AZ_IOT_MQTT_VERSION_5) ? MQTTVERSION_5 : MQTTVERSION_3_1_1;
    create_opts.sendWhileDisconnected = 0;

    int rc = MQTTAsync_createWithOptions(&m->paho, m->server_uri, m->client_id,
                                         MQTTCLIENT_PERSISTENCE_NONE, NULL, &create_opts);
    if (rc != MQTTASYNC_SUCCESS) return AZ_IOT_ERR_MQTT;

    rc = MQTTAsync_setCallbacks(m->paho, m, paho_connection_lost, paho_msg_arrived, NULL);
    if (rc != MQTTASYNC_SUCCESS) return AZ_IOT_ERR_MQTT;

#ifdef AZ_IOT_PAHO_SSL
    MQTTAsync_SSLOptions ssl_opts = MQTTAsync_SSLOptions_initializer;
    if (use_ssl)
    {
        ssl_opts.trustStore         = opts->tls.trusted_ca_path;
        ssl_opts.keyStore           = opts->tls.client_cert_path;
        ssl_opts.privateKey         = opts->tls.client_key_path;
        ssl_opts.privateKeyPassword = opts->tls.client_key_password;
        ssl_opts.enableServerCertAuth = opts->tls.verify_server ? 1 : 0;
        fprintf(stderr, "[paho] SSL: trustStore=%s keyStore=%s privateKey=%s\n",
            ssl_opts.trustStore ? ssl_opts.trustStore : "(null)",
            ssl_opts.keyStore ? ssl_opts.keyStore : "(null)",
            ssl_opts.privateKey ? ssl_opts.privateKey : "(null)");
    }
#else
    if (use_ssl)
    {
        fprintf(stderr, "[paho] ERROR: TLS requested but adapter built without SSL support\n");
        return AZ_IOT_ERR_NOT_SUPPORTED;
    }
#endif

    if (m->version == AZ_IOT_MQTT_VERSION_5)
    {
        MQTTAsync_connectOptions conn = MQTTAsync_connectOptions_initializer5;
        conn.context = m;
        conn.onSuccess5 = paho_connect_success5;
        conn.onFailure5 = paho_connect_failure5;
        conn.keepAliveInterval = opts->keep_alive_seconds ? opts->keep_alive_seconds : 60;
        conn.cleansession = 0;
        conn.cleanstart   = opts->clean_start ? 1 : 0;
        conn.MQTTVersion  = MQTTVERSION_5;
        if (opts->username) conn.username = opts->username;
        if (opts->password) conn.password = opts->password;

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
        if (opts->lwt.topic && opts->lwt.topic[0])
        {
            will_opts.topicName = opts->lwt.topic;
            will_opts.message   = NULL; /* use struct payload */
            will_opts.qos       = (int)opts->lwt.qos;
            will_opts.retained  = opts->lwt.retain ? 1 : 0;
            will_opts.payload.data = (char*)(uintptr_t)opts->lwt.payload;
            will_opts.payload.len  = (int)opts->lwt.payload_len;
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
        if (use_ssl) conn.ssl = &ssl_opts;
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
        conn.keepAliveInterval = opts->keep_alive_seconds ? opts->keep_alive_seconds : 60;
        conn.cleansession = 1;
        conn.MQTTVersion = MQTTVERSION_3_1_1;
        if (opts->username) conn.username = opts->username;
        if (opts->password) conn.password = opts->password;
#ifdef AZ_IOT_PAHO_SSL
        if (use_ssl) conn.ssl = &ssl_opts;
#endif
        rc = MQTTAsync_connect(m->paho, &conn);
    }
    return (rc == MQTTASYNC_SUCCESS) ? AZ_IOT_OK : AZ_IOT_ERR_MQTT;
}

static az_iot_result_t paho_iface_disconnect(az_iot_mqtt_client_t* self)
{
    if (!self) return AZ_IOT_ERR_INVALID_ARG;
    paho_client_t* m = paho_self(self);
    if (!m->paho) return AZ_IOT_ERR_NOT_CONNECTED;

    MQTTAsync_disconnectOptions opts = MQTTAsync_disconnectOptions_initializer;
    opts.timeout = 1000;
    int rc = MQTTAsync_disconnect(m->paho, &opts);
    return (rc == MQTTASYNC_SUCCESS) ? AZ_IOT_OK : AZ_IOT_ERR_MQTT;
}

static az_iot_result_t paho_iface_subscribe(az_iot_mqtt_client_t* self,
                                                const char* topic_filter,
                                                az_iot_mqtt_qos_t qos,
                                                uint16_t* out_packet_id)
{
    if (!self || !topic_filter) return AZ_IOT_ERR_INVALID_ARG;
    paho_client_t* m = paho_self(self);
    if (!m->paho) return AZ_IOT_ERR_NOT_CONNECTED;

    MQTTAsync_responseOptions resp = MQTTAsync_responseOptions_initializer;
    resp.context    = m;
    if (m->version == AZ_IOT_MQTT_VERSION_5)
    {
        resp.onSuccess5 = paho_subscribe_success5;
        resp.onFailure5 = paho_subscribe_failure5;
    }
    else
    {
        resp.onSuccess  = paho_subscribe_success;
        resp.onFailure  = paho_subscribe_failure;
    }
    int rc = MQTTAsync_subscribe(m->paho, topic_filter, (int)qos, &resp);
    if (out_packet_id) *out_packet_id = (uint16_t)resp.token;
    return (rc == MQTTASYNC_SUCCESS) ? AZ_IOT_OK : AZ_IOT_ERR_MQTT;
}

static az_iot_result_t paho_iface_unsubscribe(az_iot_mqtt_client_t* self,
                                                  const char* topic_filter,
                                                  uint16_t* out_packet_id)
{
    if (!self || !topic_filter) return AZ_IOT_ERR_INVALID_ARG;
    paho_client_t* m = paho_self(self);
    if (!m->paho) return AZ_IOT_ERR_NOT_CONNECTED;

    MQTTAsync_responseOptions resp = MQTTAsync_responseOptions_initializer;
    resp.context = m;
    int rc = MQTTAsync_unsubscribe(m->paho, topic_filter, &resp);
    if (out_packet_id) *out_packet_id = (uint16_t)resp.token;
    return (rc == MQTTASYNC_SUCCESS) ? AZ_IOT_OK : AZ_IOT_ERR_MQTT;
}

static az_iot_result_t paho_iface_publish(az_iot_mqtt_client_t* self,
                                              const az_iot_mqtt_message_t* msg,
                                              uint16_t* out_packet_id)
{
    if (!self || !msg || !msg->topic) return AZ_IOT_ERR_INVALID_ARG;
    paho_client_t* m = paho_self(self);
    if (!m->paho) return AZ_IOT_ERR_NOT_CONNECTED;

    MQTTAsync_message paho_msg = MQTTAsync_message_initializer;
    paho_msg.payload    = (void*)msg->payload;
    paho_msg.payloadlen = (int)msg->payload_len;
    paho_msg.qos        = (int)msg->qos;
    paho_msg.retained   = msg->retain ? 1 : 0;

    /* MQTT v5: attach User Properties, Content Type, Correlation Data,
     * Message Expiry from the typed message fields. */
    MQTTProperties props = MQTTProperties_initializer;
    if (m->version == AZ_IOT_MQTT_VERSION_5)
    {
        if (msg->content_type && msg->content_type[0])
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
            const az_iot_mqtt_user_property_t* up = &msg->user_properties[i];
            if (!up->key) continue;
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
    resp.context   = m;
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
    if (out_packet_id) *out_packet_id = (uint16_t)resp.token;
    return (rc == MQTTASYNC_SUCCESS) ? AZ_IOT_OK : AZ_IOT_ERR_MQTT;
}

static az_iot_result_t paho_iface_process_loop(az_iot_mqtt_client_t* self, uint32_t timeout_ms)
{
    if (!self) return AZ_IOT_ERR_INVALID_ARG;
    paho_client_t* m = paho_self(self);

    /* Drain the queue. We dispatch in FIFO order; the inbound callback runs on
     * the caller's thread, satisfying the API A single-thread contract. */
    queued_event_t* n;
    bool dispatched = false;
    while ((n = q_pop(m)) != NULL)
    {
        dispatched = true;
        if (m->inbound_cb) m->inbound_cb(&n->evt, m->inbound_ctx);
        q_free(n);
    }

    /* If nothing was dispatched, sleep for the requested timeout so the caller's
     * do_work loop doesn't spin-wait while Paho's background thread handles I/O. */
    if (!dispatched && timeout_ms > 0)
    {
#if defined(_WIN32)
        Sleep(timeout_ms);
#else
        struct timespec ts = { .tv_sec = timeout_ms / 1000, .tv_nsec = (long)(timeout_ms % 1000) * 1000000L };
        nanosleep(&ts, NULL);
#endif
    }
    return AZ_IOT_OK;
}

static void paho_iface_set_inbound_cb(az_iot_mqtt_client_t* self, az_iot_mqtt_event_cb cb, void* user_ctx)
{
    if (!self) return;
    paho_client_t* m = paho_self(self);
    m->inbound_cb  = cb;
    m->inbound_ctx = user_ctx;
}

static void paho_iface_destroy(az_iot_mqtt_client_t* self)
{
    if (!self) return;
    paho_client_t* m = paho_self(self);
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
    q_drain_all(m);
    paho_mutex_destroy(&m->q_mutex);
    free(m->server_uri);
    free(m->client_id);
    free(m);
}

static const az_iot_mqtt_iface_t s_iface_template_v3 = {
    AZ_IOT_MQTT_VERSION_3_1_1,
    paho_iface_connect, paho_iface_disconnect, paho_iface_subscribe, paho_iface_unsubscribe,
    paho_iface_publish, paho_iface_process_loop, paho_iface_set_inbound_cb, paho_iface_destroy
};

static const az_iot_mqtt_iface_t s_iface_template_v5 = {
    AZ_IOT_MQTT_VERSION_5,
    paho_iface_connect, paho_iface_disconnect, paho_iface_subscribe, paho_iface_unsubscribe,
    paho_iface_publish, paho_iface_process_loop, paho_iface_set_inbound_cb, paho_iface_destroy
};

/* ------------------------------------------------------------------------- */
/* factory                                                                   */
/* ------------------------------------------------------------------------- */

typedef struct paho_factory_state_tag
{
    az_iot_mqtt_factory_t public_;
} paho_factory_state_t;

static az_iot_mqtt_client_t* paho_factory_create(void* factory_ctx)
{
    paho_factory_state_t* st = (paho_factory_state_t*)factory_ctx;

    paho_client_t* m = (paho_client_t*)calloc(1, sizeof(*m));
    if (!m) return NULL;

    m->version = st->public_.version;
    m->iface_storage = (st->public_.version == AZ_IOT_MQTT_VERSION_5) ? s_iface_template_v5 : s_iface_template_v3;
    m->base.iface = &m->iface_storage;
    paho_mutex_init(&m->q_mutex);
    return &m->base;
}

static void paho_factory_cleanup(void* ctx) { free(ctx); }

static az_iot_mqtt_factory_t* build_factory(az_iot_mqtt_version_t v)
{
    paho_factory_state_t* st = (paho_factory_state_t*)calloc(1, sizeof(*st));
    if (!st) return NULL;
    st->public_.version = v;
    st->public_.create = paho_factory_create;
    st->public_.factory_ctx = st;
    st->public_.destroy = paho_factory_cleanup;
    return &st->public_;
}

az_iot_mqtt_factory_t* az_iot_paho_factory_create_v3_1_1(void)
{
    return build_factory(AZ_IOT_MQTT_VERSION_3_1_1);
}

az_iot_mqtt_factory_t* az_iot_paho_factory_create_v5(void)
{
    return build_factory(AZ_IOT_MQTT_VERSION_5);
}

void az_iot_paho_factory_destroy(az_iot_mqtt_factory_t* factory)
{
    if (!factory) return;
    paho_factory_state_t* st = (paho_factory_state_t*)factory->factory_ctx;
    free(st);
}
