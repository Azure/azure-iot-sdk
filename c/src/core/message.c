// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Helpers over the shared message types. Only mqttv3 produces az_iot_c2d_message:
 * C2D is not carried on the MQTT v5 hub. */
#include <string.h>

#include "azure/iot/az_iot_message.h"

const char* az_iot_c2d_message_property(const az_iot_c2d_message* msg, const char* key)
{
  /* properties may be NULL with a non-zero count only if a caller built the
   * struct by hand, but this is public API and a crash is a poor answer to
   * that. */
  if (!msg || !key || !msg->properties)
  {
    return NULL;
  }
  for (size_t i = 0; i < msg->properties_count; ++i)
  {
    if (msg->properties[i].key && strcmp(msg->properties[i].key, key) == 0)
    {
      return msg->properties[i].value;
    }
  }
  return NULL;
}
