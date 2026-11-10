// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* unified/esp32 - ESP32 IoT Hub sample.
 *
 * Network -> DPS (X.509, identity from the esp_secure_cert partition) -> the assigned hub, on
 * mqttv3. While connected it:
 *   - sends telemetry every HUB_TELEMETRY_INTERVAL_S seconds ({"deviceId":"<id>","count":n});
 *   - answers the direct method "echo" with its payload (status 200; 413 when too large), any
 *     other with 404;
 *   - acknowledges each desired twin version by patching {"desiredVersion":v} into reported,
 *     including one set while it was offline (a twin GET on connect).
 *
 * The feature clients are built before open(). If DPS assigns a hub generation this SDK does not
 * support, the connection faults with AZ_IOT_ERR_CONNECTION_PROFILE_UNSUPPORTED and the device
 * restarts.
 *
 * Settings: `idf.py menuconfig` (see Kconfig.projbuild); the network under "Example Connection
 * Configuration", or OpenCores Ethernet in QEMU (sdkconfig.qemu). Lines starting "hub_esp32:"
 * are stable: tests/e2e reads them.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "protocol_examples_common.h"
#include "sdkconfig.h"

#include "azure/core/az_json.h"
#include "azure/iot/adapters/az_iot_adapter_az_mqtt.h"
#include "azure/iot/az_iot.h"

#include "az_iot_certificate_provider_esp_secure_cert.h"
#include "az_iot_crypto_mbedtls.h"

static const char* TAG = "hub_esp32";

/** @brief The direct method this device implements. */
#define ECHO_METHOD "echo"
/** @brief How long a direct method request stays answerable. */
#define METHOD_RESPONSE_TIMEOUT_S 60u
/** @brief The do_work() tick. */
#define TICK_MS 50u
/** @brief Longest time without a hub connection before the device restarts. ESP-IDF's
 * protocol_examples_common stops reconnecting Wi-Fi after EXAMPLE_WIFI_CONN_MAX_RETRY failures, and
 * a restart brings the network up again. */
#define OFFLINE_RESTART_S 900u
/** @brief Wait before retrying a failed twin GET or reported patch. */
#define TWIN_RETRY_MS 5000u
/** @brief Longest wait for a telemetry acknowledgement or a twin response: those requests
 * end only on their answer or with the session, so the sample reconnects after this. */
#define RESPONSE_TIMEOUT_S 60u
/** @brief Largest echoed payload: the response must fit the adapter's send buffer, with
 * room for its topic. */
#define MQTTV3_ECHO_PAYLOAD_MAX (CONFIG_AZ_IOT_AZ_MQTT_SEND_BUFFER_SIZE - 256u)
/** @brief MQTT message store: one full-size message with az_mqtt's 18-byte per-message overhead,
 * kept for the resumed hub session. */
#define MQTT_MESSAGE_STORE_SIZE (CONFIG_AZ_IOT_AZ_MQTT_SEND_BUFFER_SIZE + 32u)
/** @brief Copies a connect makes: host, client ID, user name, device certificate and key PEM. */
#define MQTT_CONNECT_STRINGS_SIZE 8192u
/** @brief Bound of az_mqtt's TLS state with mbedTLS (about 4.5 KB). */
#define MQTT_TLS_STATE_SIZE 8192u

/** @brief A wait before the next retry: none, or #TWIN_RETRY_MS from @c started. */
typedef struct
{
  bool active;
  TickType_t started;
} retry_wait;

/** @brief Starts a #TWIN_RETRY_MS wait. */
static void retry_wait_start(retry_wait* w)
{
  w->active = true;
  w->started = xTaskGetTickCount();
}

/** @brief Whether no wait is active; ends one that has run its course. Unsigned tick arithmetic,
 * so a counter wrap does not extend it. */
static bool retry_wait_over(retry_wait* w)
{
  if (w->active && xTaskGetTickCount() - w->started >= pdMS_TO_TICKS(TWIN_RETRY_MS))
  {
    w->active = false;
  }
  return !w->active;
}

/** @brief A method answer refused with AZ_IOT_ERR_BUSY (the connection still setting
 * up), retried from the loop. */
typedef struct
{
  bool active;
  az_iot_direct_method_request request;
  int status;
  uint8_t* payload; /* owned copy; NULL when empty */
  size_t length;
} pending_method_answer;

