// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using System.Net.Security;

namespace Microsoft.Azure.Iot.Device.Authentication
{
    /// <summary>
    /// Supplies authentication material for either the boot (Device Provisioning Service) or operational (IoT Hub)
    /// role. Supply each role explicitly when provisioning; each provider is used only for its assigned endpoint.
    /// </summary>
    public abstract class AuthenticationProvider
    {
        /// <summary>
        /// Gets the protocol-agnostic authentication material (client certificate, password/token, and TLS callbacks)
        /// for connecting to an IoT Hub. The caller maps it onto its own transport (MQTT, HTTP, etc.).
        /// Called only on the operational provider.
        /// </summary>
        /// <param name="hostName">The IoT Hub host name being connected to.</param>
        /// <param name="deviceId">The device Id being authenticated.</param>
        /// <param name="issuedClientCertificates">
        /// The most recently issued client certificate chain for this device, whether it was signed by the device
        /// provisioning service during provisioning or signed by IoT Hub through a certificate signing request. Each
        /// entry is a Base64-encoded certificate; the first is the issued device certificate, followed by any
        /// intermediates. Null if no client certificate has been issued to this device.
        /// </param>
        public abstract AuthenticationCredentials GetIotHubAuthentication(string hostName, string deviceId, IReadOnlyList<string>? issuedClientCertificates);

        /// <summary>
        /// Gets the protocol-agnostic authentication material (client certificate, password/token, and TLS callbacks)
        /// for connecting to the device provisioning service. The caller maps it onto its own transport (MQTT, HTTP, etc.).
        /// Called only on the boot provider.
        /// </summary>
        public abstract AuthenticationCredentials GetDeviceProvisioningServiceAuthentication();

        /// <summary>
        /// Gets the DPS registration Id. Called only on the boot provider.
        /// </summary>
        public abstract string GetRegistrationId();

        /// <summary>
        /// Called (asynchronously) when a client certificate chain is issued to this device, and before each connection
        /// attempt to IoT Hub, so the operational provider can prepare the certificate to present.
        /// Never called on the boot provider. Must be idempotent per chain.
        /// </summary>
        public virtual Task HandleClientCertificatesIssuedAsync(IReadOnlyList<string> issuedClientCertificates) => Task.CompletedTask;

        /// <summary>
        /// An optional callback for custom validation of the server (remote) certificate during the TLS handshake.
        /// When null, the platform's default validation is used.
        /// </summary>
        public RemoteCertificateValidationCallback? RemoteCertificateValidationCallback { get; }

        /// <summary>
        /// An optional callback for selecting which client certificate to present during the TLS handshake.
        /// When null, the client certificate returned in the authentication credentials is presented.
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
