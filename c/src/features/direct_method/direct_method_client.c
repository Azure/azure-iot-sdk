// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/* SPDX-License-Identifier: MIT */
/* DirectMethodClient (Phase 3.2).
 *
 * Supports both IoT Hub Classic (MQTT v3.1.1) and Hub-Next (MQTT v5).
 *
 * Classic:
 *   Subscribe  "$iothub/methods/POST/#"
 *   Inbound    "$iothub/methods/POST/{methodName}/?$rid={rid}"
 *   Respond    "$iothub/methods/res/{status}/?$rid={rid}"
 *
 * Next:
 *   Subscribe  "ih/{device_id}/dev/methods/+"
 *   Inbound    "ih/{device_id}/dev/methods/{methodName}" + correlation_data
 *   Respond    "ih/{device_id}/srv/methods/{methodName}/response" + correlation_data
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "azure/iot/az_iot_direct_method_client.h"
#include "azure/iot/az_iot_mqtt_iface.h"

#include "internal/connection_client_internal.h"
#include "internal/protocol_profile.h"

#define AZ_IOT_DM_METHOD_NAME_MAX 96
#define AZ_IOT_DM_RID_MAX         32
#define AZ_IOT_DM_RESP_TOPIC_MAX  192
#define AZ_IOT_DM_CORR_DATA_MAX   64

/* Internal shorthand */
#define DI(d) ((d)->_internal)

struct az_iot_direct_method_request_tag
{
    az_iot_direct_method_client_t* owner;
    /* Classic: request-id from topic query string */
    char rid[AZ_IOT_DM_RID_MAX];
    /* Next: method name + correlation data (used in response topic/property) */
    char method_name[AZ_IOT_DM_METHOD_NAME_MAX];
    uint8_t correlation_data[AZ_IOT_DM_CORR_DATA_MAX];
    size_t correlation_data_len;
    bool is_next; /* true = Hub-Next path */
};

/* Parse "$iothub/methods/POST/<methodName>/?$rid=<rid>" into out_method and
 * out_rid (NUL-terminated). Returns false on malformed input. */
static bool parse_method_topic_classic(
    const char* topic,
    char* out_method, size_t method_cap,
    char* out_rid,    size_t rid_cap)
{
    static const char k_prefix[] = "$iothub/methods/POST/";
    size_t prefix_len = sizeof(k_prefix) - 1;
    if (strncmp(topic, k_prefix, prefix_len) != 0) return false;
    const char* name = topic + prefix_len;
    /* methodName is up to the next '/'. */
    const char* slash = strchr(name, '/');
    if (!slash || slash == name) return false;
    size_t name_len = (size_t)(slash - name);
    if (name_len + 1 > method_cap) return false;
    memcpy(out_method, name, name_len);
    out_method[name_len] = '\0';

    /* After the slash we expect "?$rid=<value>" (the value runs to end). */
    static const char k_rid_marker[] = "?$rid=";
    const char* rid_marker = strstr(slash, k_rid_marker);
    if (!rid_marker) return false;
    const char* rid_val = rid_marker + (sizeof(k_rid_marker) - 1);
    size_t rid_len = strlen(rid_val);
    if (rid_len == 0 || rid_len + 1 > rid_cap) return false;
    memcpy(out_rid, rid_val, rid_len);
    out_rid[rid_len] = '\0';
    return true;
}

/* Parse Hub-Next topic "ih/{device_id}/dev/methods/{methodName}".
 * Returns false on malformed input. */
static bool parse_method_topic_next(
    const char* topic,
    char* out_method, size_t method_cap)
{
    /* Expected: "ih/<id>/dev/methods/<name>" */
    static const char k_prefix[] = "ih/";
    if (strncmp(topic, k_prefix, 3) != 0) return false;
    /* Find "/dev/methods/" after device_id */
    const char* dev_methods = strstr(topic + 3, "/dev/methods/");
    if (!dev_methods) return false;
    const char* name = dev_methods + 13; /* strlen("/dev/methods/") */
    size_t name_len = strlen(name);
    /* Strip trailing '/' if present */
    while (name_len > 0 && name[name_len - 1] == '/')
        name_len--;
    if (name_len == 0 || name_len + 1 > method_cap) return false;
    memcpy(out_method, name, name_len);
    out_method[name_len] = '\0';
    return true;
}

