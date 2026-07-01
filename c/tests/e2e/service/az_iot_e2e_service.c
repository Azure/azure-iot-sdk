// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/* SPDX-License-Identifier: MIT */
#include "az_iot_e2e_service.h"

#include "e2e_amqp.h"
#include "e2e_http.h"
#include "e2e_sas.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define E2E_SAS_TTL_SECONDS 3600
#define E2E_API_VERSION "2021-04-12"

struct az_iot_e2e_service
{
    e2e_conn_info_t hub_info; /* IoT Hub service endpoint (method/twin/c2d) */
    e2e_conn_info_t eh_info; /* Event Hub-compatible endpoint (telemetry)   */
    char entity[128]; /* Event Hub entity name                       */
    int partition_count;

    e2e_amqp_telemetry_t* telemetry; /* non-NULL while watching */
    e2e_http_request_t* http; /* non-NULL while a REST call is in flight */

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
    if (svc->http != NULL)
    {
        e2e_http_end(svc->http);
        free(svc->http);
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

bool az_iot_e2e_service_telemetry_watch_begin(az_iot_e2e_service* svc)
{
    if (svc->telemetry != NULL)
    {
        return true; /* already watching */
    }

    /* Event Hubs authorizes the entity audience with a SAS whose HMAC key is the
     * raw key string (NOT base64-decoded) — the Event Hubs/Service Bus convention. */
    char audience[320];
    snprintf(audience, sizeof(audience), "amqps://%s/%s", svc->eh_info.host, svc->entity);

    char sas[512];
    if (!e2e_sas_token_create(
            audience, svc->eh_info.key_name, svc->eh_info.key, false, sas_expiry(), sas, sizeof(sas)))
    {
        set_error(svc, "telemetry: failed to build SAS token");
        return false;
    }

    svc->telemetry = (e2e_amqp_telemetry_t*)calloc(1, sizeof(*svc->telemetry));
    if (svc->telemetry == NULL)
    {
        set_error(svc, "telemetry: out of memory");
        return false;
    }

    const char* err = NULL;
    if (!e2e_amqp_telemetry_begin(
            svc->telemetry, svc->eh_info.host, svc->entity, sas, svc->partition_count, &err))
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
    if (svc->telemetry == NULL)
    {
        return true;
    }
    if (!e2e_amqp_telemetry_pump(svc->telemetry, timeout_ms))
    {
        set_error(svc, "telemetry: connection failed");
        return false;
    }
    return true;
}

bool az_iot_e2e_service_telemetry_seen(const az_iot_e2e_service* svc, const char* needle)
{
    return svc->telemetry != NULL && e2e_amqp_telemetry_seen(svc->telemetry, needle);
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

bool az_iot_e2e_service_send_c2d(
    az_iot_e2e_service* svc,
    const char* device_id,
    const uint8_t* payload,
    size_t payload_len)
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
    if (!e2e_amqp_send_c2d(svc->hub_info.host, sas, device_id, payload, payload_len, &err))
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

/* Start a REST request, replacing any previous one. */
static bool begin_request(
    az_iot_e2e_service* svc,
    const char* method,
    const char* path,
    const char* sas,
    const char* body)
{
    if (svc->http != NULL)
    {
        e2e_http_end(svc->http);
        free(svc->http);
        svc->http = NULL;
    }
    svc->http = (e2e_http_request_t*)calloc(1, sizeof(*svc->http));
    if (svc->http == NULL)
    {
        set_error(svc, "rest: out of memory");
        return false;
    }
    if (!e2e_http_begin(svc->http, svc->hub_info.host, method, path, sas, body))
    {
        set_error(svc, e2e_http_error(svc->http));
        free(svc->http);
        svc->http = NULL;
        return false;
    }
    return true;
}

bool az_iot_e2e_service_method_invoke_begin(
    az_iot_e2e_service* svc,
    const char* device_id,
    const char* method_name,
    const char* json_payload)
{
    char sas[512];
    if (!build_hub_sas(svc, sas, sizeof(sas)))
    {
        return false;
    }
    char path[256];
    snprintf(
        path, sizeof(path), "/twins/%s/methods?api-version=%s", device_id, E2E_API_VERSION);
    char body[1024];
    snprintf(
        body,
        sizeof(body),
        "{\"methodName\":\"%s\",\"responseTimeoutInSeconds\":30,"
        "\"connectTimeoutInSeconds\":30,\"payload\":%s}",
        method_name,
        (json_payload != NULL) ? json_payload : "null");
    return begin_request(svc, "POST", path, sas, body);
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
    char body[1024];
    snprintf(
        body,
        sizeof(body),
        "{\"properties\":{\"desired\":%s}}",
        (desired_json != NULL) ? desired_json : "{}");
    return begin_request(svc, "PATCH", path, sas, body);
}

int az_iot_e2e_service_request_poll(
    az_iot_e2e_service* svc,
    int* out_http_status,
    char* resp_buf,
    size_t resp_buf_size)
{
    if (svc->http == NULL)
    {
        set_error(svc, "rest: no request in flight");
        return -1;
    }

    int rc = e2e_http_poll(svc->http);
    if (rc == 0)
    {
        return 0;
    }

    if (rc == 1)
    {
        if (out_http_status != NULL)
        {
            *out_http_status = e2e_http_status(svc->http);
        }
        if (resp_buf != NULL && resp_buf_size > 0)
        {
            int body_len = 0;
            const uint8_t* body = e2e_http_body(svc->http, &body_len);
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
        set_error(svc, e2e_http_error(svc->http));
    }

    e2e_http_end(svc->http);
    free(svc->http);
    svc->http = NULL;
    return rc;
}
