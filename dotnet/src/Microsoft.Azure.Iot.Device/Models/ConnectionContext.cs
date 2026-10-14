// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Authentication;

namespace Microsoft.Azure.Iot.Device.Models
{
    /// <summary>
    /// The context used by a provisioned device to connect to IoT Hub.
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
        /// The most recently issued client certificate chain (Base64-encoded, device certificate first), whether signed
        /// during provisioning or signed by IoT Hub. Passed to
        /// <see cref="AuthenticationProvider.GetIotHubAuthentication(string, string, IReadOnlyList{string}?)"/> on every
        /// connection attempt to IoT Hub. Null if no client certificate has been issued.
        /// </summary>
        public IReadOnlyList<string>? IssuedClientCertificates
        {
            get => _issuedClientCertificates;
            set => _issuedClientCertificates = value;
        }

        // Volatile because IoT Hub certificate signing responses update it on a connection callback thread while
        // reconnect attempts read it on another.
        private volatile IReadOnlyList<string>? _issuedClientCertificates;

        /// <summary>
        /// The operational authentication provider used for all IoT Hub connections and issued certificates.
        /// Boot authentication for Device Provisioning Service is supplied separately when provisioning.
        /// </summary>
        public required AuthenticationProvider OperationalAuthenticationProvider { get; set; }
    }
}
