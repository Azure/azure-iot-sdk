// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
#ifndef AZ_IOT_RESULT_H
#define AZ_IOT_RESULT_H

#include <azure/az_core.h> /* AZ_NODISCARD (and az_span, used across the public headers) */

#ifdef __cplusplus
extern "C"
{
#endif

  typedef enum az_iot_result
  {
    AZ_IOT_OK = 0,
    AZ_IOT_ERR_INVALID_ARG,
    AZ_IOT_ERR_OUT_OF_MEMORY,
    AZ_IOT_ERR_NOT_INITIALIZED,
    AZ_IOT_ERR_ALREADY_INITIALIZED,
    AZ_IOT_ERR_NOT_CONNECTED,
    AZ_IOT_ERR_TIMEOUT,
    AZ_IOT_ERR_TLS,
    AZ_IOT_ERR_AUTH,
    AZ_IOT_ERR_PROTOCOL,
    AZ_IOT_ERR_MQTT,
    AZ_IOT_ERR_DPS,
    AZ_IOT_ERR_NOT_SUPPORTED,
    AZ_IOT_ERR_BUSY,
    AZ_IOT_ERR_NOT_ENOUGH_SPACE,
    AZ_IOT_ERR_DETACHED,
    AZ_IOT_ERR_INTERNAL,
    AZ_IOT_ERR_NOT_FOUND,
    /* The broker refused the identity itself (rejected client id, credentials or
     * authorization) rather than failing to carry the connection. Distinct from
     * AZ_IOT_ERR_MQTT because retrying the same identity cannot help: the SDK
     * re-provisions through DPS on this result, and only on this result, so a
     * hub outage never turns into a DPS stampede. New values must keep being
     * appended here so existing numeric values do not shift. */
    AZ_IOT_ERR_IDENTITY_REJECTED,
    /* The service reported a connectionProfile this SDK does not recognise, so
     * it does not know which MQTT version to speak. Connecting anyway would mean
     * guessing the wire protocol, so the connection fails closed -- the profile
     * stays readable via az_iot_connection_client_get_hub_profile(), including
     * the verbatim wire string, so the value can be logged or reported. */
    AZ_IOT_ERR_CONNECTION_PROFILE_UNSUPPORTED,
    /* The broker refused a topic filter for a reason a retry cannot change --
     * the filter is not authorized, or not one it will ever accept. Distinct
     * from AZ_IOT_ERR_MQTT so a caller can separate a filter that will be
     * refused identically next time from one that failed because the service
     * was briefly unwell. What the connection does with that distinction is
     * the subscription gate's decision, not this code's. */
    AZ_IOT_ERR_SUBSCRIPTION_REFUSED,
    /* The credential set cannot complete a TLS handshake: a client certificate
     * is present but no private key comes with it in ANY form -- no PEM, no
     * file, no non-extractable key reference, no sign() hook. Distinct from
     * AZ_IOT_ERR_AUTH, which is the service refusing a credential that was at
     * least complete, and from AZ_IOT_ERR_NOT_SUPPORTED, which is an adapter
     * declining a key form it cannot use. This one never reaches the network:
     * it is caught at open, because the alternative is a NULL private key
     * failing deep inside the TLS stack with nothing that names the cause. */
    AZ_IOT_ERR_CREDENTIAL_INCOMPLETE,
    /* A generation-specific feature client was initialized against a
     * connection resolved to the other profile. */
    AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH
  } az_iot_result;

  const char* az_iot_result_to_string(az_iot_result r);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_RESULT_H */
