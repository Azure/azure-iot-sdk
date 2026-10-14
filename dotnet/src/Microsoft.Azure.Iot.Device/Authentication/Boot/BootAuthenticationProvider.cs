// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using System.Net.Security;

namespace Microsoft.Azure.Iot.Device.Authentication.Boot
{
    /// <summary>
    /// Supplies boot authentication material for Device Provisioning Service.
    /// </summary>
    public abstract class BootAuthenticationProvider
    {
        /// <summary>
        /// Gets the protocol-agnostic authentication material (client certificate, password/token, and TLS callbacks)
        /// for connecting to the device provisioning service. The caller maps it onto its own transport (MQTT, HTTP, etc.).
        /// Called only on the boot provider.
        /// </summary>
        /// <param name="cancellationToken">The cancellation token.</param>
        public abstract Task<AuthenticationCredentials> GetCredentialsAsync(CancellationToken cancellationToken = default);

        /// <summary>
        /// Gets the DPS registration Id. Called only on the boot provider.
        /// </summary>
        public abstract string GetRegistrationId();

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

        protected BootAuthenticationProvider(
            RemoteCertificateValidationCallback? remoteCertificateValidationCallback = null,
            LocalCertificateSelectionCallback? localCertificateSelectionCallback = null)
        {
            RemoteCertificateValidationCallback = remoteCertificateValidationCallback;
            LocalCertificateSelectionCallback = localCertificateSelectionCallback;
        }
    }
}
