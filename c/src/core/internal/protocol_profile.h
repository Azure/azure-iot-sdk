// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Internal protocol-profile registry.
 *
 * The profile is the static side of the Classic-vs-Next switch tables: topic
 * prefixes, response timeouts, payload-codec hooks. Feature clients (Twin /
 * DirectMethod / Telemetry) consult the active profile to build outbound
 * topics and to declare inbound subscriptions to the ConnectionClient.
 *
 * The hub flavor is picked from the session role:
 *   HUB_CLASSIC, DPS  -> classic profile (MQTT v3.1.1)
 *   HUB_NEXT          -> next profile    (MQTT v5)
 *
 * Phase 2.3 ships only the Classic profile with topic prefixes lifted from the
 * Azure IoT Hub MQTT contract. Topic *builders* live in feature clients
 * (Phase 3) so this module stays tiny and table-only. Next remains a stub
 * (NULL fields) until Phase 3.
 */
#ifndef AZ_IOT_PROTOCOL_PROFILE_H
#define AZ_IOT_PROTOCOL_PROFILE_H

#include <stdbool.h>
#include <stdint.h>

#include "azure/iot/az_iot_mqtt_iface.h"
#include "azure/iot/az_iot_connection_client.h"

#ifdef __cplusplus
extern "C"
{
#endif

  typedef enum az_iot_hub_flavor
  {
    AZ_IOT_HUB_FLAVOR_CLASSIC = 0, /* IoT Hub Classic (MQTT v3.1.1) */
    AZ_IOT_HUB_FLAVOR_NEXT = 1 /* IoT Hub Next    (MQTT v5)     */
  } az_iot_hub_flavor;

  typedef struct az_iot_protocol_profile
  {
    az_iot_hub_flavor flavor;
    az_iot_mqtt_version mqtt_version;

    /* ----- Classic-specific fields (MQTT v3.1.1 topic-encoded) ----- */

    /* Inbound subscription prefixes. Feature clients pass these (or longer
     * filters built from them) to ConnectionClient's dispatch table so that
     * inbound MESSAGE events are routed to the right handler. NULL when the
     * feature is not supported on this flavor. */
    const char* twin_response_topic_prefix; /* e.g. "$iothub/twin/res/"                       */
    const char* twin_desired_topic_prefix; /* e.g. "$iothub/twin/PATCH/properties/desired/"  */
    const char* methods_request_topic_prefix; /* e.g. "$iothub/methods/POST/"                   */
    /* Outbound D2C template (still owned by feature client; provided here as a
     * canonical reference). Contains "{device_id}" / "{module_id}" substrings
     * the feature client substitutes. */
    const char* d2c_publish_topic_template; /* e.g. "devices/{device_id}/messages/events/"    */

    /* ----- Next-specific fields (MQTT v5, flat topics + User Properties) ----- */

    /* Base topic prefix template. Feature clients append "/srv/<feature>" or
     * "/dev/<feature>" to this. Contains "{device_id}" for substitution.
     * NULL for Classic profile. */
    const char* next_topic_base_template; /* e.g. "ih/{device_id}"                          */

    /* When true, feature clients use MQTT v5 User Properties and Correlation
     * Data for metadata instead of encoding it in topic path segments. */
    bool uses_mqtt5_properties;

    /* ----- Common fields ----- */

    /* Default request/response timeout in ms for feature clients
     * (twin get, direct-method response). Per-call overrides are still
     * supported by the feature client APIs themselves. */
    uint32_t default_request_response_timeout_ms;
  } az_iot_protocol_profile;

  /* Returns the active profile for the given session role, or NULL when no
   * profile is implemented yet for that role (Next stub returns NULL). The
   * pointer is to module-static data; never freed. */
  const az_iot_protocol_profile* az_iot_protocol_profile_for_role(az_iot_mqtt_role role);

  const char* az_iot_hub_flavor_to_string(az_iot_hub_flavor f);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_PROTOCOL_PROFILE_H */
