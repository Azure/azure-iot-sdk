// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
#include "azure/iot/az_iot_result.h"

const char* az_iot_result_to_string(az_iot_result r)
{
  switch (r)
  {
    case AZ_IOT_OK:
      return "AZ_IOT_OK";
    case AZ_IOT_ERR_INVALID_ARG:
      return "AZ_IOT_ERR_INVALID_ARG";
    case AZ_IOT_ERR_OUT_OF_MEMORY:
      return "AZ_IOT_ERR_OUT_OF_MEMORY";
    case AZ_IOT_ERR_NOT_INITIALIZED:
      return "AZ_IOT_ERR_NOT_INITIALIZED";
    case AZ_IOT_ERR_ALREADY_INITIALIZED:
      return "AZ_IOT_ERR_ALREADY_INITIALIZED";
    case AZ_IOT_ERR_NOT_CONNECTED:
      return "AZ_IOT_ERR_NOT_CONNECTED";
    case AZ_IOT_ERR_TIMEOUT:
      return "AZ_IOT_ERR_TIMEOUT";
    case AZ_IOT_ERR_TLS:
      return "AZ_IOT_ERR_TLS";
    case AZ_IOT_ERR_AUTH:
      return "AZ_IOT_ERR_AUTH";
    case AZ_IOT_ERR_PROTOCOL:
      return "AZ_IOT_ERR_PROTOCOL";
    case AZ_IOT_ERR_MQTT:
      return "AZ_IOT_ERR_MQTT";
    case AZ_IOT_ERR_DPS:
      return "AZ_IOT_ERR_DPS";
    case AZ_IOT_ERR_NOT_SUPPORTED:
      return "AZ_IOT_ERR_NOT_SUPPORTED";
    case AZ_IOT_ERR_BUSY:
      return "AZ_IOT_ERR_BUSY";
    case AZ_IOT_ERR_NOT_ENOUGH_SPACE:
      return "AZ_IOT_ERR_NOT_ENOUGH_SPACE";
    case AZ_IOT_ERR_DETACHED:
      return "AZ_IOT_ERR_DETACHED";
    case AZ_IOT_ERR_INTERNAL:
      return "AZ_IOT_ERR_INTERNAL";
    case AZ_IOT_ERR_NOT_FOUND:
      return "AZ_IOT_ERR_NOT_FOUND";
    case AZ_IOT_ERR_IDENTITY_REJECTED:
      return "AZ_IOT_ERR_IDENTITY_REJECTED";
    case AZ_IOT_ERR_CONNECTION_PROFILE_UNSUPPORTED:
      return "AZ_IOT_ERR_CONNECTION_PROFILE_UNSUPPORTED";
    case AZ_IOT_ERR_SUBSCRIPTION_REFUSED:
      return "AZ_IOT_ERR_SUBSCRIPTION_REFUSED";
    case AZ_IOT_ERR_CREDENTIAL_INCOMPLETE:
      return "AZ_IOT_ERR_CREDENTIAL_INCOMPLETE";
    case AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH:
      return "AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH";
    case AZ_IOT_ERR_PUBLISH_REFUSED:
      return "AZ_IOT_ERR_PUBLISH_REFUSED";
    default:
      return "AZ_IOT_ERR_UNKNOWN";
  }
}
