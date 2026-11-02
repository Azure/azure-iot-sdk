// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* unified/multithreaded_telemetry - sample.
 *
 * unified/telemetry, with the readings produced on other threads. Everything
 * else is identical to that sample, so the difference between the two files is
 * exactly the feature.
 *
 * WHY: the SDK is single-threaded. Its callbacks run on the thread that calls
 * into it, and it does not lock its state against other threads: every call
 * into it, for every client in the process, must come from one thread or be
 * serialized by the application. An application whose data comes from other
 * threads has to hand that data to the SDK thread itself.
 *
 * HOW, in full:
 *
 *   1. Sensor threads push readings into reading_queue (reading_queue.c), a
 *      bounded FIFO the APPLICATION owns and locks. They never call the SDK.
 *   2. This thread is the only one that calls the SDK: it runs do_work(), and
 *      between calls pops a reading and sends it. The SDK callbacks run inside
 *      do_work() on this thread, so user_context needs no lock.
 *   3. On exit, the sensors are stopped and joined before the queue is released.
 *
 * The alternative -- one application mutex held around EVERY SDK call, on every
 * thread -- also satisfies the contract, but do_work() blocks for its timeout
 * while holding it, and a callback must never take it again. The hand-off keeps
 * the SDK on one thread and the lock held only for a copy.
 *
 * DPS is handled internally by the connection client when dps.id_scope is set.
 */
#include <stdio.h>
#include <stdlib.h>

#include "azure/iot/az_iot.h"
/* The MQTT adapter this build of the sample uses (see samples/CMakeLists.txt). */
#if defined(AZ_IOT_SAMPLE_ADAPTER_PAHO)
#include "azure/iot/adapters/az_iot_adapter_paho.h"
#elif defined(AZ_IOT_SAMPLE_ADAPTER_AZ_MQTT)
#include "azure/iot/adapters/az_iot_adapter_az_mqtt.h"
#else
#error "Define AZ_IOT_SAMPLE_ADAPTER_PAHO or AZ_IOT_SAMPLE_ADAPTER_AZ_MQTT"
#endif

#include "sample_utils.h"
#include "reading_queue.h"

#if !defined(_WIN32)
#include <pthread.h>
#endif

/** @brief How long the sample runs. */
#define SAMPLE_RUN_MS 60000u
/** @brief Sensor threads producing readings. */
#define SAMPLE_SENSORS 2u
/** @brief Time between two readings of one sensor. */
#define SAMPLE_READING_INTERVAL_MS 2500u

typedef struct
{
  sample_config config;
  az_iot_certificate_provider_pem certs;
  az_iot_connection_client connection_client;
  /* Only the one matching `profile` is initialized. */
  az_iot_mqttv3_telemetry_client mqttv3;
  az_iot_mqttv5_telemetry_client mqttv5;
  az_iot_connection_profile profile;
  int telemetry_initialized;
} sample_state;

typedef struct
{
  az_iot_connection_state conn_state;
  int faulted; /* terminal: nothing this sample can do about it */
  int rebuild; /* DPS assigned `assigned_profile`; rebuild for it */
  az_iot_connection_profile assigned_profile;
  int send_pending;
  int sent_ok;
  int send_failed;
} user_context;

static void telemetry_destroy(sample_state* state)
{
  if (!state->telemetry_initialized)
  {
    return;
  }
  if (state->profile == AZ_IOT_CONNECTION_PROFILE_MQTT_V5)
  {
    az_iot_mqttv5_telemetry_client_deinit(&state->mqttv5);
  }
  else
  {
    az_iot_mqttv3_telemetry_client_deinit(&state->mqttv3);
  }
  state->telemetry_initialized = 0;
}

/* Pins the connection to `profile`: an assignment to the other generation is
 * then refused with AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH. */
