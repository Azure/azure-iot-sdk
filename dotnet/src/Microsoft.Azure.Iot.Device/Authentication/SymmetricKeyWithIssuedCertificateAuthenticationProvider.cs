// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using System.Diagnostics;
using System.Net.Security;
using System.Security.Cryptography;

namespace Microsoft.Azure.Iot.Device.Authentication
{
    /// <summary>
    /// Device authentication for a device that provisions with a symmetric key and requests a client certificate
    /// during provisioning (or later from IoT Hub), then authenticates with IoT Hub using that issued certificate.
    /// </summary>
    /// <remarks>
    /// The device provisioning service is always authenticated with a SAS token derived from the symmetric key. IoT Hub
    /// is authenticated with the most recently issued client certificate, paired with
    /// <see cref="IssuedCertificatePrivateKey"/>. Until a certificate has been issued, IoT Hub is authenticated with a
    /// SAS token derived from the symmetric key.
    /// <para>
    /// On platforms whose TLS stack cannot use an in-memory key (such as Windows SChannel), the paired certificate is
    /// round-tripped through an in-memory PKCS#12 blob. If the key cannot be exported (for example an HSM-backed key),
    /// the paired certificate is presented as-is.
    /// </para>
    /// </remarks>
    public class SymmetricKeyWithIssuedCertificateAuthenticationProvider : SymmetricKeyAuthenticationProvider
    {
        private readonly IssuedCertificateCache _issuedCertificateCache = new();
        private volatile AsymmetricAlgorithm _issuedCertificatePrivateKey;

        /// <summary>
        /// Creates an instance of this class.
        /// </summary>
        /// <param name="registrationId">The registration Id to provision as.</param>
        /// <param name="idScope">The device provisioning service Id scope.</param>
        /// <param name="symmetricKey">The Base64-encoded symmetric key used to authenticate with the device provisioning service.</param>
        /// <param name="issuedCertificatePrivateKey">
        /// The private key that the certificate signing request sent during provisioning was generated with.
        /// </param>
        /// <param name="remoteCertificateValidationCallback">
        /// An optional callback used to perform custom validation of the server (remote) certificate during the TLS handshake.
        /// When null, the platform's default validation is used.
        /// </param>
        /// <param name="localCertificateSelectionCallback">
        /// An optional callback used to select which client certificate to present during the TLS handshake with IoT Hub.
        /// When null, the issued certificate is presented.
        /// </param>
        public SymmetricKeyWithIssuedCertificateAuthenticationProvider(
            string registrationId,
            string idScope,
            string symmetricKey,
            AsymmetricAlgorithm issuedCertificatePrivateKey,
            RemoteCertificateValidationCallback? remoteCertificateValidationCallback = null,
            LocalCertificateSelectionCallback? localCertificateSelectionCallback = null)
            : base(registrationId, idScope, symmetricKey, remoteCertificateValidationCallback, localCertificateSelectionCallback)
        {
            _issuedCertificatePrivateKey = issuedCertificatePrivateKey ?? throw new ArgumentNullException(nameof(issuedCertificatePrivateKey));
        }

        /// <summary>
        /// The private key that the most recent certificate signing request was generated with. Update this before
        /// sending a certificate signing request to IoT Hub that was generated with a different key.
        /// </summary>
        public AsymmetricAlgorithm IssuedCertificatePrivateKey
        {
            get => _issuedCertificatePrivateKey;
            set => _issuedCertificatePrivateKey = value ?? throw new ArgumentNullException(nameof(value));
        }

        public override AuthenticationCredentials GetIotHubAuthentication(string hostName, string deviceId, IReadOnlyList<string>? issuedClientCertificates)
        {
            if (issuedClientCertificates == null || issuedClientCertificates.Count == 0)
            {
                Trace.TraceInformation("No client certificate has been issued to this device yet, so it will authenticate with IoT Hub using its symmetric key.");
                return base.GetIotHubAuthentication(hostName, deviceId, issuedClientCertificates);
            }

            return new AuthenticationCredentials()
            {
                ClientCertificate = _issuedCertificateCache.GetOrCreate(issuedClientCertificates, IssuedCertificatePrivateKey),
                RemoteCertificateValidationCallback = RemoteCertificateValidationCallback,
                LocalCertificateSelectionCallback = LocalCertificateSelectionCallback,
            };
        }
    }
}