/** @brief The device: connection, the feature clients, and what is pending. */
typedef struct
{
  az_iot_connection_client connection;
  az_iot_mqttv3_telemetry_client mqttv3_telemetry;
  az_iot_mqttv3_twin_client mqttv3_twin;
  uint8_t mqttv3_twin_workspace[AZ_IOT_MQTTV3_TWIN_CLIENT_DEFAULT_WORKSPACE_SIZE];
  az_iot_mqttv3_direct_method_client mqttv3_methods;
  uint8_t mqttv3_methods_workspace[AZ_IOT_MQTTV3_DIRECT_METHOD_CLIENT_DEFAULT_WORKSPACE_SIZE];
  /* Set by callbacks, acted on by the loop. */
  az_iot_connection_state hub_state;
  TickType_t offline_since; /* when hub_state last left CONNECTED, or the loop started */
  bool faulted;
  bool telemetry_pending;
  TickType_t telemetry_sent_at;
  uint32_t telemetry_sent;
  pending_method_answer method_answer;
  bool twin_get_due; /* read the desired version once connected */
  bool twin_get_in_flight;
  TickType_t twin_get_sent;
  retry_wait twin_get_retry;
  bool report_pending;
  TickType_t report_sent;
  retry_wait report_retry;
  bool reconnecting; /* closed for a stalled twin request; reopened at IDLE */
  /* Desired versions, for the hub and device id below: a new assignment starts its own count. */
  char assigned_hub[256];
  char assigned_device_id[129];
  uint64_t newest_desired_version; /* highest seen; older deliveries are ignored */
  uint64_t desired_version; /* latest desired version not yet acknowledged; 0: none */
  uint64_t reported_version; /* in the patch in flight */
} hub_device;

static hub_device s_device;

/** @brief On hub CONNECTED: a different hub or device id than before restarts the desired-version
 * tracking, since that twin counts its versions from its own start. */
static void track_assignment(hub_device* d)
{
  const char* hub = az_iot_connection_client_get_iothub_address(&d->connection);
  const char* device_id = az_iot_connection_client_get_device_id(&d->connection);
  if (hub == NULL || device_id == NULL
      || (strcmp(hub, d->assigned_hub) == 0 && strcmp(device_id, d->assigned_device_id) == 0))
  {
    return;
  }
  (void)snprintf(d->assigned_hub, sizeof(d->assigned_hub), "%s", hub);
  (void)snprintf(d->assigned_device_id, sizeof(d->assigned_device_id), "%s", device_id);
  d->newest_desired_version = 0;
  d->desired_version = 0;
}

/**
 * @brief Takes @p version as the one to acknowledge unless an equal or newer one was seen: a late
 * twin GET or patch must not move the acknowledgement back.
 */
static void note_desired_version(hub_device* d, uint64_t version)
{
  track_assignment(d);
  if (version <= d->newest_desired_version)
  {
    ESP_LOGI(TAG, "desired version %" PRIu64 " is not newer; ignored", version);
    return;
  }
  d->newest_desired_version = version;
  d->desired_version = version;
}

static void on_connection_state(const az_iot_connection_state_event* event, void* user_ctx)
{
  hub_device* d = (hub_device*)user_ctx;
  if (event->state == AZ_IOT_CONN_STATE_FAULTED)
  {
    ESP_LOGE(TAG, "connection faulted: %s", az_iot_result_to_string(event->reason));
    d->faulted = true;
  }
  if (event->scope == AZ_IOT_CONN_SCOPE_HUB && event->state != d->hub_state)
  {
    if (d->hub_state == AZ_IOT_CONN_STATE_CONNECTED)
    {
      d->offline_since = xTaskGetTickCount();
    }
    d->hub_state = event->state;
    if (event->state == AZ_IOT_CONN_STATE_CONNECTED)
    {
      ESP_LOGI(
          TAG,
          "ready on %s",
          az_iot_connection_profile_to_string(AZ_IOT_CONNECTION_PROFILE_MQTT_V3));
      track_assignment(d);
      d->twin_get_due = true;
      d->twin_get_retry.active = false;
    }
  }
}

/* ---- Telemetry ------------------------------------------------------------ */

static void on_telemetry_sent(az_iot_result status, void* user_ctx)
{
  hub_device* d = (hub_device*)user_ctx;
  d->telemetry_pending = false;
  if (status == AZ_IOT_OK)
  {
    ESP_LOGI(TAG, "telemetry %" PRIu32 " sent", ++d->telemetry_sent);
  }
  else
  {
    ESP_LOGW(TAG, "telemetry failed: %s", az_iot_result_to_string(status));
  }
}

