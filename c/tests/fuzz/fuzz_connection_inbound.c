// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.
/* SPDX-License-Identifier: MIT */

/**
 * @file fuzz_connection_inbound.c
 * @brief Fuzz target for everything the hub and DPS send to a device.
 *
 * A connection client with the twin, C2D, direct method and file upload clients runs over the
 * in-memory mock MQTT adapter. The input scripts what the service does: inbound messages on
 * the SDK's topics, subscription acknowledgements, disconnects and reconnects, and the file
 * upload HTTP response.
 *
 * Input layout:
 * - byte 0: mode. Bit 0 set: provision through DPS first; clear: connect to the hub directly.
 * - byte 1: file upload HTTP status selector; bytes 2-3: body length (little endian); body.
 * - then records, one per event, until the input ends:
 *   - op byte: op % 8 selects the event (0-4 message, 5 SUBACK, 6 disconnect, 7 connect),
 *     op / 8 the topic prefix of a message;
 *   - message: suffix length, suffix, payload length (2 bytes, little endian), payload.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "azure/iot/az_iot_connection_client.h"
#include "azure/iot/az_iot_mqtt_iface.h"
#include "azure/iot/mqttv3/az_iot_c2d_client.h"
#include "azure/iot/mqttv3/az_iot_direct_method_client.h"
#include "azure/iot/mqttv3/az_iot_file_upload_client.h"
#include "azure/iot/mqttv3/az_iot_twin_client.h"
#include "fuzz_common.h"
#include "support/mock_mqtt_iface.h"
#include "support/subscription_ack.h"
#include "support/test_provider.h"

/** @brief Events one input may script; bounds the time per input. */
#define FUZZ_INBOUND_MAX_EVENTS 64

/** @brief Longest topic suffix a message may carry. */
#define FUZZ_INBOUND_SUFFIX_MAX 128

/** @brief Largest message payload or HTTP body taken from the input. */
#define FUZZ_INBOUND_PAYLOAD_MAX 2048

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

/** @brief Topic prefixes the SDK subscribes to; the input appends the rest. */
static const char* const k_topic_prefixes[] = {
  "$iothub/twin/res/",
  "$iothub/twin/PATCH/properties/desired/",
  "devices/fuzz-device/messages/devicebound/",
  "$iothub/methods/POST/",
  "$dps/registrations/res/",
  "",
};

/** @brief HTTP statuses the file upload hook reports. */
static const int k_http_statuses[] = { 200, 204, 400, 401, 404, 429, 500, 0 };

/** @brief Cursor over the fuzz input; reads past the end yield zeros and nothing. */
typedef struct fuzz_input
{
  const uint8_t* data;
  size_t size;
  size_t position;
} fuzz_input;

/** @brief The next byte, or 0 at the end of the input. */
static uint8_t fuzz_input_next_byte(fuzz_input* in)
{
  return in->position < in->size ? in->data[in->position++] : 0;
}

/** @brief The next 16-bit little-endian value. */
static uint16_t fuzz_input_next_u16(fuzz_input* in)
{
  uint16_t low = fuzz_input_next_byte(in);
  return (uint16_t)(low | ((uint16_t)fuzz_input_next_byte(in) << 8));
}

/** @brief Up to @p wanted next bytes; sets @p out to them and returns how many there were. */
static size_t fuzz_input_next_bytes(fuzz_input* in, size_t wanted, const uint8_t** out)
{
  size_t left = in->size - in->position;
  size_t taken = wanted < left ? wanted : left;
  *out = in->data + in->position;
  in->position += taken;
  return taken;
}

/** @brief Keeps reads of callback data from being optimized away, so the sanitizers see them. */
static volatile uint32_t g_sink;

/** @brief Read every byte of @p bytes. */
static void touch(const uint8_t* bytes, size_t length)
{
  for (size_t i = 0; i < length; ++i)
  {
    g_sink += bytes[i];
  }
}

/** @brief Read every byte of the C string @p text, if any. */
static void touch_string(const char* text)
{
  if (text != NULL)
  {
    touch((const uint8_t*)text, strlen(text));
  }
}

/** @brief The scripted file upload HTTP response. */
static struct
{
  int status;
  const uint8_t* body;
  size_t body_length;
} g_http_response;

static az_iot_result on_http_send(
    const char* method,
    const char* url,
    const char* authorization,
    const char* content_type,
    const uint8_t* body,
    size_t body_len,
    az_iot_file_upload_http_response* response,
    void* hook_ctx)
{
  (void)hook_ctx;
  touch_string(method);
  touch_string(url);
  touch_string(authorization);
  touch_string(content_type);
  touch(body, body_len);
  response->status_code = g_http_response.status;
  size_t length = g_http_response.body_length;
  if (length > response->body_capacity)
  {
    length = response->body_capacity;
  }
  if (length > 0)
  {
    memcpy(response->body, g_http_response.body, length);
  }
  response->body_len = length;
  return AZ_IOT_OK;
}

