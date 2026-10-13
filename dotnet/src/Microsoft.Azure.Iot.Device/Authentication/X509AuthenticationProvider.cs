// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Mqtt;
using System;
using System.Diagnostics;
using System.Net.Security;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;

namespace Microsoft.Azure.Iot.Device.Authentication
{
    /// <summary>
    /// The device authentication for using an X509 certificate object.
    /// </summary>
    public class X509AuthenticationProvider : AuthenticationProvider
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
        /// <param name="issuedCertificatePrivateKey">
        /// The private key that this device's certificate signing requests are generated with. When provided, and a
        /// client certificate has been issued to this device (either by the device provisioning service during
        /// provisioning or by IoT Hub), the issued device certificate is paired with this key and presented to IoT Hub
        /// instead of <paramref name="clientCertificate"/>. When null, <paramref name="clientCertificate"/> is always presented.
        /// </param>
        public X509AuthenticationProvider(
            X509Certificate2 clientCertificate,
            X509Certificate2Collection? certificateChain = null,
            RemoteCertificateValidationCallback? remoteCertificateValidationCallback = null,
            LocalCertificateSelectionCallback? localCertificateSelectionCallback = null,
            AsymmetricAlgorithm? issuedCertificatePrivateKey = null) : base(remoteCertificateValidationCallback, localCertificateSelectionCallback)
        {
            ClientCertificate = clientCertificate
                ?? throw new ArgumentException("No certificate was found. To use certificate authentication certificate must be present.", nameof(clientCertificate));

            CertificateChain = certificateChain;
            IssuedCertificatePrivateKey = issuedCertificatePrivateKey;
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
        /// The private key that this device's certificate signing requests are generated with, if any.
        /// </summary>
        /// <remarks>
        /// When set, and a client certificate has been issued to this device, the issued device certificate is paired
        /// with this key and presented to IoT Hub instead of <see cref="ClientCertificate"/>. Every certificate signing
        /// request this device sends must be generated with this key; to rotate keys, create a new provider.
        /// <para>
        /// On platforms whose TLS stack cannot use an in-memory key (such as Windows SChannel), the paired certificate is
        /// round-tripped through an in-memory PKCS#12 blob. If the key cannot be exported (for example an HSM-backed key),
        /// the paired certificate is presented as-is.
        /// </para>
        /// </remarks>
        public AsymmetricAlgorithm? IssuedCertificatePrivateKey { get; }

        private readonly IssuedCertificateCache _issuedCertificateCache = new();

        public override AuthenticationCredentials GetIotHubAuthentication(string hostName, string deviceId, IReadOnlyList<string>? issuedClientCertificates)
        {
            AsymmetricAlgorithm? privateKey = IssuedCertificatePrivateKey;
            if (issuedClientCertificates == null || issuedClientCertificates.Count == 0)
            {
                return CreateCredentials(ClientCertificate);
            }

            if (privateKey == null)
            {
                Trace.TraceWarning("A client certificate was issued to this device, but this provider was created without an issuedCertificatePrivateKey, so the original client certificate will be presented to IoT Hub instead.");
                return CreateCredentials(ClientCertificate);
            }

            return CreateCredentials(_issuedCertificateCache.GetOrCreate(issuedClientCertificates, privateKey));
        }

        public override AuthenticationCredentials GetDeviceProvisioningServiceAuthentication()
        {
            return CreateCredentials(ClientCertificate);
        }

        private AuthenticationCredentials CreateCredentials(X509Certificate2 clientCertificate)
        {
            return new AuthenticationCredentials()
            {
                ClientCertificate = clientCertificate,
                RemoteCertificateValidationCallback = RemoteCertificateValidationCallback,
                LocalCertificateSelectionCallback = LocalCertificateSelectionCallback,
            };
        }

        public override string GetRegistrationId()
        {
            return ClientCertificate.GetNameInfo(X509NameType.DnsName, false);
        }
    }
}