static az_iot_result telemetry_build(sample_state* state, az_iot_connection_profile profile)
{
  az_iot_result result;
  switch (profile)
  {
    case AZ_IOT_CONNECTION_PROFILE_MQTT_V5:
      result = az_iot_mqttv5_telemetry_client_init(&state->mqttv5, &state->connection_client);
      break;
    case AZ_IOT_CONNECTION_PROFILE_MQTT_V3:
      result = az_iot_mqttv3_telemetry_client_init(&state->mqttv3, &state->connection_client);
      break;
    case AZ_IOT_CONNECTION_PROFILE_UNKNOWN:
    default:
      return AZ_IOT_ERR_CONNECTION_PROFILE_UNSUPPORTED;
  }
  if (result == AZ_IOT_OK)
  {
    state->profile = profile;
    state->telemetry_initialized = 1;
  }
  return result;
}

static void sample_state_destroy(sample_state* state)
{
  telemetry_destroy(state);
  az_iot_connection_client_deinit(&state->connection_client);
  az_iot_certificate_provider_pem_deinit(&state->certs);
  sample_config_release(&state->config);
}

static void on_conn_state(const az_iot_connection_state_event* event, void* user_ctx)
{
  user_context* ctx = (user_context*)user_ctx;

  /* Only record it: feature clients must not be destroyed from inside the
   * callback, which runs nested in do_work(). */
  if (event->state == AZ_IOT_CONN_STATE_FAULTED
      && event->reason == AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH && event->profile != NULL)
  {
    ctx->assigned_profile = event->profile->connection_profile;
    ctx->rebuild = 1;
    return;
  }

  if (event->state == AZ_IOT_CONN_STATE_FAULTED)
  {
    ctx->faulted = 1;
    /* DPS assigned a hub generation newer than this SDK. */
    if (event->reason == AZ_IOT_ERR_CONNECTION_PROFILE_UNSUPPORTED && event->profile != NULL)
    {
      printf(
          "Unsupported hub generation \"%s\"%s. Upgrade the SDK.\n",
          event->profile->connection_profile_raw != NULL ? event->profile->connection_profile_raw
                                                         : "(none)",
          event->profile->connection_profile_raw_truncated ? " (truncated)" : "");
    }
  }

  /* The provisioning lifecycle reports on its own scope; only the hub one says
   * whether telemetry can flow. */
  if (event->scope == AZ_IOT_CONN_SCOPE_HUB)
  {
    ctx->conn_state = event->state;
  }
}

static void on_send_done(az_iot_result status, void* user_ctx)
{
  user_context* ctx = (user_context*)user_ctx;
  ctx->send_pending = 0;
  if (status == AZ_IOT_OK)
  {
    ctx->sent_ok++;
  }
  else
  {
    ctx->send_failed++;
  }
}

static az_iot_result send_one(sample_state* state, user_context* ctx, const reading* r)
{
  /* The same message on either generation. MQTTv3 URL-encodes every
   * property into the topic ("$.ct" goes out as "%24.ct", "deg C" as
   * "deg%20C"); MQTTv5 carries them as MQTT v5 user properties, byte for byte,
   * with $.ct mapped to the native content type. The payload needs to stay
   * valid only during the send call. */
  char payload[64];
  int len = snprintf(
      payload, sizeof(payload), "{\"sensor\":%u,\"temp\":%d}", r->sensor, r->temperature);
  if (len < 0 || (size_t)len >= sizeof(payload))
  {
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }
  az_iot_telemetry_property props[] = {
    { AZ_IOT_MSG_PROP_CONTENT_TYPE, "application/json" },
    { "unit", "deg C" },
  };
  az_iot_telemetry_message msg = { 0 };
  msg.payload = (const uint8_t*)payload;
  msg.payload_len = (size_t)len;
  msg.properties = props;
  msg.properties_count = sizeof(props) / sizeof(props[0]);

  az_iot_result result = state->profile == AZ_IOT_CONNECTION_PROFILE_MQTT_V5
      ? az_iot_mqttv5_telemetry_client_send(&state->mqttv5, &msg, on_send_done, ctx)
      : az_iot_mqttv3_telemetry_client_send(&state->mqttv3, &msg, on_send_done, ctx);
  ctx->send_pending = (result == AZ_IOT_OK);
  return result;
}

