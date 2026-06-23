// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/* SPDX-License-Identifier: MIT */
/* FileUploadClient — Classic IoT Hub only.
 *
 * Implements the IoT Hub file upload notification protocol over MQTT:
 *
 * 1. Device publishes to "$iothub/device/files/notifications" with a JSON body
 *    requesting a SAS URI: {"blobName":"<name>"}
 *    The request carries $rid in the topic for correlation.
 *
 * 2. Hub responds on "$iothub/device/files/notifications/res/<status>/?$rid=<n>"
 *    with a JSON body containing: correlationId, hostName, containerName,
 *    blobName, sasToken — from which the full SAS URI is assembled.
 *
 * 3. Application performs HTTP PUT to the SAS URI (out of band — not this SDK).
 *
 * 4. Device publishes completion notification to
 *    "$iothub/device/files/notifications" with JSON body:
 *    {"correlationId":"<id>","isSuccess":<bool>}
 *    Also correlated by $rid.
 *
 * 5. Hub responds on the same response topic with status for the notification.
 *
 * This feature is NOT supported on IoT/AEG Hub (MQTT v5). Attempting to
 * initialize on a Next-flavor connection returns AZ_IOT_ERR_NOT_SUPPORTED.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "azure/iot/az_iot_file_upload_client.h"
#include "azure/iot/az_iot_mqtt_iface.h"

#include "internal/connection_client_internal.h"
#include "internal/protocol_profile.h"

#define AZ_IOT_FU_TOPIC_MAX      256
#define AZ_IOT_FU_PAYLOAD_MAX    512
#define AZ_IOT_FU_SAS_URI_MAX   2048
#define AZ_IOT_FU_CORR_ID_MAX    128

/* Topic patterns (Classic IoT Hub) */
static const char k_fu_request_topic_prefix[] = "$iothub/device/files/notifications";
static const char k_fu_response_topic_prefix[] = "$iothub/device/files/notifications/res/";

/* Pending slot kinds */
#define FU_PENDING_NONE     0
#define FU_PENDING_SAS_URI  1
#define FU_PENDING_NOTIFY   2

/* Internal shorthand */
#define FI(c) ((c)->_internal)

/* ------------------------------------------------------------------------- */
/* helpers                                                                   */
/* ------------------------------------------------------------------------- */

static int find_pending_by_rid(az_iot_file_upload_client_t* c, uint32_t rid)
{
    for (int i = 0; i < AZ_IOT_FILE_UPLOAD_MAX_PENDING; ++i)
    {
        if (FI(c).pending[i].in_use && FI(c).pending[i].rid == rid)
            return i;
    }
    return -1;
}

static int alloc_pending(az_iot_file_upload_client_t* c)
{
    for (int i = 0; i < AZ_IOT_FILE_UPLOAD_MAX_PENDING; ++i)
    {
        if (!FI(c).pending[i].in_use) return i;
    }
    return -1;
}

/* Minimal JSON string value extractor. Finds "key":"value" and copies value
 * (unescaped) into out. Returns true on success. */
static bool json_get_string(const char* json, size_t json_len,
                            const char* key, char* out, size_t out_cap)
{
    /* Build the search pattern: "key":" */
    char pattern[64];
    int pn = snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    if (pn < 0 || (size_t)pn >= sizeof(pattern)) return false;

    const char* found = NULL;
    const char* end = json + json_len;
    for (const char* p = json; p < end - (size_t)pn; ++p)
    {
        if (memcmp(p, pattern, (size_t)pn) == 0)
        {
            found = p + pn;
            break;
        }
    }
    if (!found) return false;

    /* Extract until closing unescaped '"' */
    size_t i = 0;
    for (const char* p = found; p < end && *p != '"'; ++p)
    {
        if (i + 1 >= out_cap) return false;
        out[i++] = *p;
    }
    out[i] = '\0';
    return i > 0;
}

