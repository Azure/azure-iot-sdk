// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* esp-mqtt implementation of the azure-iot-sdk az_iot_mqtt_iface vtable.
 *
 * esp-mqtt runs its own background task and delivers events through an
 * esp_event handler that fires on THAT task. The SDK, however, is a
 * single-threaded pump: every inbound callback must fire on the thread that
 * calls process_loop(). We therefore follow the same pattern as the Paho
 * adapter — the esp-mqtt event handler only deep-copies the event into a
 * mutex-protected FIFO, and process_loop() drains that FIFO on the SDK thread.
 *
 * Credentials are taken from the in-memory PEM fields of az_iot_mqtt_tls_options
 * (client_cert_pem / client_key_pem / trusted_ca_pem); esp-mqtt loads them from
 * memory, so the device needs no filesystem.
 */
#include "az_iot_mqtt_esp.h"

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mqtt_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"

static const char* TAG = "az_mqtt_esp";

/* ------------------------------------------------------------------------- */
/* queued event                                                              */
/* ------------------------------------------------------------------------- */

typedef struct queued_event
{
  az_iot_mqtt_event evt;
  az_iot_mqtt_message msg; /* backing storage for evt.message */
  char* topic;
  uint8_t* payload;
  size_t payload_len;
  /* v5 inbound property backing storage (all optional). */
  char* content_type;
  char* response_topic;
  uint8_t* correlation_data;
  size_t correlation_data_len;
  az_iot_mqtt_user_property* user_props;
  size_t user_props_count;
  struct queued_event* next;
} queued_event;

typedef struct esp_client
{
  az_iot_mqtt_client base; /* MUST be first */
  az_iot_mqtt_iface iface_storage;
  az_iot_mqtt_version version;

  esp_mqtt_client_handle_t handle; /* NULL until connect() */

  az_iot_mqtt_event_callback inbound_cb;
  void* inbound_ctx;

  SemaphoreHandle_t q_mutex;
  queued_event* q_head;
  queued_event* q_tail;

  /* Reassembly buffer for fragmented inbound PUBLISH payloads (esp-mqtt
   * delivers payloads larger than its RX buffer across several DATA events).
   * The topic only accompanies the first fragment. */
  char* asm_topic;
  uint8_t* asm_buf;
  size_t asm_total;
  size_t asm_filled;
  int asm_qos;
  int asm_retain;
} esp_client;

static esp_client* esp_self(az_iot_mqtt_client* c) { return (esp_client*)c; }

/* ------------------------------------------------------------------------- */
/* queue helpers                                                             */
/* ------------------------------------------------------------------------- */

static void q_push(esp_client* m, queued_event* n)
{
  n->next = NULL;
  xSemaphoreTake(m->q_mutex, portMAX_DELAY);
  if (m->q_tail)
    m->q_tail->next = n;
  else
    m->q_head = n;
  m->q_tail = n;
  xSemaphoreGive(m->q_mutex);
}

static queued_event* q_pop(esp_client* m)
{
  xSemaphoreTake(m->q_mutex, portMAX_DELAY);
  queued_event* n = m->q_head;
  if (n)
  {
    m->q_head = n->next;
    if (!m->q_head)
      m->q_tail = NULL;
  }
  xSemaphoreGive(m->q_mutex);
  if (n)
    n->next = NULL;
  return n;
}

static void q_free(queued_event* n)
{
  if (!n)
    return;
  free(n->topic);
  free(n->payload);
  free(n->content_type);
  free(n->response_topic);
  free(n->correlation_data);
  if (n->user_props)
  {
    for (size_t i = 0; i < n->user_props_count; ++i)
    {
      free((void*)n->user_props[i].key);
      free((void*)n->user_props[i].value);
    }
    free(n->user_props);
  }
  free(n);
}

static void q_drain_all(esp_client* m)
{
  queued_event* n;
  while ((n = q_pop(m)) != NULL)
    q_free(n);
}

static void enqueue_status(
    esp_client* m,
    az_iot_mqtt_event_kind kind,
    az_iot_result status,
    uint16_t packet_id,
    bool session_present)
{
  queued_event* n = (queued_event*)calloc(1, sizeof(*n));
  if (!n)
    return;
  n->evt.kind = kind;
  n->evt.status = status;
  n->evt.packet_id = packet_id;
  n->evt.session_present = session_present;
  q_push(m, n);
}

/* ------------------------------------------------------------------------- */
/* v5 inbound property extraction                                            */
/* ------------------------------------------------------------------------- */

