// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
#include "az_iot_e2e_service.h"

#include "e2e_amqp.h"
#include "e2e_http.h"
#include "e2e_sas.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#define E2E_SAS_TTL_SECONDS 3600
#define E2E_API_VERSION "2021-04-12"
/* Telemetry enqueued this long before the watch starts is still delivered, so a
 * runner clock ahead of the service cannot filter out the message under test. */
#define E2E_TELEMETRY_CLOCK_SKEW_S 300
/* Limits of one HTTPS request. An idempotent one is abandoned sooner, as it is retried. */
#define E2E_HTTPS_TIMEOUT_S 60
#define E2E_HTTPS_IDEMPOTENT_TIMEOUT_S 20
#define E2E_HTTPS_ATTEMPTS 3
#define E2E_HTTPS_RETRY_PAUSE_MS 2000u

struct az_iot_e2e_service
{
  e2e_conn_info hub_info; /* IoT Hub service endpoint (method/twin/c2d) */
  e2e_conn_info eh_info; /* Event Hub-compatible endpoint (telemetry)   */
  char entity[128]; /* Event Hub entity name                       */
  char consumer_group[64]; /* Event Hub consumer group; empty = $Default */
  int partition_count;

  e2e_amqp_telemetry* telemetry; /* non-NULL while watching */
  e2e_amqp_filenotify* filenotify; /* non-NULL while watching file notifications */
  /* Non-NULL while a REST call is in flight. [0] backs the single-request API;
   * the rest are handles. */
  e2e_http_request* http[AZ_IOT_E2E_SERVICE_MAX_REQUESTS];

  char last_error[256];
};

/* Read an environment variable into @p dst without triggering MSVC's C4996. */
static bool env_copy(const char* name, char* dst, size_t cap)
{
  if (cap == 0)
  {
    return false;
  }
  dst[0] = '\0';
#ifdef _WIN32
  size_t needed = 0;
  if (getenv_s(&needed, dst, cap, name) != 0 || needed == 0)
  {
    dst[0] = '\0';
    return false;
  }
  return true;
#else
  const char* value = getenv(name);
  if (value == NULL || value[0] == '\0')
  {
    return false;
  }
  snprintf(dst, cap, "%s", value);
  return true;
#endif
}

static void set_error(az_iot_e2e_service* svc, const char* message)
{
  snprintf(svc->last_error, sizeof(svc->last_error), "%s", (message != NULL) ? message : "unknown");
}

static int64_t sas_expiry(void) { return (int64_t)time(NULL) + E2E_SAS_TTL_SECONDS; }

/** @brief Close and free REST slot @p slot, if in use. */
static void end_slot(az_iot_e2e_service* svc, int slot)
{
  if (svc->http[slot] != NULL)
  {
    e2e_http_end(svc->http[slot]);
    free(svc->http[slot]);
    svc->http[slot] = NULL;
  }
}

az_iot_e2e_service* az_iot_e2e_service_create(const char** error_out)
{
  char hub_cs[512];
  char eh_cs[512];
  if (!env_copy("IOTHUB_CONNECTION_STRING", hub_cs, sizeof(hub_cs)))
  {
    if (error_out != NULL)
    {
      *error_out = "IOTHUB_CONNECTION_STRING not set";
    }
    return NULL;
  }
  if (!env_copy("IOTHUB_EVENTHUB_CONNECTION_STRING", eh_cs, sizeof(eh_cs)))
  {
    if (error_out != NULL)
    {
      *error_out = "IOTHUB_EVENTHUB_CONNECTION_STRING not set";
    }
    return NULL;
  }

  az_iot_e2e_service* svc = (az_iot_e2e_service*)calloc(1, sizeof(*svc));
  if (svc == NULL)
  {
    if (error_out != NULL)
    {
      *error_out = "out of memory";
    }
    return NULL;
  }

  if (!e2e_conn_info_parse(hub_cs, &svc->hub_info))
  {
    free(svc);
    if (error_out != NULL)
    {
      *error_out = "IOTHUB_CONNECTION_STRING is malformed";
    }
    return NULL;
  }
  if (!e2e_conn_info_parse(eh_cs, &svc->eh_info))
  {
    free(svc);
    if (error_out != NULL)
    {
      *error_out = "IOTHUB_EVENTHUB_CONNECTION_STRING is malformed";
    }
    return NULL;
  }

  /* Event Hub entity name: from the connection string's EntityPath, else the
   * explicit listen-name env var. */
  if (svc->eh_info.entity_path[0] != '\0')
  {
    snprintf(svc->entity, sizeof(svc->entity), "%s", svc->eh_info.entity_path);
  }
  else if (!env_copy("IOTHUB_EVENTHUB_LISTEN_NAME", svc->entity, sizeof(svc->entity)))
  {
    free(svc);
    if (error_out != NULL)
    {
      *error_out = "Event Hub entity name not found (EntityPath / IOTHUB_EVENTHUB_LISTEN_NAME)";
    }
    return NULL;
  }

  char partitions[16];
  svc->partition_count = 4;
  if (env_copy("IOTHUB_EVENTHUB_PARTITION_COUNT", partitions, sizeof(partitions)))
  {
    int parsed = atoi(partitions);
    if (parsed >= 1)
    {
      svc->partition_count = parsed;
    }
  }

  if (!env_copy("IOTHUB_EVENTHUB_CONSUMER_GROUP", svc->consumer_group, sizeof(svc->consumer_group)))
  {
    svc->consumer_group[0] = '\0';
  }

  return svc;
}