/* Assemble the full SAS URI from response JSON fields:
 * https://{hostName}/{containerName}/{blobName}{sasToken} */
static bool assemble_sas_uri(const char* json, size_t json_len,
                             char* out, size_t out_cap)
{
    char host[256] = {0};
    char container[256] = {0};
    char blob[256] = {0};
    char sas[1024] = {0};

    if (!json_get_string(json, json_len, "hostName", host, sizeof(host))) return false;
    if (!json_get_string(json, json_len, "containerName", container, sizeof(container))) return false;
    if (!json_get_string(json, json_len, "blobName", blob, sizeof(blob))) return false;
    if (!json_get_string(json, json_len, "sasToken", sas, sizeof(sas))) return false;

    int n = snprintf(out, out_cap, "https://%s/%s/%s%s", host, container, blob, sas);
    return (n > 0 && (size_t)n < out_cap);
}

/* Extract $rid=<value> from query string portion of topic. */
static bool extract_rid(const char* topic, uint32_t* out_rid)
{
    const char* qmark = strchr(topic, '?');
    if (!qmark) return false;
    const char* key = "$rid=";
    size_t key_len = 5;
    const char* p = qmark;
    while (p && *p)
    {
        if (*p == '?' || *p == '&') p++;
        if (strncmp(p, key, key_len) == 0)
        {
            *out_rid = (uint32_t)strtoul(p + key_len, NULL, 10);
            return true;
        }
        p = strchr(p, '&');
    }
    return false;
}

/* Extract HTTP-style status code from response topic:
 * "$iothub/device/files/notifications/res/<status>/?$rid=..." */
static int extract_status(const char* topic)
{
    size_t prefix_len = sizeof(k_fu_response_topic_prefix) - 1;
    if (strncmp(topic, k_fu_response_topic_prefix, prefix_len) != 0) return -1;
    const char* status_str = topic + prefix_len;
    int status = 0;
    for (const char* p = status_str; *p && *p != '/' && *p != '?'; ++p)
    {
        if (*p < '0' || *p > '9') return -1;
        status = status * 10 + (*p - '0');
    }
    return status;
}

static az_iot_result_t status_to_result(int status)
{
    if (status >= 200 && status < 300) return AZ_IOT_OK;
    if (status == 404) return AZ_IOT_ERR_INVALID_ARG;
    if (status == 429) return AZ_IOT_ERR_NOT_SUPPORTED;
    return AZ_IOT_ERR_MQTT;
}

/* ------------------------------------------------------------------------- */
/* inbound dispatch handler                                                  */
/* ------------------------------------------------------------------------- */

static void on_file_upload_response(void* user_ctx, const az_iot_mqtt_message_t* msg)
{
    az_iot_file_upload_client_t* fu = (az_iot_file_upload_client_t*)user_ctx;
    if (!fu || !msg || !msg->topic) return;

    uint32_t rid = 0;
    if (!extract_rid(msg->topic, &rid)) return;

    int idx = find_pending_by_rid(fu, rid);
    if (idx < 0) return;

    int status = extract_status(msg->topic);
    az_iot_result_t r = status_to_result(status);

    if (FI(fu).pending[idx].kind == FU_PENDING_SAS_URI)
    {
        az_iot_file_upload_sas_cb cb = FI(fu).pending[idx].cb.sas_cb;
        void* ctx = FI(fu).pending[idx].user_ctx;
        FI(fu).pending[idx].in_use = false;
        FI(fu).pending[idx].kind = FU_PENDING_NONE;

        if (cb)
        {
            if (r == AZ_IOT_OK && msg->payload && msg->payload_len > 0)
            {
                /* Parse the SAS URI and correlation ID from the response JSON */
                char sas_uri[AZ_IOT_FU_SAS_URI_MAX];
                char corr_id[AZ_IOT_FU_CORR_ID_MAX];

                bool have_uri = assemble_sas_uri(
                    (const char*)msg->payload, msg->payload_len,
                    sas_uri, sizeof(sas_uri));
                bool have_corr = json_get_string(
                    (const char*)msg->payload, msg->payload_len,
                    "correlationId", corr_id, sizeof(corr_id));

                if (have_uri && have_corr)
                {
                    cb(AZ_IOT_OK, sas_uri, corr_id, ctx);
                }
                else
                {
                    cb(AZ_IOT_ERR_PROTOCOL, NULL, NULL, ctx);
                }
            }
            else
            {
                cb(r, NULL, NULL, ctx);
            }
        }
    }
    else if (FI(fu).pending[idx].kind == FU_PENDING_NOTIFY)
    {
        az_iot_file_upload_complete_cb cb = FI(fu).pending[idx].cb.complete_cb;
        void* ctx = FI(fu).pending[idx].user_ctx;
        FI(fu).pending[idx].in_use = false;
        FI(fu).pending[idx].kind = FU_PENDING_NONE;

        if (cb) cb(r, ctx);
    }
    else
    {
        FI(fu).pending[idx].in_use = false;
    }
}

