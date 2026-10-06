// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

namespace Microsoft.Azure.Iot.Device.Models
{
    /// <summary>
    /// The context under which a provisioned device should connect to IoT Hub with
    /// </summary>
    public class ConnectionContext
    {
        /// <summary>
        /// The device Id to connect to IoT Hub as.
        /// </summary>
        public required string DeviceId { get; init; }

        /// <summary>
        /// The hostname of the IoT Hub to connect to.
        /// </summary>
        public required string IotHubHostName { get; init; }

        /// <summary>
        /// The type of IoT Hub that will be connected to.
        /// </summary>
        public required IotHubConnectionType ConnectionProfile { get; init; }

        /// <summary>
        /// Any client certificates that were issued during provisioning that should be used when connecting to IoT Hub.
        /// </summary>
        public IReadOnlyList<string>? IssuedClientCertificates { get; init; }

        /// <summary>
        /// The authentication to use when connecting to IoT Hub.
        /// </summary>
        public required X509AuthenticationProvider AuthenticationProvider { get; set; }
    }
}