void az_iot_e2e_service_destroy(az_iot_e2e_service* svc)
{
  if (svc == NULL)
  {
    return;
  }
  if (svc->telemetry != NULL)
  {
    e2e_amqp_telemetry_end(svc->telemetry);
    free(svc->telemetry);
  }
  if (svc->filenotify != NULL)
  {
    e2e_amqp_filenotify_end(svc->filenotify);
    free(svc->filenotify);
  }
  for (int i = 0; i < AZ_IOT_E2E_SERVICE_MAX_REQUESTS; i++)
  {
    end_slot(svc, i);
  }
  free(svc);
}

const char* az_iot_e2e_service_last_error(const az_iot_e2e_service* svc)
{
  if (svc == NULL || svc->last_error[0] == '\0')
  {
    return NULL;
  }
  return svc->last_error;
}

bool az_iot_e2e_service_telemetry_watch_begin(az_iot_e2e_service* svc, const char* capture_filter)
{
  if (svc->telemetry != NULL)
  {
    if (strcmp(svc->telemetry->capture_filter, capture_filter != NULL ? capture_filter : "") == 0)
    {
      return true; /* already watching */
    }
    az_iot_e2e_service_telemetry_watch_end(svc);
  }

  /* Event Hubs authorizes the entity audience with a SAS whose HMAC key is the
   * raw key string (NOT base64-decoded) — the Event Hubs/Service Bus convention. */
  /* Sized for the widest possible "amqps://<host>/<entity>": host[256] + entity[128]
   * plus scheme and separator. A tighter bound trips GCC -Werror=format-truncation. */
  char audience[512];
  snprintf(audience, sizeof(audience), "amqps://%s/%s", svc->eh_info.host, svc->entity);

  char sas[512];
  if (!e2e_sas_token_create(
          audience, svc->eh_info.key_name, svc->eh_info.key, false, sas_expiry(), sas, sizeof(sas)))
  {
    set_error(svc, "telemetry: failed to build SAS token");
    return false;
  }

  svc->telemetry = (e2e_amqp_telemetry*)calloc(1, sizeof(*svc->telemetry));
  if (svc->telemetry == NULL)
  {
    set_error(svc, "telemetry: out of memory");
    return false;
  }

  const char* err = NULL;
  int64_t enqueued_after_ms = ((int64_t)time(NULL) - E2E_TELEMETRY_CLOCK_SKEW_S) * 1000;
  if (!e2e_amqp_telemetry_begin(
          svc->telemetry,
          svc->eh_info.host,
          svc->entity,
          svc->consumer_group,
          enqueued_after_ms,
          sas,
          svc->partition_count,
          capture_filter,
          &err))
  {
    set_error(svc, (err != NULL) ? err : "telemetry: begin failed");
    free(svc->telemetry);
    svc->telemetry = NULL;
    return false;
  }
  return true;
}

bool az_iot_e2e_service_do_work(az_iot_e2e_service* svc, int timeout_ms)
{
  /* Split the budget when both watchers are up so neither starves the other,
   * but never below 1ms: halving a 1ms budget would turn both pumps into
   * no-wait calls and busy-spin the caller. */
  int slice = timeout_ms;
  if (svc->telemetry != NULL && svc->filenotify != NULL && timeout_ms > 1)
  {
    slice = timeout_ms / 2;
  }

  if (svc->telemetry != NULL && !e2e_amqp_telemetry_do_work(svc->telemetry, slice))
  {
    set_error(svc, "telemetry: connection failed");
    return false;
  }
  if (svc->filenotify != NULL && !e2e_amqp_filenotify_do_work(svc->filenotify, slice))
  {
    set_error(svc, "filenotify: connection failed");
    return false;
  }
  return true;
}

