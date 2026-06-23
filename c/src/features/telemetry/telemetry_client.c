// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/* SPDX-License-Identifier: MIT */
/* TelemetryClient (Phase 3.1).
 *
 * Builds the device-to-cloud (D2C) publish topic from the active protocol
 * profile + device id, attaches optional system and application properties as
 * a property bag, and publishes through the ConnectionClient. The send
 * completion callback fires from inside do_work(): synchronously for QoS 0,
 * after PUBACK for QoS 1.
 *
 * NOTE on encoding: per the Azure IoT Hub Classic MQTT topic spec, property
 * bag entries must be URL-encoded. Phase 3.1 forwards keys/values verbatim;
 * callers are responsible for pre-encoding values containing '%', '&', '=',
 * '?', or non-ASCII bytes. Full URL-encoding is tracked as a follow-up.
 */
#include <stdbool.h>
#include <string.h>

#include "azure/iot/az_iot_telemetry_client.h"
#include "azure/iot/az_iot_mqtt_iface.h"

#include "internal/connection_client_internal.h"
#include "internal/log_internal.h"
#include "internal/protocol_profile.h"

#define AZ_IOT_TELEMETRY_TOPIC_MAX 512

/* Max user properties on a single Next-mode telemetry publish (type + content-type +
 * forwarded application properties). Generous for telemetry use cases. */
#define AZ_IOT_TELEMETRY_MAX_USER_PROPS 16

az_iot_result_t az_iot_telemetry_client_init(
    az_iot_telemetry_client_t* client,
    az_iot_connection_client_t* conn)
{
    if (client == NULL || conn == NULL)
    {
        AZ_IOT_LOG_ERROR("telemetry_client_init: invalid arguments");
        return AZ_IOT_ERR_INVALID_ARG;
    }
    memset(client, 0, sizeof(*client));
    client->_internal.conn = conn;
    return AZ_IOT_OK;
}

void az_iot_telemetry_client_deinit(az_iot_telemetry_client_t* client)
{
    if (client == NULL) return;
    memset(client, 0, sizeof(*client));
}

/* Append a NUL-terminated string to dst at *off, advancing *off. Returns
 * false on overflow without modifying dst beyond *off. */
static bool append_str(char* dst, size_t cap, size_t* off, const char* src)
{
    size_t n = strlen(src);
    if (*off + n + 1 > cap)
    {
        return false; /* +1 leaves room for the trailing NUL */
    }
    memcpy(dst + *off, src, n);
    *off += n;
    dst[*off] = '\0';
    return true;
}

/* Build "devices/<device_id>/messages/events/" + optional "<bag>" property
 * string into out_topic. Classic (MQTT v3.1.1) path only. */
static az_iot_result_t build_topic_classic(
    const az_iot_protocol_profile_t* profile,
    const char* device_id,
    const az_iot_telemetry_message_t* msg,
    char* out_topic, size_t cap)
{
    /* The profile's d2c_publish_topic_template is the canonical reference
     * ("devices/{device_id}/messages/events/"). Phase 3.1 expands the single
     * {device_id} substring inline rather than carrying a templating helper. */
    const char* tmpl = profile->d2c_publish_topic_template;
    if (tmpl == NULL)
    {
        return AZ_IOT_ERR_NOT_SUPPORTED;
    }
    static const char k_placeholder[] = "{device_id}";
    const char* placeholder = strstr(tmpl, k_placeholder);
    if (placeholder == NULL)
    {
        return AZ_IOT_ERR_INTERNAL;
    }

    size_t off = 0;
    size_t prefix_len = (size_t)(placeholder - tmpl);
    if (prefix_len + 1 > cap)
    {
        return AZ_IOT_ERR_NOT_SUPPORTED;
    }
    memcpy(out_topic, tmpl, prefix_len);
    off = prefix_len;
    out_topic[off] = '\0';

    if (!append_str(out_topic, cap, &off, device_id))
    {
        return AZ_IOT_ERR_NOT_SUPPORTED;
    }
    if (!append_str(out_topic, cap, &off, placeholder + (sizeof(k_placeholder) - 1)))
    {
        return AZ_IOT_ERR_NOT_SUPPORTED;
    }

    /* Property bag: all properties (system and application) are serialized
     * uniformly as key=value pairs separated by '&'. System properties use
     * well-known keys like "$.ct", "$.ce", etc. */
    bool first = true;
    for (size_t i = 0; i < msg->properties_count; ++i)
    {
        const az_iot_telemetry_property_t* p = &msg->properties[i];
        if (p->key == NULL || p->key[0] == '\0')
        {
            continue;
        }
        if (!first && !append_str(out_topic, cap, &off, "&"))
        {
            return AZ_IOT_ERR_NOT_SUPPORTED;
        }
        if (!append_str(out_topic, cap, &off, p->key))
        {
            return AZ_IOT_ERR_NOT_SUPPORTED;
        }
        if (p->value != NULL)
        {
            if (!append_str(out_topic, cap, &off, "="))
            {
                return AZ_IOT_ERR_NOT_SUPPORTED;
            }
            if (!append_str(out_topic, cap, &off, p->value))
            {
                return AZ_IOT_ERR_NOT_SUPPORTED;
            }
        }
        first = false;
    }
    return AZ_IOT_OK;
}

