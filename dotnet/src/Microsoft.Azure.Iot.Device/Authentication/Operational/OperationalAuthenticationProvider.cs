// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using System.Net.Security;

namespace Microsoft.Azure.Iot.Device.Authentication.Operational
{
    /// <summary>
    /// Supplies operational authentication material for IoT Hub.
    /// </summary>
    public abstract class OperationalAuthenticationProvider
    {
        /// <summary>
        /// Gets protocol-agnostic authentication material for connecting to IoT Hub.
        /// The caller maps it onto its own transport (MQTT, HTTP, etc.).
        /// </summary>
        /// <param name="hostName">The assigned IoT Hub host name.</param>
        /// <param name="deviceId">The assigned device Id.</param>
        /// <param name="issuedClientCertificates">
        /// The most recently issued certificate chain from DPS or IoT Hub, Base64-encoded and device certificate
        /// first, followed by intermediates. Null if no certificate has been issued.
        /// </param>
        /// <param name="cancellationToken">The cancellation token.</param>
        /// <remarks>
        /// Called before each connection attempt to IoT Hub, always with the latest issued certificate chain. Implementations
        /// are responsible for turning that chain into the certificate to present, and should cache the result per chain.
        /// </remarks>
        public abstract Task<AuthenticationCredentials> GetCredentialsAsync(
            string hostName,
            string deviceId,
            IReadOnlyList<string>? issuedClientCertificates,
            CancellationToken cancellationToken = default);

        /// <summary>
        /// An optional callback for validating the remote TLS certificate. Null uses platform validation.
        /// </summary>
        public RemoteCertificateValidationCallback? RemoteCertificateValidationCallback { get; }

        /// <summary>
        /// An optional callback for selecting the client TLS certificate.
        /// Null presents the certificate returned in the authentication credentials.
        /// </summary>
        public LocalCertificateSelectionCallback? LocalCertificateSelectionCallback { get; }

        protected OperationalAuthenticationProvider(
            RemoteCertificateValidationCallback? remoteCertificateValidationCallback = null,
            LocalCertificateSelectionCallback? localCertificateSelectionCallback = null)
        {
            RemoteCertificateValidationCallback = remoteCertificateValidationCallback;
            LocalCertificateSelectionCallback = localCertificateSelectionCallback;
        }
    }
}