/* Inbound dispatch handler for Classic topics. */
static void on_method_invocation_classic(void* user_ctx, const az_iot_mqtt_message_t* msg)
{
    az_iot_direct_method_client_t* dm = (az_iot_direct_method_client_t*)user_ctx;
    if (!dm || !msg || !msg->topic) return;
    if (!DI(dm).handler) return;

    char method_name[AZ_IOT_DM_METHOD_NAME_MAX];
    char rid[AZ_IOT_DM_RID_MAX];
    if (!parse_method_topic_classic(msg->topic, method_name, sizeof(method_name), rid, sizeof(rid)))
    {
        return;
    }

    az_iot_direct_method_request_t* req =
        (az_iot_direct_method_request_t*)calloc(1, sizeof(*req));
    if (!req) return;
    req->owner = dm;
    req->is_next = false;
    memcpy(req->rid, rid, strlen(rid) + 1);

    DI(dm).handler(req, method_name, msg->payload, msg->payload_len, DI(dm).handler_ctx);
}

/* Inbound dispatch handler for Hub-Next topics. */
static void on_method_invocation_next(void* user_ctx, const az_iot_mqtt_message_t* msg)
{
    az_iot_direct_method_client_t* dm = (az_iot_direct_method_client_t*)user_ctx;
    if (!dm || !msg || !msg->topic) return;
    if (!DI(dm).handler) return;

    char method_name[AZ_IOT_DM_METHOD_NAME_MAX];
    if (!parse_method_topic_next(msg->topic, method_name, sizeof(method_name)))
    {
        return;
    }

    az_iot_direct_method_request_t* req =
        (az_iot_direct_method_request_t*)calloc(1, sizeof(*req));
    if (!req) return;
    req->owner = dm;
    req->is_next = true;
    memcpy(req->method_name, method_name, strlen(method_name) + 1);

    /* Copy correlation data from inbound message if present */
    if (msg->correlation_data && msg->correlation_data_len > 0)
    {
        size_t copy_len = msg->correlation_data_len;
        if (copy_len > AZ_IOT_DM_CORR_DATA_MAX)
            copy_len = AZ_IOT_DM_CORR_DATA_MAX;
        memcpy(req->correlation_data, msg->correlation_data, copy_len);
        req->correlation_data_len = copy_len;
    }

    DI(dm).handler(req, method_name, msg->payload, msg->payload_len, DI(dm).handler_ctx);
}

az_iot_result_t az_iot_direct_method_client_init(
    az_iot_direct_method_client_t* client,
    az_iot_connection_client_t* conn)
{
    if (client == NULL || conn == NULL)
    {
        return AZ_IOT_ERR_INVALID_ARG;
    }
    const az_iot_protocol_profile_t* profile =
        az_iot_connection_client__profile(conn);
    if (!profile)
    {
        return AZ_IOT_ERR_NOT_SUPPORTED;
    }

    memset(client, 0, sizeof(*client));
    DI(client).conn = conn;

    if (profile->flavor == AZ_IOT_HUB_FLAVOR_NEXT)
    {
        /* Hub-Next: subscribe to "ih/{device_id}/dev/methods/+" */
        const char* device_id = az_iot_connection_client__device_id(conn);
        if (!device_id)
        {
            memset(client, 0, sizeof(*client));
            return AZ_IOT_ERR_NOT_INITIALIZED;
        }

        /* Build topic prefix for inbound dispatch: "ih/{device_id}/dev/methods/" */
        char prefix[AZ_IOT_DM_RESP_TOPIC_MAX];
        int n = snprintf(prefix, sizeof(prefix), "ih/%s/dev/methods/", device_id);
        if (n < 0 || (size_t)n >= sizeof(prefix))
        {
            memset(client, 0, sizeof(*client));
            return AZ_IOT_ERR_INTERNAL;
        }

        az_iot_result_t r = az_iot_connection_client__register_inbound_handler(
            conn, prefix, on_method_invocation_next, client);
        if (r != AZ_IOT_OK)
        {
            memset(client, 0, sizeof(*client));
            return r;
        }

        /* Build wildcard subscription: "ih/{device_id}/dev/methods/+" */
        char filter[AZ_IOT_DM_RESP_TOPIC_MAX];
        n = snprintf(filter, sizeof(filter), "ih/%s/dev/methods/+", device_id);
        if (n < 0 || (size_t)n >= sizeof(filter))
        {
            (void)az_iot_connection_client__unregister_inbound_handlers(conn, client);
            memset(client, 0, sizeof(*client));
            return AZ_IOT_ERR_INTERNAL;
        }
        r = az_iot_connection_client__add_subscription_on_connect(
            conn, filter, AZ_IOT_MQTT_QOS_1);
        if (r != AZ_IOT_OK)
        {
            (void)az_iot_connection_client__unregister_inbound_handlers(conn, client);
            memset(client, 0, sizeof(*client));
            return r;
        }
    }
    else
    {
        /* Classic: subscribe to "$iothub/methods/POST/#" */
        if (!profile->methods_request_topic_prefix)
        {
            memset(client, 0, sizeof(*client));
            return AZ_IOT_ERR_NOT_SUPPORTED;
        }

        az_iot_result_t r = az_iot_connection_client__register_inbound_handler(
            conn, profile->methods_request_topic_prefix, on_method_invocation_classic, client);
        if (r != AZ_IOT_OK)
        {
            memset(client, 0, sizeof(*client));
            return r;
        }

        char filter[AZ_IOT_DM_RESP_TOPIC_MAX];
        int n = snprintf(filter, sizeof(filter), "%s#", profile->methods_request_topic_prefix);
        if (n < 0 || (size_t)n >= sizeof(filter))
        {
            (void)az_iot_connection_client__unregister_inbound_handlers(conn, client);
            memset(client, 0, sizeof(*client));
            return AZ_IOT_ERR_INTERNAL;
        }
        r = az_iot_connection_client__add_subscription_on_connect(
            conn, filter, AZ_IOT_MQTT_QOS_0);
        if (r != AZ_IOT_OK)
        {
            (void)az_iot_connection_client__unregister_inbound_handlers(conn, client);
            memset(client, 0, sizeof(*client));
            return r;
        }
    }

    return AZ_IOT_OK;
}