/* Build the flat topic "ih/<device_id>/srv/telemetry" for Next (MQTT v5). */
static az_iot_result_t build_topic_next(
    const az_iot_protocol_profile_t* profile,
    const char* device_id,
    char* out_topic, size_t cap)
{
    const char* tmpl = profile->next_topic_base_template;
    if (tmpl == NULL)
    {
        return AZ_IOT_ERR_NOT_SUPPORTED;
    }
    static const char k_placeholder[] = "{device_id}";
    static const char k_suffix[] = "/srv/telemetry";
    const char* placeholder = strstr(tmpl, k_placeholder);
    if (placeholder == NULL)
    {
        return AZ_IOT_ERR_INTERNAL;
    }

    size_t off = 0;
    size_t prefix_len = (size_t)(placeholder - tmpl);
    if (prefix_len + 1 > cap)
    {
        return AZ_IOT_ERR_NOT_SUPPORTED;
    }
    memcpy(out_topic, tmpl, prefix_len);
    off = prefix_len;
    out_topic[off] = '\0';

    if (!append_str(out_topic, cap, &off, device_id))
    {
        return AZ_IOT_ERR_NOT_SUPPORTED;
    }
    /* Append remainder of template after placeholder */
    if (!append_str(out_topic, cap, &off, placeholder + (sizeof(k_placeholder) - 1)))
    {
        return AZ_IOT_ERR_NOT_SUPPORTED;
    }
    /* Append /srv/telemetry */
    if (!append_str(out_topic, cap, &off, k_suffix))
    {
        return AZ_IOT_ERR_NOT_SUPPORTED;
    }
    return AZ_IOT_OK;
}

/* -----------------------------------------------------------------------
 * telemetry_send_classic — Classic IoT Hub path (MQTT v3.1.1, topic-encoded)
 * ----------------------------------------------------------------------- */
static az_iot_result_t telemetry_send_classic(
    az_iot_telemetry_client_t* client,
    const az_iot_protocol_profile_t* profile,
    const char* device_id,
    const az_iot_telemetry_message_t* msg,
    az_iot_telemetry_send_cb cb,
    void* user_ctx)
{
    char topic[AZ_IOT_TELEMETRY_TOPIC_MAX];
    az_iot_result_t r = build_topic_classic(profile, device_id, msg, topic, sizeof(topic));
    if (r != AZ_IOT_OK)
    {
        return r;
    }

    az_iot_mqtt_message_t out = { 0 };
    out.topic = topic;
    out.payload = msg->payload;
    out.payload_len = msg->payload_len;
    out.qos = AZ_IOT_MQTT_QOS_1;
    out.retain = false;
    return az_iot_connection_client__publish(client->_internal.conn, &out, cb, user_ctx);
}

/* -----------------------------------------------------------------------
 * telemetry_send_next — IoT Hub Next path (MQTT v5, flat topic + User Props)
 * ----------------------------------------------------------------------- */