/* ---- sensor threads: application code, they never call the SDK ---------- */

typedef struct
{
  reading_queue* queue;
  unsigned id;
} sensor_args;

#if defined(_WIN32)
typedef HANDLE sensor_thread;
static DWORD WINAPI sensor_main(LPVOID arg)
#else
typedef pthread_t sensor_thread;
static void* sensor_main(void* arg)
#endif
{
  const sensor_args* s = (const sensor_args*)arg;
  int temperature = 20 + (int)s->id;
  while (!reading_queue_stopping(s->queue))
  {
    reading r;
    r.sensor = s->id;
    r.temperature = temperature;
    reading_queue_push(s->queue, r);
    temperature = temperature < 30 ? temperature + 1 : 20;
    sample_sleep_ms((long)SAMPLE_READING_INTERVAL_MS);
  }
#if defined(_WIN32)
  return 0;
#else
  return NULL;
#endif
}

/** @brief Start a sensor thread on @p args. @return false if it could not start. */
static bool sensor_start(sensor_thread* t, sensor_args* args)
{
#if defined(_WIN32)
  *t = CreateThread(NULL, 0, sensor_main, args, 0, NULL);
  return *t != NULL;
#else
  return pthread_create(t, NULL, sensor_main, args) == 0;
#endif
}

/** @brief Stop @p q's producers and join the first @p count of @p threads. */
static void sensors_stop(reading_queue* q, sensor_thread* threads, unsigned count)
{
  reading_queue_stop(q);
  for (unsigned i = 0; i < count; ++i)
  {
#if defined(_WIN32)
    (void)WaitForSingleObject(threads[i], INFINITE);
    (void)CloseHandle(threads[i]);
#else
    (void)pthread_join(threads[i], NULL);
#endif
  }
}