bool az_iot_e2e_service_telemetry_seen(const az_iot_e2e_service* svc, const char* needle)
{
  return svc->telemetry != NULL && e2e_amqp_telemetry_seen(svc->telemetry, needle);
}

bool az_iot_e2e_service_telemetry_watch_needle(az_iot_e2e_service* svc, const char* needle)
{
  if (svc->telemetry == NULL)
  {
    set_error(svc, "telemetry: not watching");
    return false;
  }
  if (!e2e_amqp_telemetry_watch_needle(svc->telemetry, needle))
  {
    set_error(svc, "telemetry: watched needle table full, or needle empty or too long");
    return false;
  }
  return true;
}

bool az_iot_e2e_service_telemetry_needle_seen(const az_iot_e2e_service* svc, const char* needle)
{
  return svc->telemetry != NULL && e2e_amqp_telemetry_watched_seen(svc->telemetry, needle);
}

static const e2e_amqp_capture* find_telemetry(const az_iot_e2e_service* svc, const char* needle)
{
  return svc->telemetry != NULL ? e2e_amqp_telemetry_find(svc->telemetry, needle) : NULL;
}

int az_iot_e2e_service_telemetry_index(const az_iot_e2e_service* svc, const char* needle)
{
  const e2e_amqp_capture* c = find_telemetry(svc, needle);
  return c != NULL ? (int)c->seq : -1;
}

bool az_iot_e2e_service_telemetry_body_size(
    const az_iot_e2e_service* svc,
    const char* needle,
    size_t* size)
{
  const e2e_amqp_capture* c = find_telemetry(svc, needle);
  if (c == NULL)
  {
    return false;
  }
  *size = c->body_size;
  return true;
}

bool az_iot_e2e_service_telemetry_property(
    const az_iot_e2e_service* svc,
    const char* needle,
    const char* name,
    char* out,
    size_t out_size)
{
  const e2e_amqp_capture* c = find_telemetry(svc, needle);
  if (c != NULL && e2e_amqp_capture_property(c, name, out, out_size))
  {
    return true;
  }
  printf("[e2e] telemetry property '%s' not found\n", name);
  az_iot_e2e_service_telemetry_dump(svc, needle);
  return false;
}

bool az_iot_e2e_service_telemetry_find_value(
    const az_iot_e2e_service* svc,
    const char* needle,
    const char* value,
    char* name_out,
    size_t name_out_size)
{
  const e2e_amqp_capture* c = find_telemetry(svc, needle);
  return c != NULL && e2e_amqp_capture_find_value(c, value, name_out, name_out_size);
}

void az_iot_e2e_service_telemetry_dump(const az_iot_e2e_service* svc, const char* needle)
{
  const e2e_amqp_capture* c = find_telemetry(svc, needle);
  if (c == NULL)
  {
    printf(
        "[e2e] no telemetry containing '%s' (%u messages received)\n",
        needle,
        svc->telemetry != NULL ? (unsigned)svc->telemetry->received_count : 0u);
    return;
  }
  e2e_amqp_capture_dump(c);
}

void az_iot_e2e_service_telemetry_watch_end(az_iot_e2e_service* svc)
{
  if (svc->telemetry != NULL)
  {
    e2e_amqp_telemetry_end(svc->telemetry);
    free(svc->telemetry);
    svc->telemetry = NULL;
  }
}

bool az_iot_e2e_service_file_notification_watch_begin(
    az_iot_e2e_service* svc,
    const char* device_id,
    bool* attach_refused_out)
{
  if (attach_refused_out != NULL)
  {
    *attach_refused_out = false;
  }
  if (svc->filenotify != NULL)
  {
    return true; /* already watching */
  }

  /* IoT Hub service SAS: HMAC key is base64-decode(key); audience is the host. */
  char sas[512];
  if (!e2e_sas_token_create(
          svc->hub_info.host,
          svc->hub_info.key_name,
          svc->hub_info.key,
          true,
          sas_expiry(),
          sas,
          sizeof(sas)))
  {
    set_error(svc, "filenotify: failed to build SAS token");
    return false;
  }

  svc->filenotify = (e2e_amqp_filenotify*)calloc(1, sizeof(*svc->filenotify));
  if (svc->filenotify == NULL)
  {
    set_error(svc, "filenotify: out of memory");
    return false;
  }

  const char* err = NULL;
  if (!e2e_amqp_filenotify_begin(
          svc->filenotify, svc->hub_info.host, sas, device_id, &err, attach_refused_out))
  {
    set_error(svc, (err != NULL) ? err : "filenotify: begin failed");
    free(svc->filenotify);
    svc->filenotify = NULL;
    return false;
  }
  return true;
}