static void on_sas_uri(
    az_iot_result status,
    const char* blob_sas_uri,
    const char* correlation_id,
    void* user_ctx)
{
  (void)status;
  (void)user_ctx;
  touch_string(blob_sas_uri);
  touch_string(correlation_id);
}

static void on_c2d(const az_iot_c2d_message* msg, void* user_ctx)
{
  (void)user_ctx;
  touch(msg->payload, msg->payload_len);
}

static void on_method(
    az_iot_direct_method_request request,
    const char* method_name,
    const uint8_t* payload,
    size_t payload_len,
    void* user_ctx)
{
  touch_string(method_name);
  touch(payload, payload_len);
  static const uint8_t k_response[] = "{}";
  (void)az_iot_mqttv3_direct_method_respond(
      (az_iot_mqttv3_direct_method_client*)user_ctx,
      request,
      200,
      k_response,
      sizeof(k_response) - 1);
}

static void on_desired(
    const uint8_t* desired_patch,
    size_t desired_patch_len,
    uint64_t version,
    void* user_ctx)
{
  (void)version;
  (void)user_ctx;
  touch(desired_patch, desired_patch_len);
}

static void on_twin(
    az_iot_result status,
    const uint8_t* twin_payload,
    size_t twin_payload_len,
    void* user_ctx)
{
  (void)status;
  (void)user_ctx;
  touch(twin_payload, twin_payload_len);
}

/** @brief The device and its clients, built for one input. */
typedef struct fuzz_device
{
  az_iot_connection_client connection;
  az_iot_mqttv3_twin_client twin;
  az_iot_mqttv3_c2d_client c2d;
  az_iot_mqttv3_direct_method_client methods;
  az_iot_mqttv3_file_upload_client file_upload;
  uint8_t twin_workspace[AZ_IOT_MQTTV3_TWIN_CLIENT_DEFAULT_WORKSPACE_SIZE];
  uint8_t methods_workspace[AZ_IOT_MQTTV3_DIRECT_METHOD_CLIENT_DEFAULT_WORKSPACE_SIZE];
  bool connection_initialized;
} fuzz_device;

/** @brief Initialize the connection and its clients; false if any refused. */
static bool fuzz_device_init(fuzz_device* device, bool use_dps)
{
  az_iot_connection_client_options options = { 0 };
  if (use_dps)
  {
    options.dps.id_scope = "0ne00000000";
    options.dps.registration_id = "fuzz-device";
  }
  else
  {
    options.iothub_hostname = "hub.example";
    options.device_id = "fuzz-device";
  }
  options.port = 8883;
  options.mqtt_adapter = az_iot_mock_mqtt_get_adapter_for_version(AZ_IOT_MQTT_VERSION_3_1_1);
  if (az_iot_test_connection_client_init(&device->connection, &options) != AZ_IOT_OK)
  {
    return false;
  }
  device->connection_initialized = true;

  az_iot_mqttv3_twin_client_options twin_options = az_iot_mqttv3_twin_client_options_default();
  twin_options.workspace = device->twin_workspace;
  twin_options.workspace_size = sizeof(device->twin_workspace);
  az_iot_mqttv3_direct_method_client_options method_options
      = az_iot_mqttv3_direct_method_client_options_default();
  method_options.workspace = device->methods_workspace;
  method_options.workspace_size = sizeof(device->methods_workspace);
  static const az_iot_file_upload_http_transport k_transport = { on_http_send, NULL };

  if (az_iot_mqttv3_twin_client_init(&device->twin, &device->connection, &twin_options) != AZ_IOT_OK
      || az_iot_mqttv3_c2d_client_init(&device->c2d, &device->connection) != AZ_IOT_OK
      || az_iot_mqttv3_direct_method_client_init(
             &device->methods, &device->connection, &method_options)
          != AZ_IOT_OK
      || az_iot_mqttv3_file_upload_client_init(
             &device->file_upload, &device->connection, &k_transport)
          != AZ_IOT_OK)
  {
    return false;
  }
  (void)az_iot_mqttv3_twin_client_set_desired_handler(&device->twin, on_desired, NULL);
  (void)az_iot_mqttv3_c2d_client_set_handler(&device->c2d, on_c2d, NULL);
  (void)az_iot_mqttv3_direct_method_client_set_handler(
      &device->methods, on_method, &device->methods);
  return true;
}

/** @brief Tear down everything fuzz_device_init() set up; safe after a partial init. */
static void fuzz_device_deinit(fuzz_device* device)
{
  az_iot_mqttv3_file_upload_client_deinit(&device->file_upload);
  az_iot_mqttv3_direct_method_client_deinit(&device->methods);
  az_iot_mqttv3_c2d_client_deinit(&device->c2d);
  az_iot_mqttv3_twin_client_deinit(&device->twin);
  if (device->connection_initialized)
  {
    (void)az_iot_test_connection_client_deinit(&device->connection);
  }
}

