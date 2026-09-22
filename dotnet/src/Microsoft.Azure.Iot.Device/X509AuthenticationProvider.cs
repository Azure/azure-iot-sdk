// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using System;
using System.Net.Security;
using System.Security.Cryptography.X509Certificates;

namespace Microsoft.Azure.Iot.Device
{
    /// <summary>
    /// The device authentication for using an X509 certificate object.
    /// </summary>
    public class X509AuthenticationProvider
    {
        /// <summary>
        /// Creates an instance of this class.
        /// </summary>
        /// <param name="clientCertificate">
        /// The client certificate used for authentication.
        /// <para>
        /// The certificate's private key does not need to be exportable, and its key material never leaves this process.
        /// The TLS stack only invokes the private key to produce the handshake signature, so a key held in a hardware
        /// security module (HSM) is fully supported: on Windows the certificate can be backed by a CNG Key Storage
        /// Provider from the certificate store, and cross-platform the certificate can be bound to a custom
        /// <see cref="System.Security.Cryptography.RSA"/> or <see cref="System.Security.Cryptography.ECDsa"/> (for
        /// example a PKCS#11 or Key Vault backed key) via
        /// <see cref="System.Security.Cryptography.X509Certificates.RSACertificateExtensions.CopyWithPrivateKey(X509Certificate2, System.Security.Cryptography.RSA)"/>.
        /// The SDK never exports this certificate or its private key.
        /// </para>
        /// </param>
        /// <param name="certificateChain">
        /// The certificate chain leading to the root certificate uploaded to the device provisioning service.
        /// </param>
        /// <param name="remoteCertificateValidationCallback">
        /// An optional callback used to perform custom validation of the server (remote) certificate presented during
        /// the TLS handshake, for example certificate pinning or trusting a private/enterprise root. When null, the
        /// platform's default validation is used. This callback validates the peer's certificate only; it is not
        /// involved in signing with the client's private key.
        /// </param>
        /// <param name="localCertificateSelectionCallback">
        /// An optional callback used to select which client certificate to present during the TLS handshake, for example
        /// to choose a specific certificate from a store or to support certificate rotation. When null,
        /// <paramref name="clientCertificate"/> is presented. The selected certificate must be able to sign with its
        /// associated (possibly HSM-backed) private key.
        /// </param>
        public X509AuthenticationProvider(
            X509Certificate2 clientCertificate,
            X509Certificate2Collection? certificateChain = null,
            RemoteCertificateValidationCallback? remoteCertificateValidationCallback = null,
            LocalCertificateSelectionCallback? localCertificateSelectionCallback = null)
        {
            ClientCertificate = clientCertificate
                ?? throw new ArgumentException("No certificate was found. To use certificate authentication certificate must be present.", nameof(clientCertificate));

            CertificateChain = certificateChain;
            RemoteCertificateValidationCallback = remoteCertificateValidationCallback;
            LocalCertificateSelectionCallback = localCertificateSelectionCallback;
        }

        /// <summary>
        /// The client certificate used for TLS device authentication.
        /// </summary>
        public X509Certificate2 ClientCertificate { get; }

        /// <summary>
        /// The certificate trust chain that will end in the Trusted Root installed on the server side.
        /// </summary>
        public X509Certificate2Collection? CertificateChain { get; }

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

        internal string GetRegistrationId()
        {
            return ClientCertificate.GetNameInfo(X509NameType.DnsName, false);
        }
    }
}
