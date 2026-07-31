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
 * bag entries are URL-encoded, and both halves of every pair are encoded here.
 * Values must be, because an unencoded '&', '=' or '%' would otherwise be read
 * by the service as bag structure rather than content. Keys must be, because
 * the system property names begin with '$': encoding turns "$.ct" into
 * "%24.ct", which is byte-for-byte the form azure-sdk-for-c puts on the wire
 * (see AZ_IOT_MESSAGE_PROPERTIES_CONTENT_TYPE) and keeps the reserved '$' out
 * of the topic. Callers therefore always pass plain text, never pre-encoded
 * text, for both key and value.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "azure/iot/az_iot_telemetry_client.h"
#include "azure/iot/az_iot_mqtt_iface.h"

#include "internal/connection_client_internal.h"
#include "internal/log_internal.h"
#include "internal/protocol_profile.h"
#include "internal/span_writer.h"

#define AZ_IOT_TELEMETRY_TOPIC_MAX 512

/* Max user properties on a single Next-mode telemetry publish (type + content-type +
 * forwarded application properties). Generous for telemetry use cases. */
#define AZ_IOT_TELEMETRY_MAX_USER_PROPS 16

az_iot_result az_iot_telemetry_client_init(
    az_iot_telemetry_client* client,
    az_iot_connection_client* conn)
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

void az_iot_telemetry_client_destroy(az_iot_telemetry_client* client)
{
    if (client == NULL) return;
    memset(client, 0, sizeof(*client));
}

/* The single substitution the profile topic templates use. */
static const char k_device_id_placeholder[] = "{device_id}";

/* Appends @p tmpl with its "{device_id}" placeholder replaced by @p device_id.
 * Returns false when the template carries no placeholder, which would mean the
 * profile table itself is malformed rather than the caller being at fault.
 * The template is our own NUL-terminated constant, so locating the placeholder
 * with strstr is a lookup in trusted data, not a parse of untrusted input. */
static bool append_template_with_device_id(
    az_iot_span_writer* writer, const char* tmpl, const char* device_id)
{
    const char* placeholder = strstr(tmpl, k_device_id_placeholder);
    if (placeholder == NULL) return false;

    az_iot_span_writer_append_span(
        writer,
        az_span_create((uint8_t*)(uintptr_t)tmpl, (int32_t)(placeholder - tmpl)));
    az_iot_span_writer_append_str(writer, device_id);
    az_iot_span_writer_append_str(writer, placeholder + (sizeof(k_device_id_placeholder) - 1));
    return true;
}

/* Build "devices/<device_id>/messages/events/" plus the optional property bag
 * into out_topic. Classic (MQTT v3.1.1) path only. */
static az_iot_result build_topic_classic(
    const az_iot_protocol_profile* profile,
    const char* device_id,
    const az_iot_telemetry_message* msg,
    char* out_topic, size_t cap)
{
    const char* tmpl = profile->d2c_publish_topic_template;
    if (tmpl == NULL)
    {
        return AZ_IOT_ERR_NOT_SUPPORTED;
    }

    az_iot_span_writer writer;
    az_iot_span_writer_init(&writer, az_span_create((uint8_t*)out_topic, (int32_t)cap));

    if (!append_template_with_device_id(&writer, tmpl, device_id))
    {
        return AZ_IOT_ERR_INTERNAL;
    }

    /* Property bag: system and application properties serialize uniformly as
     * key=value pairs separated by '&'. Both halves are percent-encoded; only
     * the '&' and '=' emitted here carry structure. */
    bool first = true;
    for (size_t i = 0; i < msg->properties_count; ++i)
    {
        const az_iot_telemetry_property* p = &msg->properties[i];
        if (p->key == NULL || p->key[0] == '\0')
        {
            continue;
        }
        if (!first)
        {
            az_iot_span_writer_append_u8(&writer, (uint8_t)'&');
        }
        az_iot_span_writer_append_url_encoded(&writer, p->key);
        if (p->value != NULL)
        {
            az_iot_span_writer_append_u8(&writer, (uint8_t)'=');
            az_iot_span_writer_append_url_encoded(&writer, p->value);
        }
        first = false;
    }

    return az_iot_span_writer_end_str(&writer, NULL);
}

/* Build the flat topic "ih/<device_id>/srv/telemetry" for Next (MQTT v5). */
static az_iot_result build_topic_next(
    const az_iot_protocol_profile* profile,
    const char* device_id,
    char* out_topic, size_t cap)
{
    const char* tmpl = profile->next_topic_base_template;
    if (tmpl == NULL)
    {
        return AZ_IOT_ERR_NOT_SUPPORTED;
    }

    az_iot_span_writer writer;
    az_iot_span_writer_init(&writer, az_span_create((uint8_t*)out_topic, (int32_t)cap));

    if (!append_template_with_device_id(&writer, tmpl, device_id))
    {
        return AZ_IOT_ERR_INTERNAL;
    }
    az_iot_span_writer_append_str(&writer, "/srv/telemetry");

    return az_iot_span_writer_end_str(&writer, NULL);
}

/* -----------------------------------------------------------------------
 * telemetry_send_classic — Classic IoT Hub path (MQTT v3.1.1, topic-encoded)
 * ----------------------------------------------------------------------- */
static az_iot_result telemetry_send_classic(
    az_iot_telemetry_client* client,
    const az_iot_protocol_profile* profile,
    const char* device_id,
    const az_iot_telemetry_message* msg,
    az_iot_telemetry_send_callback cb,
    void* user_ctx)
{
    char topic[AZ_IOT_TELEMETRY_TOPIC_MAX];
    az_iot_result r = build_topic_classic(profile, device_id, msg, topic, sizeof(topic));
    if (r != AZ_IOT_OK)
    {
        return r;
    }

    az_iot_mqtt_message out = { 0 };
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
static az_iot_result telemetry_send_next(
    az_iot_telemetry_client* client,
    const az_iot_protocol_profile* profile,
    const char* device_id,
    const az_iot_telemetry_message* msg,
    az_iot_telemetry_send_callback cb,
    void* user_ctx)
{
    char topic[AZ_IOT_TELEMETRY_TOPIC_MAX];
    az_iot_result r = build_topic_next(profile, device_id, topic, sizeof(topic));
    if (r != AZ_IOT_OK)
    {
        return r;
    }

    /* MQTT v5 User Properties carry the message type and content-type.
     * Application properties from the caller are also forwarded. */
    az_iot_mqtt_user_property user_props[AZ_IOT_TELEMETRY_MAX_USER_PROPS];
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
        const az_iot_telemetry_property* p = &msg->properties[i];
        if (p->key == NULL || p->key[0] == '\0') continue;
        /* Skip system properties ($.ct handled above) in user-prop forwarding */
        if (p->key[0] == '$' && p->key[1] == '.') continue;
        user_props[up_count].key = p->key;
        user_props[up_count].value = p->value ? p->value : "";
        up_count++;
    }

    az_iot_mqtt_message out = { 0 };
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

az_iot_result az_iot_telemetry_client_send(
    az_iot_telemetry_client* client,
    const az_iot_telemetry_message* msg,
    az_iot_telemetry_send_callback cb,
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

    const az_iot_protocol_profile* profile =
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