bool az_iot_e2e_service_file_notification_seen(const az_iot_e2e_service* svc, const char* needle)
{
  return svc->filenotify != NULL && e2e_amqp_filenotify_seen(svc->filenotify, needle);
}

void az_iot_e2e_service_file_notification_stats(
    const az_iot_e2e_service* svc,
    int* out_delivered,
    int* out_captured,
    int* out_released,
    int* out_stale,
    int* out_unparsed)
{
  if (svc->filenotify != NULL)
  {
    e2e_amqp_filenotify_stats(
        svc->filenotify, out_delivered, out_captured, out_released, out_stale, out_unparsed);
    return;
  }
  if (out_delivered != NULL)
  {
    *out_delivered = 0;
  }
  if (out_captured != NULL)
  {
    *out_captured = 0;
  }
  if (out_released != NULL)
  {
    *out_released = 0;
  }
  if (out_stale != NULL)
  {
    *out_stale = 0;
  }
  if (out_unparsed != NULL)
  {
    *out_unparsed = 0;
  }
}

void az_iot_e2e_service_file_notification_watch_end(az_iot_e2e_service* svc)
{
  if (svc->filenotify != NULL)
  {
    e2e_amqp_filenotify_end(svc->filenotify);
    free(svc->filenotify);
    svc->filenotify = NULL;
  }
}

bool az_iot_e2e_service_send_c2d(
    az_iot_e2e_service* svc,
    const char* device_id,
    const uint8_t* payload,
    size_t payload_len)
{
  return az_iot_e2e_service_send_c2d_with_properties(svc, device_id, payload, payload_len, NULL);
}

bool az_iot_e2e_service_send_c2d_with_properties(
    az_iot_e2e_service* svc,
    const char* device_id,
    const uint8_t* payload,
    size_t payload_len,
    const az_iot_e2e_c2d_properties* properties)
{
  /* IoT Hub service SAS: HMAC key is base64-decode(key); audience is the host. */
  char sas[512];
  if (!e2e_sas_token_create(
          svc->hub_info.host,
          svc->hub_info.key_name,
          svc->hub_info.key,
          true,
          sas_expiry(),
          sas,
          sizeof(sas)))
  {
    set_error(svc, "c2d: failed to build SAS token");
    return false;
  }

  const char* err = NULL;
  if (!e2e_amqp_send_c2d(
          svc->hub_info.host, sas, device_id, payload, payload_len, properties, &err))
  {
    set_error(svc, (err != NULL) ? err : "c2d: send failed");
    return false;
  }
  return true;
}

/* Build the IoT Hub service SAS used for the REST APIs. */
static bool build_hub_sas(az_iot_e2e_service* svc, char* out, size_t out_size)
{
  if (!e2e_sas_token_create(
          svc->hub_info.host,
          svc->hub_info.key_name,
          svc->hub_info.key,
          true,
          sas_expiry(),
          out,
          out_size))
  {
    set_error(svc, "rest: failed to build SAS token");
    return false;
  }
  return true;
}

/** @brief Start a REST request in @p slot, replacing any previous one there. */
static bool begin_request_in(
    az_iot_e2e_service* svc,
    int slot,
    const char* method,
    const char* path,
    const char* sas,
    const char* body)
{
  end_slot(svc, slot);
  e2e_http_request* r = (e2e_http_request*)calloc(1, sizeof(*r));
  if (r == NULL)
  {
    set_error(svc, "rest: out of memory");
    return false;
  }
  if (!e2e_http_begin(r, svc->hub_info.host, method, path, sas, body))
  {
    set_error(svc, e2e_http_error(r));
    free(r);
    return false;
  }
  svc->http[slot] = r;
  return true;
}

/** @brief Start a REST request in the single-request slot. */
static bool begin_request(
    az_iot_e2e_service* svc,
    const char* method,
    const char* path,
    const char* sas,
    const char* body)
{
  return begin_request_in(svc, 0, method, path, sas, body);
}