/* ------------------------------------------------------------------------- */
/* public API                                                                */
/* ------------------------------------------------------------------------- */

az_iot_result_t az_iot_file_upload_client_init(
    az_iot_file_upload_client_t* client,
    az_iot_connection_client_t* conn)
{
    if (!client || !conn) return AZ_IOT_ERR_INVALID_ARG;

    const az_iot_protocol_profile_t* profile = az_iot_connection_client__profile(conn);

    /* File upload is Classic-only */
    if (profile && profile->flavor == AZ_IOT_HUB_FLAVOR_NEXT)
    {
        return AZ_IOT_ERR_NOT_SUPPORTED;
    }

    memset(client, 0, sizeof(*client));
    FI(client).conn = conn;
    FI(client).next_rid = 1;

    /* Register inbound handler for file upload response topic */
    az_iot_result_t r = az_iot_connection_client__register_inbound_handler(
        conn, k_fu_response_topic_prefix, on_file_upload_response, client);
    if (r != AZ_IOT_OK)
    {
        memset(client, 0, sizeof(*client));
        return r;
    }

    /* Persistent subscription: "$iothub/device/files/notifications/res/#" */
    char filter[AZ_IOT_FU_TOPIC_MAX];
    int n = snprintf(filter, sizeof(filter), "%s#", k_fu_response_topic_prefix);
    if (n < 0 || (size_t)n >= sizeof(filter))
    {
        (void)az_iot_connection_client__unregister_inbound_handlers(conn, client);
        memset(client, 0, sizeof(*client));
        return AZ_IOT_ERR_INTERNAL;
    }

    r = az_iot_connection_client__add_subscription_on_connect(conn, filter, AZ_IOT_MQTT_QOS_1);
    if (r != AZ_IOT_OK)
    {
        (void)az_iot_connection_client__unregister_inbound_handlers(conn, client);
        memset(client, 0, sizeof(*client));
        return r;
    }

    return AZ_IOT_OK;
}

void az_iot_file_upload_client_deinit(az_iot_file_upload_client_t* client)
{
    if (!client) return;
    (void)az_iot_connection_client__unregister_inbound_handlers(FI(client).conn, client);
    memset(client, 0, sizeof(*client));
}