/** @brief Accept the current MQTT connection and every subscription it has asked for. */
static void fuzz_device_accept(fuzz_device* device, az_iot_result status)
{
  az_iot_mock_mqtt_client* mqtt = az_iot_mock_mqtt_adapter_last_client();
  if (mqtt == NULL)
  {
    return;
  }
  (void)az_iot_mock_mqtt_client_inject_connected(mqtt, status);
  (void)az_iot_connection_client_do_work(&device->connection, 0);
  az_iot_test_ack_subscriptions(&device->connection, mqtt);
  az_iot_mock_mqtt_client_clear_calls(mqtt);
}

/** @brief Deliver one scripted inbound message. */
static void fuzz_device_message(fuzz_input* in, uint8_t op, az_iot_mock_mqtt_client* mqtt)
{
  const char* prefix = k_topic_prefixes
      [(size_t)(op / 8) % (sizeof(k_topic_prefixes) / sizeof(k_topic_prefixes[0]))];
  const uint8_t* suffix;
  size_t suffix_length = fuzz_input_next_bytes(
      in, (size_t)(fuzz_input_next_byte(in) % FUZZ_INBOUND_SUFFIX_MAX), &suffix);
  const uint8_t* payload;
  size_t payload_length = fuzz_input_next_bytes(
      in, (size_t)(fuzz_input_next_u16(in) % FUZZ_INBOUND_PAYLOAD_MAX), &payload);

  char topic[sizeof("devices/fuzz-device/messages/devicebound/") + FUZZ_INBOUND_SUFFIX_MAX];
  size_t prefix_length = strlen(prefix);
  memcpy(topic, prefix, prefix_length);
  memcpy(topic + prefix_length, suffix, suffix_length);
  topic[prefix_length + suffix_length] = '\0';
  (void)az_iot_mock_mqtt_client_inject_message(
      mqtt, topic, payload, payload_length, (op & 1) ? AZ_IOT_MQTT_QOS_1 : AZ_IOT_MQTT_QOS_0);
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
  fuzz_input in = { data, size, 0 };
  fuzz_abort_on_precondition_failure();
  bool use_dps = (fuzz_input_next_byte(&in) & 1) != 0;
  g_http_response.status = k_http_statuses
      [fuzz_input_next_byte(&in) % (sizeof(k_http_statuses) / sizeof(k_http_statuses[0]))];
  g_http_response.body_length = fuzz_input_next_bytes(
      &in, (size_t)(fuzz_input_next_u16(&in) % FUZZ_INBOUND_PAYLOAD_MAX), &g_http_response.body);

  static fuzz_device device;
  memset(&device, 0, sizeof(device));
  if (!fuzz_device_init(&device, use_dps)
      || az_iot_connection_client_open(&device.connection) != AZ_IOT_OK)
  {
    fuzz_device_deinit(&device);
    return 0;
  }
  fuzz_device_accept(&device, AZ_IOT_OK);
  /* Requests the script may answer; refused ones (not connected yet) are fine. */
  g_sink += (uint32_t)az_iot_mqttv3_twin_client_get(&device.twin, on_twin, NULL);
  g_sink += (uint32_t)az_iot_mqttv3_file_upload_client_get_sas_uri(
      &device.file_upload, "fuzz.bin", on_sas_uri, NULL);
  (void)az_iot_connection_client_do_work(&device.connection, 0);

  for (int event = 0; event < FUZZ_INBOUND_MAX_EVENTS && in.position < in.size; ++event)
  {
    uint8_t op = fuzz_input_next_byte(&in);
    az_iot_mock_mqtt_client* mqtt = az_iot_mock_mqtt_adapter_last_client();
    if (mqtt == NULL)
    {
      break;
    }
    switch (op % 8)
    {
      case 5:
        if ((op & 8) != 0)
        {
          (void)az_iot_mock_mqtt_client_inject_suback_refusal(
              mqtt, fuzz_input_next_u16(&in), (int32_t)fuzz_input_next_byte(&in));
        }
        else
        {
          (void)az_iot_mock_mqtt_client_inject_suback(mqtt, fuzz_input_next_u16(&in), AZ_IOT_OK);
        }
        break;
      case 6:
        (void)az_iot_mock_mqtt_client_inject_disconnected(mqtt);
        break;
      case 7:
        fuzz_device_accept(&device, (op & 8) != 0 ? AZ_IOT_ERR_AUTH : AZ_IOT_OK);
        break;
      default:
        fuzz_device_message(&in, op, mqtt);
        break;
    }
    (void)az_iot_connection_client_do_work(&device.connection, 0);
  }

  fuzz_device_deinit(&device);
  return 0;
}