/** @brief Start a direct-method invocation in @p slot. */
static bool begin_method_invoke(
    az_iot_e2e_service* svc,
    int slot,
    const char* device_id,
    const char* method_name,
    const char* json_payload,
    int response_timeout_s,
    int connect_timeout_s)
{
  char sas[512];
  if (!build_hub_sas(svc, sas, sizeof(sas)))
  {
    return false;
  }
  char path[256];
  snprintf(path, sizeof(path), "/twins/%s/methods?api-version=%s", device_id, E2E_API_VERSION);
  char body[2048];
  int n = snprintf(
      body,
      sizeof(body),
      "{\"methodName\":\"%s\",\"responseTimeoutInSeconds\":%d,"
      "\"connectTimeoutInSeconds\":%d,\"payload\":%s}",
      method_name,
      response_timeout_s,
      connect_timeout_s,
      (json_payload != NULL) ? json_payload : "null");
  if (n < 0 || n >= (int)sizeof(body))
  {
    set_error(svc, "rest: method invocation body too large");
    return false;
  }
  return begin_request_in(svc, slot, "POST", path, sas, body);
}

bool az_iot_e2e_service_method_invoke_begin(
    az_iot_e2e_service* svc,
    const char* device_id,
    const char* method_name,
    const char* json_payload)
{
  return az_iot_e2e_service_method_invoke_with_timeout_begin(
      svc, device_id, method_name, json_payload, 30, 30);
}

bool az_iot_e2e_service_method_invoke_with_timeout_begin(
    az_iot_e2e_service* svc,
    const char* device_id,
    const char* method_name,
    const char* json_payload,
    int response_timeout_s,
    int connect_timeout_s)
{
  return begin_method_invoke(
      svc, 0, device_id, method_name, json_payload, response_timeout_s, connect_timeout_s);
}

int az_iot_e2e_service_method_invoke_start(
    az_iot_e2e_service* svc,
    const char* device_id,
    const char* method_name,
    const char* json_payload,
    int response_timeout_s,
    int connect_timeout_s)
{
  for (int slot = 1; slot < AZ_IOT_E2E_SERVICE_MAX_REQUESTS; slot++)
  {
    if (svc->http[slot] == NULL)
    {
      return begin_method_invoke(
                 svc,
                 slot,
                 device_id,
                 method_name,
                 json_payload,
                 response_timeout_s,
                 connect_timeout_s)
          ? slot
          : -1;
    }
  }
  set_error(svc, "rest: no free request slot");
  return -1;
}

bool az_iot_e2e_service_twin_get_begin(az_iot_e2e_service* svc, const char* device_id)
{
  char sas[512];
  if (!build_hub_sas(svc, sas, sizeof(sas)))
  {
    return false;
  }
  char path[256];
  snprintf(path, sizeof(path), "/twins/%s?api-version=%s", device_id, E2E_API_VERSION);
  return begin_request(svc, "GET", path, sas, NULL);
}

bool az_iot_e2e_service_twin_patch_desired_begin(
    az_iot_e2e_service* svc,
    const char* device_id,
    const char* desired_json)
{
  char sas[512];
  if (!build_hub_sas(svc, sas, sizeof(sas)))
  {
    return false;
  }
  char path[256];
  snprintf(path, sizeof(path), "/twins/%s?api-version=%s", device_id, E2E_API_VERSION);
  /* Large enough for a software updates deployment desired PATCH (~3 KB: signed JWS +
   * escaped v5 manifest) wrapped in the properties/desired envelope. */
  char body[4096];
  snprintf(
      body,
      sizeof(body),
      "{\"properties\":{\"desired\":%s}}",
      (desired_json != NULL) ? desired_json : "{}");
  return begin_request(svc, "PATCH", path, sas, body);
}

/** @brief Advance REST slot @p slot; on completion copy out and release it. */
static int poll_slot(
    az_iot_e2e_service* svc,
    int slot,
    int* out_http_status,
    char* resp_buf,
    size_t resp_buf_size)
{
  e2e_http_request* r = svc->http[slot];
  if (r == NULL)
  {
    set_error(svc, "rest: no request in flight");
    return -1;
  }

  int rc = e2e_http_poll(r);
  if (rc == 0)
  {
    return 0;
  }

  if (rc == 1)
  {
    if (out_http_status != NULL)
    {
      *out_http_status = e2e_http_status(r);
    }
    if (resp_buf != NULL && resp_buf_size > 0)
    {
      int body_len = 0;
      const uint8_t* body = e2e_http_body(r, &body_len);
      size_t copy = (size_t)body_len;
      if (copy > resp_buf_size - 1)
      {
        copy = resp_buf_size - 1;
      }
      memcpy(resp_buf, body, copy);
      resp_buf[copy] = '\0';
    }
  }
  else
  {
    set_error(svc, e2e_http_error(r));
  }

  end_slot(svc, slot);
  return rc;
}