#if defined(CONFIG_MQTT_PROTOCOL_5)
static char* dup_n(const char* src, int len)
{
  if (!src || len <= 0)
    return NULL;
  char* d = (char*)malloc((size_t)len + 1);
  if (d)
  {
    memcpy(d, src, (size_t)len);
    d[len] = '\0';
  }
  return d;
}

static void extract_v5_props(queued_event* n, esp_mqtt_event_handle_t e)
{
  esp_mqtt5_event_property_t* p = e->property;
  if (!p)
    return;

  if (p->content_type && p->content_type_len)
  {
    n->content_type = dup_n(p->content_type, p->content_type_len);
    n->msg.content_type = n->content_type;
  }
  if (p->response_topic && p->response_topic_len)
  {
    n->response_topic = dup_n(p->response_topic, p->response_topic_len);
    n->msg.response_topic = n->response_topic;
  }
  if (p->correlation_data && p->correlation_data_len)
  {
    n->correlation_data = (uint8_t*)malloc(p->correlation_data_len);
    if (n->correlation_data)
    {
      memcpy(n->correlation_data, p->correlation_data, p->correlation_data_len);
      n->correlation_data_len = p->correlation_data_len;
      n->msg.correlation_data = n->correlation_data;
      n->msg.correlation_data_len = n->correlation_data_len;
    }
  }

  /* User properties: query count, then pull each item. */
  uint8_t count = 0;
  if (p->user_property
      && esp_mqtt5_client_get_user_property(p->user_property, NULL, &count) == ESP_OK && count > 0)
  {
    esp_mqtt5_user_property_item_t* items
        = (esp_mqtt5_user_property_item_t*)calloc(count, sizeof(*items));
    az_iot_mqtt_user_property* out = (az_iot_mqtt_user_property*)calloc(count, sizeof(*out));
    if (items && out
        && esp_mqtt5_client_get_user_property(p->user_property, items, &count) == ESP_OK)
    {
      size_t kept = 0;
      for (uint8_t i = 0; i < count; ++i)
      {
        char* k = items[i].key ? strdup(items[i].key) : NULL;
        char* v = items[i].value ? strdup(items[i].value) : NULL;
        if (k && v)
        {
          out[kept].key = k;
          out[kept].value = v;
          ++kept;
        }
        else
        {
          free(k);
          free(v);
        }
      }
      n->user_props = out;
      n->user_props_count = kept;
      n->msg.user_properties = out;
      n->msg.user_properties_count = kept;
      out = NULL;
    }
    if (items)
      esp_mqtt5_client_delete_user_property(p->user_property);
    free(items);
    free(out);
  }
}
#endif /* CONFIG_MQTT_PROTOCOL_5 */

/* ------------------------------------------------------------------------- */
/* inbound message assembly + enqueue                                        */
/* ------------------------------------------------------------------------- */

static void asm_reset(esp_client* m)
{
  free(m->asm_topic);
  free(m->asm_buf);
  m->asm_topic = NULL;
  m->asm_buf = NULL;
  m->asm_total = m->asm_filled = 0;
}

/* Finalize a fully-reassembled message into a queued event. Takes ownership of
 * asm_topic/asm_buf. */
static void enqueue_message(esp_client* m, esp_mqtt_event_handle_t e)
{
  queued_event* n = (queued_event*)calloc(1, sizeof(*n));
  if (!n)
  {
    asm_reset(m);
    return;
  }

  n->topic = m->asm_topic; /* transfer ownership */
  n->payload = m->asm_buf;
  n->payload_len = m->asm_filled;
  m->asm_topic = NULL;
  m->asm_buf = NULL;

  n->msg.topic = n->topic;
  n->msg.payload = n->payload;
  n->msg.payload_len = n->payload_len;
  n->msg.qos = (az_iot_mqtt_qos)m->asm_qos;
  n->msg.retain = (m->asm_retain != 0);

#if defined(CONFIG_MQTT_PROTOCOL_5)
  if (m->version == AZ_IOT_MQTT_VERSION_5)
  {
    extract_v5_props(n, e);
  }
#else
  (void)e;
#endif

  n->evt.kind = AZ_IOT_MQTT_EVT_MESSAGE;
  n->evt.message = &n->msg;
  q_push(m, n);
  asm_reset(m);
}