static void send_telemetry(hub_device* d)
{
  // The device id DPS assigned, which need not equal the registration id (custom allocation).
  char payload[192];
  int length = snprintf(
      payload,
      sizeof(payload),
      "{\"deviceId\":\"%s\",\"count\":%" PRIu32 "}",
      d->assigned_device_id,
      d->telemetry_sent + 1);
  az_iot_telemetry_property properties[] = {
    { AZ_IOT_MSG_PROP_CONTENT_TYPE, "application/json" },
  };
  az_iot_telemetry_message message = { 0 };
  message.payload = (const uint8_t*)payload;
  message.payload_len = (size_t)length;
  message.properties = properties;
  message.properties_count = sizeof(properties) / sizeof(properties[0]);
  az_iot_result result
      = az_iot_mqttv3_telemetry_client_send(&d->mqttv3_telemetry, &message, on_telemetry_sent, d);
  d->telemetry_pending = result == AZ_IOT_OK;
  d->telemetry_sent_at = xTaskGetTickCount();
}

/* ---- Twin ----------------------------------------------------------------- */

static void on_desired_mqttv3(const uint8_t* patch, size_t length, uint64_t version, void* user_ctx)
{
  ESP_LOGI(TAG, "desired version %" PRIu64 ": %.*s", version, (int)length, (const char*)patch);
  note_desired_version((hub_device*)user_ctx, version);
}

/**
 * @brief Reads `desired.$version` from a twin document.
 * @return The version; 0 when absent or unreadable.
 */
static uint64_t read_desired_version(const uint8_t* twin, size_t length)
{
  az_json_reader reader;
  if (az_result_failed(
          az_json_reader_init(&reader, az_span_create((uint8_t*)twin, (int32_t)length), NULL))
      || az_result_failed(az_json_reader_next_token(&reader))
      || reader.token.kind != AZ_JSON_TOKEN_BEGIN_OBJECT)
  {
    return 0;
  }
  int depth = 1; // Inside the document; 2: inside "desired".
  while (az_result_succeeded(az_json_reader_next_token(&reader))
         && reader.token.kind == AZ_JSON_TOKEN_PROPERTY_NAME)
  {
    bool const wanted = depth == 1
        ? az_json_token_is_text_equal(&reader.token, AZ_SPAN_FROM_STR("desired"))
        : az_json_token_is_text_equal(&reader.token, AZ_SPAN_FROM_STR("$version"));
    if (az_result_failed(az_json_reader_next_token(&reader)))
    {
      return 0;
    }
    if (wanted && depth == 1)
    {
      if (reader.token.kind != AZ_JSON_TOKEN_BEGIN_OBJECT)
      {
        return 0;
      }
      depth = 2;
    }
    else if (wanted)
    {
      uint64_t version = 0;
      return az_result_succeeded(az_json_token_get_uint64(&reader.token, &version)) ? version : 0;
    }
    else if (az_result_failed(az_json_reader_skip_children(&reader)))
    {
      return 0;
    }
  }
  return 0;
}

static void on_twin_mqttv3(az_iot_result status, const uint8_t* twin, size_t length, void* user_ctx)
{
  hub_device* d = (hub_device*)user_ctx;
  d->twin_get_in_flight = false;
  if (status != AZ_IOT_OK)
  {
    ESP_LOGW(TAG, "twin get failed: %s; retrying", az_iot_result_to_string(status));
    d->twin_get_due = true;
    retry_wait_start(&d->twin_get_retry);
    return;
  }
  uint64_t const version = read_desired_version(twin, length);
  if (version != 0)
  {
    ESP_LOGI(TAG, "desired version %" PRIu64 ": from the twin", version);
    note_desired_version(d, version); // May be older than a live patch.
  }
}

/** @brief Ends the reported patch in flight; a refused one is retried after #TWIN_RETRY_MS. */
static void finish_report(hub_device* d, bool accepted)
{
  d->report_pending = false;
  if (!accepted)
  {
    retry_wait_start(&d->report_retry);
    return;
  }
  ESP_LOGI(TAG, "desired version %" PRIu64 " acknowledged", d->reported_version);
  if (d->desired_version == d->reported_version)
  {
    d->desired_version = 0;
  }
}

static void on_reported_mqttv3(az_iot_result status, uint64_t version, void* user_ctx)
{
  (void)version;
  if (status != AZ_IOT_OK)
  {
    ESP_LOGW(TAG, "reported patch refused: %s", az_iot_result_to_string(status));
  }
  finish_report((hub_device*)user_ctx, status == AZ_IOT_OK);
}