int az_iot_e2e_service_request_poll(
    az_iot_e2e_service* svc,
    int* out_http_status,
    char* resp_buf,
    size_t resp_buf_size)
{
  return poll_slot(svc, 0, out_http_status, resp_buf, resp_buf_size);
}

int az_iot_e2e_service_request_poll_handle(
    az_iot_e2e_service* svc,
    int handle,
    int* out_http_status,
    char* resp_buf,
    size_t resp_buf_size)
{
  if (handle < 1 || handle >= AZ_IOT_E2E_SERVICE_MAX_REQUESTS)
  {
    set_error(svc, "rest: invalid request handle");
    return -1;
  }
  return poll_slot(svc, handle, out_http_status, resp_buf, resp_buf_size);
}

bool az_iot_e2e_service_request_is_sent(const az_iot_e2e_service* svc, int handle)
{
  if (svc == NULL || handle < 1 || handle >= AZ_IOT_E2E_SERVICE_MAX_REQUESTS
      || svc->http[handle] == NULL)
  {
    return false;
  }
  const e2e_http_request* r = svc->http[handle];
  return r->phase == E2E_HTTP_PHASE_READING || r->phase == E2E_HTTP_PHASE_DONE;
}

void az_iot_e2e_service_request_cancel(az_iot_e2e_service* svc, int handle)
{
  if (handle >= 1 && handle < AZ_IOT_E2E_SERVICE_MAX_REQUESTS)
  {
    end_slot(svc, handle);
  }
}

/* ---- Device-side HTTPS helper --------------------------------------------- */

/* Read a whole file into a NUL-terminated heap buffer (caller frees). */
static char* read_file_alloc(const char* path)
{
#ifdef _WIN32
  FILE* f = NULL;
  if (fopen_s(&f, path, "rb") != 0 || f == NULL)
  {
    return NULL;
  }
#else
  FILE* f = fopen(path, "rb");
  if (f == NULL)
  {
    return NULL;
  }
#endif
  char* buf = NULL;
  if (fseek(f, 0, SEEK_END) == 0)
  {
    long size = ftell(f);
    if (size >= 0 && fseek(f, 0, SEEK_SET) == 0)
    {
      buf = (char*)malloc((size_t)size + 1);
      if (buf != NULL)
      {
        size_t got = fread(buf, 1, (size_t)size, f);
        buf[got] = '\0';
      }
    }
  }
  fclose(f);
  return buf;
}

/** @brief Waits @p ms milliseconds between two attempts of a request. */
static void pause_between_attempts(unsigned ms)
{
#ifdef _WIN32
  Sleep(ms);
#else
  struct timespec delay = { (time_t)(ms / 1000u), (long)(ms % 1000u) * 1000000L };
  (void)nanosleep(&delay, NULL);
#endif
}

/**
 * @brief Logs why an attempt of @p method on @p host @p path failed, without the query
 * string (it can hold a SAS token).
 */
static void log_failed_attempt(
    const e2e_http_request* r,
    const char* host,
    const char* method,
    const char* path,
    int attempt,
    long elapsed_s)
{
  const char* err = e2e_http_error(r);
  const char* native = (const char*)r->transport_storage.last_error_message;
  (void)fprintf(
      stderr,
      "[e2e] HTTPS %s %s%.*s, attempt %d of %d, after %lds: %s%s; %d of %d request bytes sent, "
      "%d response bytes received%s%s\n",
      method,
      host,
      (int)strcspn(path, "?"),
      path,
      attempt,
      E2E_HTTPS_ATTEMPTS,
      elapsed_s,
      err != NULL ? err : "no complete response while ",
      err != NULL ? "" : e2e_http_phase_name(r->phase),
      r->request_sent,
      r->request_len,
      r->response_len,
      native[0] != '\0' ? "; transport: " : "",
      native);
}

/**
 * @brief Sends one HTTPS request and waits for its response.
 *
 * A request with no response yet is sent again on a new connection, up to
 * #E2E_HTTPS_ATTEMPTS times, when e2e_http_retry_is_safe() allows it. An @p idempotent
 * request is abandoned after #E2E_HTTPS_IDEMPOTENT_TIMEOUT_S rather than
 * #E2E_HTTPS_TIMEOUT_S, as it can be retried whatever stage it reached.
 *
 * @return true when a complete response arrived. @p out_status is set once a status line
 * arrived; the other outputs once the header block did, even with an incomplete body.
 */