static void on_data(esp_client* m, esp_mqtt_event_handle_t e)
{
  /* First fragment of a new message: capture topic + allocate the full
   * reassembly buffer (total_data_len is the complete payload size). */
  if (e->current_data_offset == 0)
  {
    asm_reset(m);
    size_t total = (e->total_data_len > 0) ? (size_t)e->total_data_len : (size_t)e->data_len;
    if (e->topic_len > 0 && e->topic)
    {
      m->asm_topic = (char*)malloc((size_t)e->topic_len + 1);
      if (m->asm_topic)
      {
        memcpy(m->asm_topic, e->topic, e->topic_len);
        m->asm_topic[e->topic_len] = '\0';
      }
    }
    m->asm_buf = (uint8_t*)malloc(total ? total : 1);
    m->asm_total = total;
    m->asm_filled = 0;
    m->asm_qos = e->qos;
    m->asm_retain = e->retain;
    if (!m->asm_buf)
    {
      asm_reset(m);
      return;
    }
  }

  if (m->asm_buf && e->data_len > 0
      && (size_t)e->current_data_offset + (size_t)e->data_len <= m->asm_total)
  {
    memcpy(m->asm_buf + e->current_data_offset, e->data, e->data_len);
    m->asm_filled = (size_t)e->current_data_offset + (size_t)e->data_len;
  }

  if (m->asm_buf && m->asm_filled >= m->asm_total)
  {
    enqueue_message(m, e);
  }
}

/* ------------------------------------------------------------------------- */
/* esp-mqtt event handler (runs on the esp-mqtt task — queue only)            */
/* ------------------------------------------------------------------------- */

static void mqtt_event_handler(
    void* args,
    esp_event_base_t base,
    int32_t event_id,
    void* event_data)
{
  (void)base;
  esp_client* m = (esp_client*)args;
  esp_mqtt_event_handle_t e = (esp_mqtt_event_handle_t)event_data;

  switch ((esp_mqtt_event_id_t)event_id)
  {
    case MQTT_EVENT_CONNECTED:
      enqueue_status(m, AZ_IOT_MQTT_EVT_CONNECTED, AZ_IOT_OK, 0, e->session_present);
      break;
    case MQTT_EVENT_DISCONNECTED:
      enqueue_status(m, AZ_IOT_MQTT_EVT_DISCONNECTED, AZ_IOT_OK, 0, false);
      break;
    case MQTT_EVENT_SUBSCRIBED:
      enqueue_status(m, AZ_IOT_MQTT_EVT_SUBSCRIBE_ACK, AZ_IOT_OK, (uint16_t)e->msg_id, false);
      break;
    case MQTT_EVENT_UNSUBSCRIBED:
      enqueue_status(m, AZ_IOT_MQTT_EVT_UNSUBSCRIBE_ACK, AZ_IOT_OK, (uint16_t)e->msg_id, false);
      break;
    case MQTT_EVENT_PUBLISHED:
      enqueue_status(m, AZ_IOT_MQTT_EVT_PUBLISH_ACK, AZ_IOT_OK, (uint16_t)e->msg_id, false);
      break;
    case MQTT_EVENT_DATA:
      on_data(m, e);
      break;
    case MQTT_EVENT_ERROR:
      enqueue_status(m, AZ_IOT_MQTT_EVT_ERROR, AZ_IOT_ERR_MQTT, 0, false);
      break;
    default:
      break;
  }
}

/* ------------------------------------------------------------------------- */
/* iface vtable                                                              */
/* ------------------------------------------------------------------------- */

