// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

namespace Microsoft.Azure.Iot.Device.Models
{
    /// <summary>
    /// The MQTT protocol version a device should use to connect to its assigned IoT hub.
    /// </summary>
    public enum IotHubConnectionType
    {
        /// <summary>
        /// Classic MQTT 3.x capable IoT hub. This is the default when unspecified.
        /// </summary>
        Mqttv3,

        /// <summary>
        /// MQTT 5 capable IoT hub.
        /// </summary>
        Mqttv5
    }
}
