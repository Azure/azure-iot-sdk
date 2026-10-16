// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using System.Net.Security;
using System.Security.Cryptography.X509Certificates;

namespace Microsoft.Azure.Iot.Device.Authentication
{
    /// <summary>
    /// Protocol-agnostic authentication material produced by an <see cref="AuthenticationProvider"/>.
    /// Each transport (MQTT, HTTP, ...) maps these values onto its own connection options.
    /// </summary>
    public class AuthenticationCredentials
    {
        /// <summary>
        /// The client certificate to present during the TLS handshake, if the provider authenticates with one.
        /// </summary>
        public X509Certificate2? ClientCertificate { get; init; }

        /// <summary>
        /// A password or token (for example a UTF-8 encoded SAS token) to send as the shared secret for the connection.
        /// Empty when the provider authenticates with a client certificate only.
        /// </summary>
        public byte[] Password { get; init; } = Array.Empty<byte>();

        /// <summary>
        /// An optional callback for custom validation of the server (remote) certificate during the TLS handshake.
        /// When null, the platform's default validation is used.
        /// </summary>
        public RemoteCertificateValidationCallback? RemoteCertificateValidationCallback { get; init; }

        /// <summary>
        /// An optional callback for selecting which client certificate to present during the TLS handshake.
        /// When null, <see cref="ClientCertificate"/> is presented.
        /// </summary>
        public LocalCertificateSelectionCallback? LocalCertificateSelectionCallback { get; init; }
    }
}