static az_iot_result esp_connect(az_iot_mqtt_client* self, const az_iot_mqtt_connect_options* opts)
{
  esp_client* m = esp_self(self);
  if (m->handle)
    return AZ_IOT_ERR_ALREADY_INITIALIZED;

  esp_mqtt_client_config_t cfg = { 0 };
  cfg.broker.address.hostname = opts->host;
  cfg.broker.address.port = opts->port ? opts->port : 8883;
  cfg.broker.address.transport = MQTT_TRANSPORT_OVER_SSL;
  /* Use a caller-supplied CA PEM when present, otherwise fall back to the
   * ESP-IDF certificate bundle (which already trusts the Azure roots). */
  if (opts->tls.trusted_ca_pem && opts->tls.trusted_ca_pem[0] == '-')
  {
    cfg.broker.verification.certificate = opts->tls.trusted_ca_pem;
  }
  else
  {
    cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
  }
  cfg.broker.verification.skip_cert_common_name_check = false;

  cfg.credentials.client_id = opts->client_id;
  cfg.credentials.username = opts->username;
  cfg.credentials.authentication.certificate = opts->tls.client_cert_pem;
  cfg.credentials.authentication.key = opts->tls.client_key_pem;
  if (opts->tls.client_key_password)
  {
    cfg.credentials.authentication.key_password = (char*)opts->tls.client_key_password;
    cfg.credentials.authentication.key_password_len = (int)strlen(opts->tls.client_key_password);
  }

  cfg.session.keepalive = opts->keep_alive_seconds ? opts->keep_alive_seconds : 30;
  cfg.session.disable_clean_session = !opts->clean_start;
  cfg.session.protocol_ver
      = (m->version == AZ_IOT_MQTT_VERSION_5) ? MQTT_PROTOCOL_V_5 : MQTT_PROTOCOL_V_3_1_1;

  /* The SDK owns reconnect (backoff/jitter); keep esp-mqtt from racing it. */
  cfg.network.disable_auto_reconnect = true;
  /* esp-mqtt wants milliseconds; the SDK option is in seconds. */
  cfg.network.timeout_ms
      = opts->connect_timeout_seconds ? (int)(opts->connect_timeout_seconds * 1000) : 30000;

  /* A roomy RX buffer keeps multi-KB twin/ADU payloads in a single fragment;
   * on_data() still reassembles if a payload exceeds it. */
  cfg.buffer.size = 4096;

  m->handle = esp_mqtt_client_init(&cfg);
  if (!m->handle)
  {
    ESP_LOGE(TAG, "esp_mqtt_client_init failed");
    return AZ_IOT_ERR_MQTT;
  }

  esp_mqtt_client_register_event(m->handle, ESP_EVENT_ANY_ID, mqtt_event_handler, m);

  if (esp_mqtt_client_start(m->handle) != ESP_OK)
  {
    ESP_LOGE(TAG, "esp_mqtt_client_start failed");
    esp_mqtt_client_destroy(m->handle);
    m->handle = NULL;
    return AZ_IOT_ERR_MQTT;
  }
  return AZ_IOT_OK;
}

static az_iot_result esp_disconnect(az_iot_mqtt_client* self)
{
  esp_client* m = esp_self(self);
  if (!m->handle)
    return AZ_IOT_ERR_NOT_CONNECTED;
  esp_mqtt_client_disconnect(m->handle);
  enqueue_status(m, AZ_IOT_MQTT_EVT_DISCONNECTED, AZ_IOT_OK, 0, false);
  return AZ_IOT_OK;
}

static az_iot_result esp_subscribe(
    az_iot_mqtt_client* self,
    const char* topic_filter,
    az_iot_mqtt_qos qos,
    uint16_t* out_packet_id)
{
  esp_client* m = esp_self(self);
  if (!m->handle)
    return AZ_IOT_ERR_NOT_CONNECTED;
  int id = esp_mqtt_client_subscribe(m->handle, topic_filter, (int)qos);
  if (id < 0)
    return AZ_IOT_ERR_MQTT;
  if (out_packet_id)
    *out_packet_id = (uint16_t)id;
  return AZ_IOT_OK;
}

static az_iot_result esp_unsubscribe(
    az_iot_mqtt_client* self,
    const char* topic_filter,
    uint16_t* out_packet_id)
{
  esp_client* m = esp_self(self);
  if (!m->handle)
    return AZ_IOT_ERR_NOT_CONNECTED;
  int id = esp_mqtt_client_unsubscribe(m->handle, topic_filter);
  if (id < 0)
    return AZ_IOT_ERR_MQTT;
  if (out_packet_id)
    *out_packet_id = (uint16_t)id;
  return AZ_IOT_OK;
}

static az_iot_result esp_publish(
    az_iot_mqtt_client* self,
    const az_iot_mqtt_message* msg,
    uint16_t* out_packet_id)
{
  esp_client* m = esp_self(self);
  if (!m->handle)
    return AZ_IOT_ERR_NOT_CONNECTED;

#if defined(CONFIG_MQTT_PROTOCOL_5)
  /* Attach v5 properties (correlation data / response topic / content type /
   * user properties) to the next publish. These are required for the twin and
   * direct-method feature clients on Hub-Next. */
  mqtt5_user_property_handle_t up = NULL;
  if (m->version == AZ_IOT_MQTT_VERSION_5)
  {
    esp_mqtt5_publish_property_config_t pub = { 0 };
    pub.content_type = msg->content_type;
    pub.response_topic = msg->response_topic;
    pub.correlation_data = (const char*)msg->correlation_data;
    pub.correlation_data_len = (uint16_t)msg->correlation_data_len;

    /* Build a user-property handle from the message's key/value pairs and
     * hang it off the publish property config. */
    if (msg->user_properties && msg->user_properties_count > 0)
    {
      esp_mqtt5_user_property_item_t* items
          = (esp_mqtt5_user_property_item_t*)calloc(msg->user_properties_count, sizeof(*items));
      if (items)
      {
        for (size_t i = 0; i < msg->user_properties_count; ++i)
        {
          items[i].key = msg->user_properties[i].key;
          items[i].value = msg->user_properties[i].value;
        }
        if (esp_mqtt5_client_set_user_property(&up, items, (uint8_t)msg->user_properties_count)
            == ESP_OK)
        {
          pub.user_property = up;
        }
        free(items);
      }
    }

    esp_mqtt5_client_set_publish_property(m->handle, &pub);
  }
#endif

  int id = esp_mqtt_client_publish(
      m->handle,
      msg->topic,
      (const char*)msg->payload,
      (int)msg->payload_len,
      (int)msg->qos,
      msg->retain ? 1 : 0);
#if defined(CONFIG_MQTT_PROTOCOL_5)
  if (up)
    esp_mqtt5_client_delete_user_property(up);
#endif
  if (id < 0)
    return AZ_IOT_ERR_MQTT;
  if (out_packet_id)
    *out_packet_id = (uint16_t)id;
  return AZ_IOT_OK;
}