static bool https_request(
    const char* host,
    const char* method,
    const char* path,
    const char* authorization,
    const char* client_cert_path,
    const char* client_key_path,
    const char* content_type,
    const char* extra_header,
    const void* body,
    size_t body_len,
    bool idempotent,
    int* out_status,
    char* resp_buf,
    size_t resp_buf_size,
    size_t* out_resp_len,
    unsigned* out_retry_after)
{
  char* cert_pem = (client_cert_path != NULL) ? read_file_alloc(client_cert_path) : NULL;
  char* key_pem = (client_key_path != NULL) ? read_file_alloc(client_key_path) : NULL;
  if ((client_cert_path != NULL && cert_pem == NULL)
      || (client_key_path != NULL && key_pem == NULL))
  {
    free(cert_pem);
    free(key_pem);
    return false;
  }

  e2e_http_request* r = (e2e_http_request*)calloc(1, sizeof(*r));
  if (r == NULL)
  {
    free(cert_pem);
    free(key_pem);
    return false;
  }

  const long timeout_s = idempotent ? E2E_HTTPS_IDEMPOTENT_TIMEOUT_S : E2E_HTTPS_TIMEOUT_S;
  bool ok = false;
  for (int attempt = 1; attempt <= E2E_HTTPS_ATTEMPTS; attempt++)
  {
    if (attempt > 1)
    {
      pause_between_attempts(E2E_HTTPS_RETRY_PAUSE_MS);
    }

    time_t start = time(NULL);
    bool begun = e2e_http_begin_ex(
        r,
        host,
        method,
        path,
        authorization,
        content_type,
        extra_header,
        body,
        body_len,
        cert_pem,
        key_pem);
    int rc = -1;
    if (begun)
    {
      do
      {
        rc = e2e_http_poll(r);
      } while (rc == 0 && (time(NULL) - start) < timeout_s);
    }
    ok = rc == 1;
    if (!ok)
    {
      log_failed_attempt(r, host, method, path, attempt, (long)(time(NULL) - start));
    }
    bool retry = !ok && begun && e2e_http_retry_is_safe(r, idempotent);
    if (!retry)
    {
      break;
    }
    e2e_http_end(r);
  }

  if (out_status != NULL && e2e_http_status(r) != 0)
  {
    *out_status = e2e_http_status(r);
  }
  if (r->headers_done)
  {
    if (out_retry_after != NULL)
    {
      *out_retry_after = e2e_http_retry_after(r);
    }
    int b_len = 0;
    const uint8_t* b = e2e_http_body(r, &b_len);
    size_t copy = (size_t)b_len;
    if (resp_buf != NULL && resp_buf_size > 0)
    {
      if (copy > resp_buf_size - 1)
      {
        copy = resp_buf_size - 1;
      }
      memcpy(resp_buf, b, copy);
      resp_buf[copy] = '\0';
    }
    if (out_resp_len != NULL)
    {
      *out_resp_len = copy;
    }
  }

  e2e_http_end(r);
  free(r);
  free(cert_pem);
  free(key_pem);
  return ok;
}

bool az_iot_e2e_https_request(
    const char* host,
    const char* method,
    const char* path,
    const char* client_cert_path,
    const char* client_key_path,
    const char* content_type,
    const char* extra_header,
    const void* body,
    size_t body_len,
    int* out_status,
    char* resp_buf,
    size_t resp_buf_size,
    size_t* out_resp_len)
{
  return https_request(
      host,
      method,
      path,
      NULL,
      client_cert_path,
      client_key_path,
      content_type,
      extra_header,
      body,
      body_len,
      false,
      out_status,
      resp_buf,
      resp_buf_size,
      out_resp_len,
      NULL);
}

bool az_iot_e2e_https_request_auth(
    const char* host,
    const char* method,
    const char* path,
    const char* authorization,
    const char* content_type,
    const void* body,
    size_t body_len,
    int* out_status,
    char* resp_buf,
    size_t resp_buf_size,
    size_t* out_resp_len,
    unsigned* out_retry_after)
{
  return https_request(
      host,
      method,
      path,
      authorization,
      NULL,
      NULL,
      content_type,
      NULL,
      body,
      body_len,
      false,
      out_status,
      resp_buf,
      resp_buf_size,
      out_resp_len,
      out_retry_after);
}

/* ---- DPS service: individual enrollments -------------------------------------- */

#define E2E_DPS_API_VERSION "2021-10-01"

/** @brief The DPS service host and a SAS token from IOT_DPS_CONNECTION_STRING. */
static bool dps_service_auth(e2e_conn_info* info, char* sas, size_t sas_size)
{
  char cs[512];
  return env_copy("IOT_DPS_CONNECTION_STRING", cs, sizeof(cs)) && e2e_conn_info_parse(cs, info)
      && e2e_sas_token_create(
             info->host, info->key_name, info->key, true, sas_expiry(), sas, sas_size);
}