static az_iot_result_t telemetry_send_next(
    az_iot_telemetry_client_t* client,
    const az_iot_protocol_profile_t* profile,
    const char* device_id,
    const az_iot_telemetry_message_t* msg,
    az_iot_telemetry_send_cb cb,
    void* user_ctx)
{
    char topic[AZ_IOT_TELEMETRY_TOPIC_MAX];
    az_iot_result_t r = build_topic_next(profile, device_id, topic, sizeof(topic));
    if (r != AZ_IOT_OK)
    {
        return r;
    }

    /* MQTT v5 User Properties carry the message type and content-type.
     * Application properties from the caller are also forwarded. */
    az_iot_mqtt_user_property_t user_props[AZ_IOT_TELEMETRY_MAX_USER_PROPS];
    size_t up_count = 0;

    /* Required: type identifier */
    user_props[up_count].key = "type";
    user_props[up_count].value = "telemetry:1";
    up_count++;

    /* Content-type: look for the $.ct system property, or default to json */
    const char* content_type = "application/json";
    for (size_t i = 0; i < msg->properties_count; ++i)
    {
        if (msg->properties[i].key &&
            strcmp(msg->properties[i].key, AZ_IOT_MSG_PROP_CONTENT_TYPE) == 0 &&
            msg->properties[i].value)
        {
            content_type = msg->properties[i].value;
            break;
        }
    }
    user_props[up_count].key = "content-type";
    user_props[up_count].value = content_type;
    up_count++;

    /* Forward remaining application properties as User Properties */
    for (size_t i = 0; i < msg->properties_count && up_count < AZ_IOT_TELEMETRY_MAX_USER_PROPS; ++i)
    {
        const az_iot_telemetry_property_t* p = &msg->properties[i];
        if (p->key == NULL || p->key[0] == '\0') continue;
        /* Skip system properties ($.ct handled above) in user-prop forwarding */
        if (p->key[0] == '$' && p->key[1] == '.') continue;
        user_props[up_count].key = p->key;
        user_props[up_count].value = p->value ? p->value : "";
        up_count++;
    }

    az_iot_mqtt_message_t out = { 0 };
    out.topic = topic;
    out.payload = msg->payload;
    out.payload_len = msg->payload_len;
    out.qos = AZ_IOT_MQTT_QOS_1;
    out.retain = false;
    out.user_properties = user_props;
    out.user_properties_count = up_count;
    out.content_type = content_type;

    return az_iot_connection_client__publish(client->_internal.conn, &out, cb, user_ctx);
}

az_iot_result_t az_iot_telemetry_client_send(
    az_iot_telemetry_client_t* client,
    const az_iot_telemetry_message_t* msg,
    az_iot_telemetry_send_cb cb,
    void* user_ctx)
{
    if (client == NULL || msg == NULL)
    {
        AZ_IOT_LOG_ERROR("telemetry_client_send: invalid arguments");
        return AZ_IOT_ERR_INVALID_ARG;
    }
    if (msg->payload_len > 0 && msg->payload == NULL)
    {
        return AZ_IOT_ERR_INVALID_ARG;
    }

    const az_iot_protocol_profile_t* profile =
        az_iot_connection_client__profile(client->_internal.conn);
    if (profile == NULL)
    {
        return AZ_IOT_ERR_NOT_SUPPORTED;
    }
    const char* device_id = az_iot_connection_client__device_id(client->_internal.conn);
    if (device_id == NULL)
    {
        return AZ_IOT_ERR_NOT_INITIALIZED;
    }

    /* Dispatch to flavor-specific implementation.
     * Classic: metadata encoded in topic path segments.
     * Next: flat topic + MQTT v5 User Properties. */
    switch (profile->flavor)
    {
        case AZ_IOT_HUB_FLAVOR_NEXT:
            return telemetry_send_next(client, profile, device_id, msg, cb, user_ctx);

        case AZ_IOT_HUB_FLAVOR_CLASSIC:
        default:
            return telemetry_send_classic(client, profile, device_id, msg, cb, user_ctx);
    }
}