az_iot_result_t az_iot_file_upload_client_get_sas_uri(
    az_iot_file_upload_client_t* client,
    const char* blob_name,
    az_iot_file_upload_sas_cb cb,
    void* user_ctx)
{
    if (!client || !blob_name || !cb) return AZ_IOT_ERR_INVALID_ARG;

    int idx = alloc_pending(client);
    if (idx < 0) return AZ_IOT_ERR_NOT_SUPPORTED;

    uint32_t rid = FI(client).next_rid++;
    if (FI(client).next_rid == 0) FI(client).next_rid = 1;

    /* Build topic: "$iothub/device/files/notifications/?$rid=<n>" */
    char topic[AZ_IOT_FU_TOPIC_MAX];
    int n = snprintf(topic, sizeof(topic), "%s/?$rid=%u",
                     k_fu_request_topic_prefix, (unsigned)rid);
    if (n < 0 || (size_t)n >= sizeof(topic)) return AZ_IOT_ERR_INTERNAL;

    /* Build payload: {"blobName":"<name>"} */
    char payload[AZ_IOT_FU_PAYLOAD_MAX];
    n = snprintf(payload, sizeof(payload), "{\"blobName\":\"%s\"}", blob_name);
    if (n < 0 || (size_t)n >= sizeof(payload)) return AZ_IOT_ERR_INVALID_ARG;

    /* Reserve pending slot */
    FI(client).pending[idx].in_use = true;
    FI(client).pending[idx].rid = rid;
    FI(client).pending[idx].kind = FU_PENDING_SAS_URI;
    FI(client).pending[idx].cb.sas_cb = cb;
    FI(client).pending[idx].user_ctx = user_ctx;

    az_iot_mqtt_message_t out = {0};
    out.topic = topic;
    out.payload = (const uint8_t*)payload;
    out.payload_len = (size_t)strlen(payload);
    out.qos = AZ_IOT_MQTT_QOS_1;

    az_iot_result_t r = az_iot_connection_client__publish(
        FI(client).conn, &out, NULL, NULL);
    if (r != AZ_IOT_OK)
    {
        FI(client).pending[idx].in_use = false;
        FI(client).pending[idx].kind = FU_PENDING_NONE;
    }
    return r;
}

az_iot_result_t az_iot_file_upload_client_notify_complete(
    az_iot_file_upload_client_t* client,
    const char* correlation_id,
    bool is_success,
    az_iot_file_upload_complete_cb cb,
    void* user_ctx)
{
    if (!client || !correlation_id || !cb) return AZ_IOT_ERR_INVALID_ARG;

    int idx = alloc_pending(client);
    if (idx < 0) return AZ_IOT_ERR_NOT_SUPPORTED;

    uint32_t rid = FI(client).next_rid++;
    if (FI(client).next_rid == 0) FI(client).next_rid = 1;

    /* Build topic: "$iothub/device/files/notifications/?$rid=<n>" */
    char topic[AZ_IOT_FU_TOPIC_MAX];
    int n = snprintf(topic, sizeof(topic), "%s/?$rid=%u",
                     k_fu_request_topic_prefix, (unsigned)rid);
    if (n < 0 || (size_t)n >= sizeof(topic)) return AZ_IOT_ERR_INTERNAL;

    /* Build payload: {"correlationId":"<id>","isSuccess":<bool>} */
    char payload[AZ_IOT_FU_PAYLOAD_MAX];
    n = snprintf(payload, sizeof(payload),
                 "{\"correlationId\":\"%s\",\"isSuccess\":%s}",
                 correlation_id, is_success ? "true" : "false");
    if (n < 0 || (size_t)n >= sizeof(payload)) return AZ_IOT_ERR_INVALID_ARG;

    /* Reserve pending slot */
    FI(client).pending[idx].in_use = true;
    FI(client).pending[idx].rid = rid;
    FI(client).pending[idx].kind = FU_PENDING_NOTIFY;
    FI(client).pending[idx].cb.complete_cb = cb;
    FI(client).pending[idx].user_ctx = user_ctx;

    az_iot_mqtt_message_t out = {0};
    out.topic = topic;
    out.payload = (const uint8_t*)payload;
    out.payload_len = (size_t)strlen(payload);
    out.qos = AZ_IOT_MQTT_QOS_1;

    az_iot_result_t r = az_iot_connection_client__publish(
        FI(client).conn, &out, NULL, NULL);
    if (r != AZ_IOT_OK)
    {
        FI(client).pending[idx].in_use = false;
        FI(client).pending[idx].kind = FU_PENDING_NONE;
    }
    return r;
}
