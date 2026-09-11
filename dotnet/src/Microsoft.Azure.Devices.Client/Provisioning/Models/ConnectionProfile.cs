using System;
using System.Collections.Generic;
using System.Runtime.Serialization;
using System.Text;

namespace Microsoft.Azure.Devices.Client.Provisioning.Models
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
