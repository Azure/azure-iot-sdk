// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Mqtt;
using System.Net.Security;

namespace Microsoft.Azure.Iot.Device.Authentication
{
    public abstract class AuthenticationProvider
    {
        //TODO track signed certificates in here somehow? SecureString stuff probably
        // Kill the onCsrCompleted callback thing since now users who want to customly handle signed certificates just implement this interface instead

        /// <summary>
        /// Gets the protocol-agnostic authentication material (client certificate, password/token, and TLS callbacks)
        /// for connecting to the given endpoint. The caller maps it onto its own transport (MQTT, HTTP, etc.).
        /// </summary>
        /// <param name="endpointKind">Which service the connection targets.</param>
        /// <param name="hostName">The host name being connected to.</param>
        /// <param name="deviceId">The device Id (or registration Id when targeting DPS) being authenticated.</param>
        public virtual AuthenticationCredentials GetAuthentication(ConnectionEndpoint endpointKind, string hostName, string deviceId)
        {
            return new AuthenticationCredentials()
            {
                LocalCertificateSelectionCallback = LocalCertificateSelectionCallback,
                RemoteCertificateValidationCallback = RemoteCertificateValidationCallback,
            };
        }

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
