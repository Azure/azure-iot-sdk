// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

namespace Microsoft.Azure.Iot.Device.Models
{
    /// <summary>
    /// The protocol used to register a device with Device Provisioning Service.
    /// </summary>
    /// <remarks>
    /// This only affects provisioning. Once assigned, the device always connects to its IoT hub over MQTT.
    /// </remarks>
    public enum ProvisioningTransport
    {
        /// <summary>
        /// Register over MQTT (port 8883, or WebSockets on port 443).
        /// </summary>
        Mqtt,

        /// <summary>
        /// Register over HTTPS (port 443) using the Device Provisioning Service REST API.
        /// </summary>
        Http,
    }
}