static void report_desired_version(hub_device* d)
{
  char patch[48];
  d->reported_version = d->desired_version;
  int length
      = snprintf(patch, sizeof(patch), "{\"desiredVersion\":%" PRIu64 "}", d->reported_version);
  az_iot_result result = az_iot_mqttv3_twin_client_patch_reported(
      &d->mqttv3_twin, (const uint8_t*)patch, (size_t)length, on_reported_mqttv3, d);
  d->report_pending = result == AZ_IOT_OK;
  d->report_sent = xTaskGetTickCount();
  if (result != AZ_IOT_OK)
  {
    ESP_LOGW(TAG, "reported patch not sent: %s", az_iot_result_to_string(result));
    retry_wait_start(&d->report_retry);
  }
}

/* ---- Direct methods ------------------------------------------------------- */

/** @brief Drops the held method answer, if any. */
static void drop_method_answer(hub_device* d)
{
  free(d->method_answer.payload);
  d->method_answer = (pending_method_answer){ 0 };
}

/** @brief Sends the held method answer; keeps it while the connection is still setting up
 * (AZ_IOT_ERR_BUSY), drops it otherwise (answered, timed out, or its session ended). */
static void retry_method_answer(hub_device* d)
{
  pending_method_answer* a = &d->method_answer;
  az_iot_result const r = az_iot_mqttv3_direct_method_respond(
      &d->mqttv3_methods, a->request, a->status, a->payload, a->length);
  if (r == AZ_IOT_ERR_BUSY)
  {
    return;
  }
  if (r == AZ_IOT_OK)
  {
    ESP_LOGI(TAG, "held method answer sent: %d", a->status);
  }
  else
  {
    ESP_LOGW(TAG, "method answer dropped: %s", az_iot_result_to_string(r));
  }
  drop_method_answer(d);
}

static void on_method_mqttv3(
    az_iot_direct_method_request request,
    const char* method_name,
    const uint8_t* payload,
    size_t length,
    void* user_ctx)
{
  hub_device* d = (hub_device*)user_ctx;
  const char* const name = method_name ? method_name : "";
  bool const echo = strcmp(name, ECHO_METHOD) == 0;
  bool const fits = length <= MQTTV3_ECHO_PAYLOAD_MAX;
  int const status = !echo ? 404 : fits ? 200 : 413;
  size_t const answer_length = status == 200 ? length : 0;
  az_iot_result const r = az_iot_mqttv3_direct_method_respond(
      &d->mqttv3_methods, request, status, answer_length > 0 ? payload : NULL, answer_length);
  if (r == AZ_IOT_OK)
  {
    ESP_LOGI(TAG, "method '%s' answered %d", name, status);
    return;
  }
  // BUSY: the connection is still setting up and the request stays answerable; hold one answer.
  uint8_t* copy = NULL;
  if (r != AZ_IOT_ERR_BUSY || d->method_answer.active
      || (answer_length > 0 && (copy = malloc(answer_length)) == NULL))
  {
    ESP_LOGW(TAG, "method '%s' not answered: %s", name, az_iot_result_to_string(r));
    return;
  }
  if (copy != NULL)
  {
    memcpy(copy, payload, answer_length);
  }
  d->method_answer = (pending_method_answer){ true, request, status, copy, answer_length };
  ESP_LOGI(TAG, "method '%s': answer held until the connection is ready", name);
}

/* ---- Feature clients ------------------------------------------------------ */

/** @brief Builds the telemetry, twin and direct method clients. On failure the device must
 * restart. */
static az_iot_result build_clients(hub_device* d)
{
  az_iot_result r = az_iot_mqttv3_telemetry_client_init(&d->mqttv3_telemetry, &d->connection);
  r = r == AZ_IOT_OK ? az_iot_mqttv3_twin_client_init(
                           &d->mqttv3_twin,
                           &d->connection,
                           &(az_iot_mqttv3_twin_client_options){
                               .workspace = d->mqttv3_twin_workspace,
                               .workspace_size = sizeof(d->mqttv3_twin_workspace) })
                     : r;
  r = r == AZ_IOT_OK
      ? az_iot_mqttv3_twin_client_set_desired_handler(&d->mqttv3_twin, on_desired_mqttv3, d)
      : r;
  r = r == AZ_IOT_OK ? az_iot_mqttv3_direct_method_client_init(
                           &d->mqttv3_methods,
                           &d->connection,
                           &(az_iot_mqttv3_direct_method_client_options){
                               .workspace = d->mqttv3_methods_workspace,
                               .workspace_size = sizeof(d->mqttv3_methods_workspace) })
                     : r;
  r = r == AZ_IOT_OK
      ? az_iot_mqttv3_direct_method_client_set_handler(&d->mqttv3_methods, on_method_mqttv3, d)
      : r;
  r = r == AZ_IOT_OK ? az_iot_mqttv3_direct_method_client_set_response_timeout(
                           &d->mqttv3_methods, METHOD_RESPONSE_TIMEOUT_S)
                     : r;
  // A partly built set is left as is: the caller restarts.
  return r;
}

