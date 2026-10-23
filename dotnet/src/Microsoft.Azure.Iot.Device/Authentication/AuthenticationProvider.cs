// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Mqtt;
using System.Net.Security;

namespace Microsoft.Azure.Iot.Device.Authentication
{
    public abstract class AuthenticationProvider
    {
        /// <summary>
        /// Gets the protocol-agnostic authentication material (client certificate, password/token, and TLS callbacks)
        /// for connecting to an IoT Hub. The caller maps it onto its own transport (MQTT, HTTP, etc.).
        /// </summary>
        /// <param name="hostName">The IoT Hub host name being connected to.</param>
        /// <param name="deviceId">The device Id being authenticated.</param>
        /// <param name="issuedClientCertificates">
        /// The most recently issued client certificate chain for this device, whether it was signed by the device
        /// provisioning service during provisioning or signed by IoT Hub through a certificate signing request. Each
        /// entry is a Base64-encoded certificate; the first is the issued device certificate, followed by any
        /// intermediates. Null if no client certificate has been issued to this device.
        /// </param>
        /// <remarks>
        /// Called before every connection attempt to IoT Hub. Providers should prepare any issued certificates here
        /// before returning the credentials to present. If preparation fails, the connection attempt fails.
        /// </remarks>
        public abstract Task<AuthenticationCredentials> GetOperationalAuthenticationCredentialsAsync(string hostName, string deviceId, IReadOnlyList<string>? issuedClientCertificates, CancellationToken cancellationToken = default);

        /// <summary>
        /// Gets the protocol-agnostic authentication material (client certificate, password/token, and TLS callbacks)
        /// for connecting to the device provisioning service. The caller maps it onto its own transport (MQTT, HTTP, etc.).
        /// </summary>
        /// <param name="hostName">The device provisioning service host name being connected to.</param>
        public abstract AuthenticationCredentials GetBootAuthenticationCredentials();

        public abstract string GetRegistrationId();

        /// <summary>
        /// An optional callback for custom validation of the server (remote) certificate during the TLS handshake.
        /// When null, the platform's default validation is used.
        /// </summary>
        public RemoteCertificateValidationCallback? RemoteCertificateValidationCallback { get; }

        /// <summary>
        /// An optional callback for selecting which client certificate to present during the TLS handshake.
        /// When null, <see cref="ClientCertificate"/> is presented.
        /// </summary>
        public LocalCertificateSelectionCallback? LocalCertificateSelectionCallback { get; }

        public AuthenticationProvider(
            RemoteCertificateValidationCallback? remoteCertificateValidationCallback = null,
            LocalCertificateSelectionCallback? localCertificateSelectionCallback = null)
        {
            RemoteCertificateValidationCallback = remoteCertificateValidationCallback;
            LocalCertificateSelectionCallback = localCertificateSelectionCallback;
        }
    }
}