int main(void)
{
  az_iot_log_sink log = sample_log_sink(AZ_IOT_LOG_LEVEL_INFO);
  az_iot_log_set_global_sink(&log);

  sample_state state = { 0 };
  if (sample_config_load(&state.config) != 0)
  {
    return 1;
  }

  user_context user_ctx = { 0 };

  /* Certificate provider */
  az_iot_certificate_provider_pem_options pem = az_iot_certificate_provider_pem_options_default();
  pem.trusted_ca_pem_path = state.config.ca;
  pem.client_cert_pem_path = state.config.cert;
  pem.client_key_pem_path = state.config.key;

  if (az_iot_certificate_provider_pem_init(&state.certs, &pem) != AZ_IOT_OK)
  {
    sample_state_destroy(&state);
    return 1;
  }

  /* Connection client (DPS provisioning is internal). The default reconnection
   * and identity recovery policies keep the device reconnecting on its own. */
  /* Memory for the connection client's entries, sized for the default limits. */
  static uint8_t connection_workspace[AZ_IOT_CONNECTION_CLIENT_DEFAULT_WORKSPACE_SIZE];
  az_iot_connection_client_options copts = az_iot_connection_client_options_default();
  copts.workspace = connection_workspace;
  copts.workspace_size = sizeof(connection_workspace);
  copts.dps.id_scope = state.config.id_scope;
  copts.dps.registration_id = state.config.reg_id;
  copts.dps.global_endpoint = state.config.dps_global_endpoint; /* NULL: the global default */
  copts.certificate_provider = &state.certs.base;
  /* The adapter builds each session's MQTT client in this workspace, one at a time: MQTT 3.1.1
   * for DPS and an MQTTv3 hub, MQTT 5 for an MQTTv5 hub. Limits: sample_utils.h. */
  static uint8_t mqtt_workspace[SAMPLE_MQTT_WORKSPACE_SIZE];
#if defined(AZ_IOT_SAMPLE_ADAPTER_PAHO)
  copts.mqtt_adapter = az_iot_paho_get_adapter();
#elif defined(AZ_IOT_SAMPLE_ADAPTER_AZ_MQTT)
  copts.mqtt_adapter = az_iot_az_mqtt_get_adapter();
#endif
  copts.mqtt_limits = SAMPLE_MQTT_LIMITS;
  copts.mqtt_workspace = mqtt_workspace;
  copts.mqtt_workspace_size = sizeof(mqtt_workspace);

  if (az_iot_connection_client_init(&state.connection_client, &copts) != AZ_IOT_OK)
  {
    sample_state_destroy(&state);
    return 1;
  }
  az_iot_connection_client_add_state_observer(&state.connection_client, on_conn_state, &user_ctx);

  /* Assume a generation until DPS says otherwise; see unified/telemetry. */
  if (telemetry_build(&state, AZ_IOT_CONNECTION_PROFILE_MQTT_V3) != AZ_IOT_OK
      || az_iot_connection_client_open(&state.connection_client) != AZ_IOT_OK)
  {
    sample_state_destroy(&state);
    return 1;
  }

  /* The application's hand-off: sensor threads push, this thread pops. */
  reading_queue readings;
  if (!reading_queue_init(&readings))
  {
    sample_state_destroy(&state);
    return 1;
  }
  sensor_thread sensors[SAMPLE_SENSORS];
  sensor_args sensor_ctx[SAMPLE_SENSORS];
  unsigned sensors_started = 0;
  for (; sensors_started < SAMPLE_SENSORS; ++sensors_started)
  {
    sensor_ctx[sensors_started].queue = &readings;
    sensor_ctx[sensors_started].id = sensors_started;
    if (!sensor_start(&sensors[sensors_started], &sensor_ctx[sensors_started]))
    {
      break;
    }
  }
  if (sensors_started < SAMPLE_SENSORS)
  {
    sensors_stop(&readings, sensors, sensors_started);
    reading_queue_deinit(&readings);
    sample_state_destroy(&state);
    return 1;
  }

  uint64_t end_ms = sample_now_ms() + SAMPLE_RUN_MS;
  while (!user_ctx.faulted && sample_now_ms() < end_ms)
  {
    (void)az_iot_connection_client_do_work(&state.connection_client, 100);

    if (user_ctx.rebuild)
    {
      user_ctx.rebuild = 0;
      printf(
          "DPS assigned %s; rebuilding the telemetry client.\n",
          az_iot_connection_profile_to_string(user_ctx.assigned_profile));
      telemetry_destroy(&state);
      user_ctx.send_pending = 0; /* lost with the old client */
      az_iot_connection_client_close(&state.connection_client);
      if (telemetry_build(&state, user_ctx.assigned_profile) != AZ_IOT_OK
          || az_iot_connection_client_open(&state.connection_client) != AZ_IOT_OK)
      {
        user_ctx.faulted = 1; /* recovery failed */
        break;
      }
      continue;
    }

    /* One reading per completed send. While not connected readings wait in the
     * queue, which drops the oldest when full. */
    reading r;
    if (user_ctx.conn_state == AZ_IOT_CONN_STATE_CONNECTED && !user_ctx.send_pending
        && reading_queue_pop(&readings, &r))
    {
      if (send_one(&state, &user_ctx, &r) != AZ_IOT_OK)
      {
        user_ctx.send_failed++;
      }
    }
  }

  /* Stop and join the sensors before their queue goes away. */
  sensors_stop(&readings, sensors, SAMPLE_SENSORS);
  unsigned const dropped = reading_queue_dropped(&readings);
  reading_queue_deinit(&readings);

  printf(
      "Sent %d message(s) on %s, %d failed, %u reading(s) dropped.\n",
      user_ctx.sent_ok,
      az_iot_connection_profile_to_string(state.profile),
      user_ctx.send_failed,
      dropped);

  az_iot_connection_client_close(&state.connection_client);
  for (int i = 0; i < 100 && user_ctx.conn_state != AZ_IOT_CONN_STATE_IDLE; ++i)
  {
    (void)az_iot_connection_client_do_work(&state.connection_client, 50);
  }

  sample_state_destroy(&state);

  return (user_ctx.sent_ok > 0 && !user_ctx.faulted) ? 0 : 1;
}