/**
 * @brief Waits out what is left of a #TICK_MS tick begun at @p tick_start: do_work() returns at
 * once while no session is open (a retry backoff), and the loop must not starve the idle task.
 */
static void wait_for_tick_end(TickType_t tick_start)
{
  TickType_t const spent = xTaskGetTickCount() - tick_start;
  if (spent < pdMS_TO_TICKS(TICK_MS))
  {
    vTaskDelay(pdMS_TO_TICKS(TICK_MS) - spent);
  }
}

/** @brief Logs @p what and restarts: this sample has nothing better to do on such a failure. */
static void restart(const char* what)
{
  ESP_LOGE(TAG, "%s; rebooting", what);
  vTaskDelay(pdMS_TO_TICKS(5000));
  esp_restart();
}

void app_main(void)
{
  ESP_LOGI(TAG, "IoT Hub ESP32 sample starting");

  esp_err_t nvs = nvs_flash_init(); // Wi-Fi keeps its calibration in NVS.
  if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND)
  {
    ESP_ERROR_CHECK(nvs_flash_erase());
    nvs = nvs_flash_init();
  }
  ESP_ERROR_CHECK(nvs);

  // Wi-Fi, or OpenCores Ethernet in QEMU (sdkconfig.qemu); blocks until an IPv4 address.
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  if (example_connect() != ESP_OK)
  {
    restart("network connect failed");
  }

  az_iot_log_sink log = az_iot_log_stderr_sink(AZ_IOT_LOG_LEVEL_INFO);
  az_iot_log_set_global_sink(&log);

  /* Device identity from the esp_secure_cert partition (see README). The servers are verified
   * against the ESP-IDF certificate bundle: no CA here. Never deinitialized: the connection
   * uses it until the task ends, which it never does. */
  static az_iot_certificate_provider_esp_secure_cert certs;
  if (az_iot_certificate_provider_esp_secure_cert_init(&certs, NULL) != AZ_IOT_OK)
  {
    restart("no device certificate in the esp_secure_cert partition; see README");
  }

  hub_device* d = &s_device;
  az_iot_connection_client_options options = az_iot_connection_client_options_default();
  options.dps.id_scope = CONFIG_HUB_DPS_ID_SCOPE;
  options.dps.registration_id = CONFIG_HUB_DPS_REGISTRATION_ID;
  options.dps.global_endpoint
      = CONFIG_HUB_DPS_GLOBAL_ENDPOINT[0] != '\0' ? CONFIG_HUB_DPS_GLOBAL_ENDPOINT : NULL;
  options.certificate_provider = &certs.base;
  options.crypto = az_iot_crypto_mbedtls();
  // Memory for the connection client's entries, sized for the default limits.
  static uint8_t connection_workspace[AZ_IOT_CONNECTION_CLIENT_DEFAULT_WORKSPACE_SIZE];
  options.workspace = connection_workspace;
  options.workspace_size = sizeof(connection_workspace);
  /* The az_mqtt adapter builds each session's MQTT 3.1.1 client in this workspace, one at a time:
   * DPS, then the hub. Sizes: menuconfig. */
  static uint8_t mqtt_workspace[AZ_IOT_AZ_MQTT_WORKSPACE_SIZE(
      CONFIG_AZ_IOT_AZ_MQTT_SEND_BUFFER_SIZE,
      CONFIG_AZ_IOT_AZ_MQTT_RECEIVE_BUFFER_SIZE,
      CONFIG_AZ_IOT_AZ_MQTT_INFLIGHT_MAX,
      MQTT_MESSAGE_STORE_SIZE,
      MQTT_CONNECT_STRINGS_SIZE,
      MQTT_TLS_STATE_SIZE)];
  options.mqtt_adapter = az_iot_az_mqtt_get_adapter();
  options.mqtt_limits = (az_iot_mqtt_limits){
    .send_buffer_size = CONFIG_AZ_IOT_AZ_MQTT_SEND_BUFFER_SIZE,
    .receive_buffer_size = CONFIG_AZ_IOT_AZ_MQTT_RECEIVE_BUFFER_SIZE,
    .max_in_flight_messages = CONFIG_AZ_IOT_AZ_MQTT_INFLIGHT_MAX,
    .message_store_size = MQTT_MESSAGE_STORE_SIZE,
    .connect_strings_size = MQTT_CONNECT_STRINGS_SIZE,
  };
  options.mqtt_workspace = mqtt_workspace;
  options.mqtt_workspace_size = sizeof(mqtt_workspace);
  if (az_iot_connection_client_init(&d->connection, &options) != AZ_IOT_OK)
  {
    restart("connection client init failed");
  }
  az_iot_connection_client_add_state_observer(&d->connection, on_connection_state, d);

  if (build_clients(d) != AZ_IOT_OK || az_iot_connection_client_open(&d->connection) != AZ_IOT_OK)
  {
    restart("open failed");
  }

  // Seconds to ticks directly: pdMS_TO_TICKS would overflow its 32-bit product for long intervals.
  TickType_t const interval = (TickType_t)CONFIG_HUB_TELEMETRY_INTERVAL_S * configTICK_RATE_HZ;
  TickType_t const offline_limit = (TickType_t)OFFLINE_RESTART_S * configTICK_RATE_HZ;
  TickType_t const response_limit = (TickType_t)RESPONSE_TIMEOUT_S * configTICK_RATE_HZ;
  TickType_t next_telemetry = xTaskGetTickCount();
  TickType_t tick_start = xTaskGetTickCount();
  d->offline_since = tick_start;
  for (;;)
  {
    wait_for_tick_end(tick_start); // At the top: the body continues early.
    tick_start = xTaskGetTickCount();
    (void)az_iot_connection_client_do_work(&d->connection, TICK_MS);

    if (d->faulted)
    {
      restart("connection faulted"); // Settled until close(): nothing more would be sent.
    }
    if (d->method_answer.active)
    {
      retry_method_answer(d); // Also drops it once unanswerable, connected or not.
    }
    if (d->hub_state != AZ_IOT_CONN_STATE_CONNECTED)
    {
      if (d->reconnecting && d->hub_state == AZ_IOT_CONN_STATE_IDLE)
      {
        d->reconnecting = false;
        if (az_iot_connection_client_open(&d->connection) != AZ_IOT_OK)
        {
          restart("reopen failed");
        }
      }
      if (xTaskGetTickCount() - d->offline_since >= offline_limit)
      {
        ESP_LOGE(TAG, "no hub connection for %u s", OFFLINE_RESTART_S);
        restart("offline too long");
      }
      continue;
    }

    TickType_t const now = xTaskGetTickCount();
    if (!d->reconnecting
        && ((d->telemetry_pending && now - d->telemetry_sent_at >= response_limit)
            || (d->twin_get_in_flight && now - d->twin_get_sent >= response_limit)
            || (d->report_pending && now - d->report_sent >= response_limit)))
    {
      // close() fails the outstanding requests (AZ_IOT_ERR_NOT_CONNECTED); they retry after.
      ESP_LOGW(
          TAG, "no acknowledgement or twin response for %u s; reconnecting", RESPONSE_TIMEOUT_S);
      d->reconnecting = true;
      (void)az_iot_connection_client_close(&d->connection);
      continue;
    }

    if (!d->telemetry_pending && (int32_t)(xTaskGetTickCount() - next_telemetry) >= 0)
    {
      next_telemetry = xTaskGetTickCount() + interval;
      send_telemetry(d);
    }
    if (d->twin_get_due && retry_wait_over(&d->twin_get_retry))
    {
      az_iot_result const r = az_iot_mqttv3_twin_client_get(&d->mqttv3_twin, on_twin_mqttv3, d);
      d->twin_get_due = r != AZ_IOT_OK;
      d->twin_get_in_flight = r == AZ_IOT_OK;
      d->twin_get_sent = xTaskGetTickCount();
      if (r != AZ_IOT_OK)
      {
        ESP_LOGW(TAG, "twin get not sent: %s; retrying", az_iot_result_to_string(r));
        retry_wait_start(&d->twin_get_retry);
      }
    }
    if (!d->report_pending && d->desired_version != 0 && retry_wait_over(&d->report_retry))
    {
      report_desired_version(d);
    }
  }
}
