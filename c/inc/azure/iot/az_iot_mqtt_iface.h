// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/* SPDX-License-Identifier: MIT */
#ifndef AZ_IOT_MQTT_IFACE_H
#define AZ_IOT_MQTT_IFACE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "az_iot_result.h"

#ifdef __cplusplus
extern "C" {
#endif

/* MQTT protocol versions supported by adapters. DPS + IoTHub-Classic require v3.1.1;
 * IoTHub-Next requires v5. A single adapter binary may register factories for both,
 * but each instance speaks exactly one version. */
typedef enum az_iot_mqtt_version_tag
{
    AZ_IOT_MQTT_VERSION_3_1_1 = 0,
    AZ_IOT_MQTT_VERSION_5     = 1
} az_iot_mqtt_version_t;

typedef enum az_iot_mqtt_qos_tag
{
    AZ_IOT_MQTT_QOS_0 = 0,
    AZ_IOT_MQTT_QOS_1 = 1,
    AZ_IOT_MQTT_QOS_2 = 2
} az_iot_mqtt_qos_t;

/* MQTT v5 User Property (key-value pair). Carried on PUBLISH and CONNECT.
 * Ignored by v3.1.1 adapters. */
typedef struct az_iot_mqtt_user_property_tag
{
    const char* key;
    const char* value;
} az_iot_mqtt_user_property_t;

typedef struct az_iot_mqtt_message_tag
{
    const char* topic;
    const uint8_t* payload;
    size_t payload_len;
    az_iot_mqtt_qos_t qos;
    bool retain;
    /* MQTTv5-only fields; ignored by v3.1.1 adapters. */
    const az_iot_mqtt_user_property_t* user_properties; /* typed array */
    size_t user_properties_count;                       /* number of entries */
    uint16_t topic_alias;              /* 0 = none */
    uint32_t message_expiry_seconds;   /* 0 = none */
    const char* content_type;
    const char* response_topic;
    const uint8_t* correlation_data;
    size_t correlation_data_len;
} az_iot_mqtt_message_t;

typedef struct az_iot_mqtt_tls_options_tag
{
    const char* trusted_ca_path;       /* file or NULL for system store */
    const char* client_cert_path;      /* PEM */
    const char* client_key_path;       /* PEM */
    const char* client_key_password;   /* may be NULL */
    bool verify_server;                /* default true */
    /* In-memory PEM material. Adapters that load credentials from memory rather
     * than from disk (e.g. esp-mqtt on a device with no filesystem) use these;
     * file-path adapters (Paho + OpenSSL) ignore them. Any field may be NULL. */
    const char* trusted_ca_pem;        /* CA chain PEM, or NULL */
    const char* client_cert_pem;       /* client certificate PEM, or NULL */
    const char* client_key_pem;        /* client private key PEM, or NULL */
} az_iot_mqtt_tls_options_t;

typedef struct az_iot_mqtt_connect_options_tag
{
    const char* host;
    uint16_t port;                     /* typically 8883 */
    const char* client_id;
    const char* username;              /* may be NULL */
    const char* password;              /* may be NULL */
    uint16_t keep_alive_seconds;
    uint32_t connect_timeout_ms;
    az_iot_mqtt_tls_options_t tls;
    /* MQTTv5-only fields. */
    bool clean_start;                  /* v5 Clean Start flag (v3.1.1: maps to cleanSession) */
    uint32_t session_expiry_seconds;
    const az_iot_mqtt_user_property_t* user_properties; /* typed array */
    size_t user_properties_count;
    /* Last Will and Testament (v3.1.1 + v5). */
    struct {
        const char* topic;             /* NULL = no LWT */
        const uint8_t* payload;
        size_t payload_len;
        az_iot_mqtt_qos_t qos;
        bool retain;
        uint32_t will_delay_seconds;   /* v5 only; 0 = immediate */
    } lwt;
} az_iot_mqtt_connect_options_t;

/* Inbound event types delivered through the single adapter callback. */
typedef enum az_iot_mqtt_event_kind_tag
{
    AZ_IOT_MQTT_EVT_CONNECTED = 0,
    AZ_IOT_MQTT_EVT_DISCONNECTED,
    AZ_IOT_MQTT_EVT_MESSAGE,
    AZ_IOT_MQTT_EVT_PUBLISH_ACK,
    AZ_IOT_MQTT_EVT_SUBSCRIBE_ACK,
    AZ_IOT_MQTT_EVT_UNSUBSCRIBE_ACK,
    AZ_IOT_MQTT_EVT_ERROR
} az_iot_mqtt_event_kind_t;

typedef struct az_iot_mqtt_event_tag
{
    az_iot_mqtt_event_kind_t kind;
    uint16_t packet_id;                /* for *_ACK events */
    az_iot_result_t status;               /* for ACK / ERROR events */
    const az_iot_mqtt_message_t* message; /* for AZ_IOT_MQTT_EVT_MESSAGE only */
    bool session_present;              /* for AZ_IOT_MQTT_EVT_CONNECTED (v5 CONNACK) */
} az_iot_mqtt_event_t;

typedef void (*az_iot_mqtt_event_cb)(const az_iot_mqtt_event_t* evt, void* user_ctx);

typedef struct az_iot_mqtt_client_tag az_iot_mqtt_client_t;

/* Adapter vtable. Each instance carries its own version. All calls are non-blocking;
 * actual I/O happens inside process_loop(). */
typedef struct az_iot_mqtt_iface_tag
{
    az_iot_mqtt_version_t version;

    az_iot_result_t (*connect)(az_iot_mqtt_client_t* self, const az_iot_mqtt_connect_options_t* opts);
    az_iot_result_t (*disconnect)(az_iot_mqtt_client_t* self);
    az_iot_result_t (*subscribe)(az_iot_mqtt_client_t* self, const char* topic_filter, az_iot_mqtt_qos_t qos, uint16_t* out_packet_id);
    az_iot_result_t (*unsubscribe)(az_iot_mqtt_client_t* self, const char* topic_filter, uint16_t* out_packet_id);
    az_iot_result_t (*publish)(az_iot_mqtt_client_t* self, const az_iot_mqtt_message_t* msg, uint16_t* out_packet_id);
    az_iot_result_t (*process_loop)(az_iot_mqtt_client_t* self, uint32_t timeout_ms);
    void         (*set_inbound_cb)(az_iot_mqtt_client_t* self, az_iot_mqtt_event_cb cb, void* user_ctx);
    void         (*destroy)(az_iot_mqtt_client_t* self);
} az_iot_mqtt_iface_t;

/* Concrete client object returned by a factory. The first member MUST be a pointer
 * to the iface so the core can dispatch generically. */
struct az_iot_mqtt_client_tag
{
    const az_iot_mqtt_iface_t* iface;
    /* adapter-specific state follows */
};

/* Factory: produces a client that speaks a specific MQTT version. The SDK
 * selects a factory by version at connection time based on the service being
 * targeted (DPS/Classic require v3.1.1, Hub-Next requires v5). */
typedef struct az_iot_mqtt_factory_tag
{
    az_iot_mqtt_version_t version;
    az_iot_mqtt_client_t* (*create)(void* factory_ctx);
    void* factory_ctx;
    /* Called by connection_client_destroy() to free factory resources.
     * NULL means no cleanup needed (e.g. stack-allocated factory). */
    void (*destroy)(void* factory_ctx);
} az_iot_mqtt_factory_t;

const char* az_iot_mqtt_version_to_string(az_iot_mqtt_version_t v);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_MQTT_IFACE_H */