void az_iot_direct_method_client_deinit(az_iot_direct_method_client_t* client)
{
    if (!client) return;
    (void)az_iot_connection_client__unregister_inbound_handlers(DI(client).conn, client);
    memset(client, 0, sizeof(*client));
}

az_iot_result_t az_iot_direct_method_client_set_handler(
    az_iot_direct_method_client_t* dm,
    az_iot_direct_method_handler_cb cb,
    void* user_ctx)
{
    if (dm == NULL)
    {
        return AZ_IOT_ERR_INVALID_ARG;
    }
    DI(dm).handler = cb;
    DI(dm).handler_ctx = user_ctx;
    return AZ_IOT_OK;
}

az_iot_result_t az_iot_direct_method_respond(
    az_iot_direct_method_request_t* request,
    int status_code,
    const uint8_t* payload,
    size_t payload_len)
{
    if (request == NULL) return AZ_IOT_ERR_INVALID_ARG;
    if (payload_len > 0 && payload == NULL)
    {
        return AZ_IOT_ERR_INVALID_ARG;
    }

    az_iot_direct_method_client_t* dm = request->owner;

    if (request->is_next)
    {
        /* Hub-Next: respond on "ih/{device_id}/srv/methods/{methodName}/response" */
        const char* device_id = az_iot_connection_client__device_id(DI(dm).conn);
        char topic[AZ_IOT_DM_RESP_TOPIC_MAX];
        int n = snprintf(topic, sizeof(topic), "ih/%s/srv/methods/%s/response",
                         device_id, request->method_name);
        if (n < 0 || (size_t)n >= sizeof(topic))
        {
            free(request);
            return AZ_IOT_ERR_NOT_SUPPORTED;
        }

        /* Build status property as user property */
        char status_str[12];
        snprintf(status_str, sizeof(status_str), "%d", status_code);

        az_iot_mqtt_user_property_t user_props[1];
        user_props[0].key = "status";
        user_props[0].value = status_str;

        az_iot_mqtt_message_t out = { 0 };
        out.topic = topic;
        out.payload = payload;
        out.payload_len = payload_len;
        out.qos = AZ_IOT_MQTT_QOS_1;
        out.retain = false;
        out.user_properties = user_props;
        out.user_properties_count = 1;
        out.correlation_data = request->correlation_data;
        out.correlation_data_len = request->correlation_data_len;

        az_iot_result_t r = az_iot_connection_client__publish(
            DI(dm).conn, &out, NULL, NULL);
        free(request);
        return r;
    }
    else
    {
        /* Classic: "$iothub/methods/res/{status}/?$rid={rid}" */
        char topic[AZ_IOT_DM_RESP_TOPIC_MAX];
        int n = snprintf(topic, sizeof(topic), "$iothub/methods/res/%d/?$rid=%s",
                         status_code, request->rid);
        if (n < 0 || (size_t)n >= sizeof(topic))
        {
            free(request);
            return AZ_IOT_ERR_NOT_SUPPORTED;
        }

        az_iot_mqtt_message_t out = { 0 };
        out.topic       = topic;
        out.payload     = payload;
        out.payload_len = payload_len;
        out.qos         = AZ_IOT_MQTT_QOS_0;
        out.retain      = false;

        az_iot_result_t r = az_iot_connection_client__publish(
            DI(dm).conn, &out, NULL, NULL);
        free(request);
        return r;
    }
}