static az_iot_result esp_process_loop(az_iot_mqtt_client* self, uint32_t timeout_ms)
{
  esp_client* m = esp_self(self);

  /* Drain queued events on the SDK thread (the single-threaded contract). */
  queued_event* n;
  while ((n = q_pop(m)) != NULL)
  {
    if (m->inbound_cb)
      m->inbound_cb(&n->evt, m->inbound_ctx);
    q_free(n);
  }

  /* esp-mqtt does its own I/O on its task, so there is nothing to pump here;
   * yield briefly so the caller's polling loop does not spin. */
  if (timeout_ms)
    vTaskDelay(pdMS_TO_TICKS(timeout_ms > 50 ? 50 : timeout_ms));
  return AZ_IOT_OK;
}

static void esp_set_inbound_cb(
    az_iot_mqtt_client* self,
    az_iot_mqtt_event_callback cb,
    void* user_ctx)
{
  esp_client* m = esp_self(self);
  m->inbound_cb = cb;
  m->inbound_ctx = user_ctx;
}

static void esp_destroy(az_iot_mqtt_client* self)
{
  esp_client* m = esp_self(self);
  if (!m)
    return;
  if (m->handle)
  {
    esp_mqtt_client_stop(m->handle);
    esp_mqtt_client_destroy(m->handle);
    m->handle = NULL;
  }
  q_drain_all(m);
  asm_reset(m);
  if (m->q_mutex)
    vSemaphoreDelete(m->q_mutex);
  free(m);
}

/* ------------------------------------------------------------------------- */
/* factory                                                                   */
/* ------------------------------------------------------------------------- */

static az_iot_mqtt_client* esp_factory_create(void* factory_ctx)
{
  az_iot_mqtt_version version = (az_iot_mqtt_version)(intptr_t)factory_ctx;

  esp_client* m = (esp_client*)calloc(1, sizeof(*m));
  if (!m)
    return NULL;
  m->q_mutex = xSemaphoreCreateMutex();
  if (!m->q_mutex)
  {
    free(m);
    return NULL;
  }

  m->version = version;
  m->iface_storage.version = version;
  m->iface_storage.connect = esp_connect;
  m->iface_storage.disconnect = esp_disconnect;
  m->iface_storage.subscribe = esp_subscribe;
  m->iface_storage.unsubscribe = esp_unsubscribe;
  m->iface_storage.publish = esp_publish;
  m->iface_storage.process_loop = esp_process_loop;
  m->iface_storage.set_inbound_cb = esp_set_inbound_cb;
  m->iface_storage.destroy = esp_destroy;
  m->base.iface = &m->iface_storage;
  return &m->base;
}

static az_iot_mqtt_factory* make_factory(az_iot_mqtt_version version)
{
  az_iot_mqtt_factory* f = (az_iot_mqtt_factory*)calloc(1, sizeof(*f));
  if (!f)
    return NULL;
  f->version = version;
  f->create = esp_factory_create;
  f->factory_ctx = (void*)(intptr_t)version;
  f->destroy = NULL;
  return f;
}

az_iot_mqtt_factory* az_iot_esp_mqtt_factory_create_v3_1_1(void)
{
  return make_factory(AZ_IOT_MQTT_VERSION_3_1_1);
}

az_iot_mqtt_factory* az_iot_esp_mqtt_factory_create_v5(void)
{
  return make_factory(AZ_IOT_MQTT_VERSION_5);
}

void az_iot_esp_mqtt_factory_destroy(az_iot_mqtt_factory* factory) { free(factory); }