/** @brief A DPS service REST call; true for a 2xx (or 404 when @p allow_404). */
static bool dps_service_call(const char* method, const char* path, const char* body, bool allow_404)
{
  e2e_conn_info info;
  char sas[512];
  if (!dps_service_auth(&info, sas, sizeof(sas)))
  {
    return false;
  }
  int status = 0;
  char resp[1024] = { 0 };
  bool ok = https_request(
      info.host,
      method,
      path,
      sas,
      NULL,
      NULL,
      body != NULL ? "application/json" : NULL,
      strcmp(method, "DELETE") == 0 ? "If-Match: *" : NULL,
      body,
      body != NULL ? strlen(body) : 0,
      strcmp(method, "DELETE") == 0,
      &status,
      resp,
      sizeof(resp),
      NULL,
      NULL);
  bool done = ok && ((status >= 200 && status < 300) || (allow_404 && status == 404));
  if (!done)
  {
    (void)fprintf(stderr, "[e2e] DPS service %s %s: HTTP %d %s\n", method, path, status, resp);
  }
  return done;
}

bool az_iot_e2e_dps_tpm_enrollment_create(
    const char* registration_id,
    const char* endorsement_key_base64)
{
  char path[256];
  char body[2048];
  int n = snprintf(
      path, sizeof(path), "/enrollments/%s?api-version=" E2E_DPS_API_VERSION, registration_id);
  int m = snprintf(
      body,
      sizeof(body),
      "{\"registrationId\":\"%s\",\"attestation\":{\"type\":\"tpm\",\"tpm\":"
      "{\"endorsementKey\":\"%s\"}}}",
      registration_id,
      endorsement_key_base64);
  return n > 0 && (size_t)n < sizeof(path) && m > 0 && (size_t)m < sizeof(body)
      && dps_service_call("PUT", path, body, false);
}

bool az_iot_e2e_dps_enrollment_delete(const char* registration_id)
{
  char path[256];
  int n = snprintf(
      path, sizeof(path), "/registrations/%s?api-version=" E2E_DPS_API_VERSION, registration_id);
  bool ok = n > 0 && (size_t)n < sizeof(path) && dps_service_call("DELETE", path, NULL, true);
  n = snprintf(
      path, sizeof(path), "/enrollments/%s?api-version=" E2E_DPS_API_VERSION, registration_id);
  return n > 0 && (size_t)n < sizeof(path) && dps_service_call("DELETE", path, NULL, true) && ok;
}

bool az_iot_e2e_service_device_delete(az_iot_e2e_service* svc, const char* device_id)
{
  /* The policy of IOTHUB_REGISTRY_CONNECTION_STRING (RegistryWrite) when set, else
   * that of the service's hub connection string. The request always goes to the
   * service's host: a hub with a separate device hostname refuses service
   * operations on it. */
  e2e_conn_info info = svc->hub_info;
  char cs[512];
  if (env_copy("IOTHUB_REGISTRY_CONNECTION_STRING", cs, sizeof(cs)))
  {
    e2e_conn_info registry;
    if (!e2e_conn_info_parse(cs, &registry))
    {
      (void)fprintf(stderr, "[e2e] IOTHUB_REGISTRY_CONNECTION_STRING is malformed\n");
      return false;
    }
    memcpy(info.key_name, registry.key_name, sizeof(info.key_name));
    memcpy(info.key, registry.key, sizeof(info.key));
  }
  char sas[512];
  char path[256];
  int n = snprintf(path, sizeof(path), "/devices/%s?api-version=%s", device_id, E2E_API_VERSION);
  if (n <= 0 || (size_t)n >= sizeof(path)
      || !e2e_sas_token_create(
          info.host, info.key_name, info.key, true, sas_expiry(), sas, sizeof(sas)))
  {
    return false;
  }
  int status = 0;
  char resp[512] = { 0 };
  bool ok = https_request(
      info.host,
      "DELETE",
      path,
      sas,
      NULL,
      NULL,
      NULL,
      "If-Match: \"*\"",
      NULL,
      0,
      true,
      &status,
      resp,
      sizeof(resp),
      NULL,
      NULL);
  bool done = ok && ((status >= 200 && status < 300) || status == 404);
  if (!done)
  {
    (void)fprintf(stderr, "[e2e] hub DELETE /devices/%s: HTTP %d %s\n", device_id, status, resp);
  }
  return done;
}
