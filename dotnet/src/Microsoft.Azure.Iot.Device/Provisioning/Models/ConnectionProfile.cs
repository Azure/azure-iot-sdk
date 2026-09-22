// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using System;
using System.Collections.Generic;
using System.Runtime.Serialization;
using System.Text;

namespace Microsoft.Azure.Iot.Device.Provisioning.Models
{
    public enum ConnectionProfile
    {
        /// <summary>
        /// Classic MQTT 3.x capable IoT Hub. This is the default when unspecified.
        /// </summary>
        [EnumMember(Value = "classic")]
        Classic,

        /// <summary>
        /// MQTT 5 capable IoT Hub.
        /// </summary>
        [EnumMember(Value = "mqttV5")]
        MqttV5
    }
}
