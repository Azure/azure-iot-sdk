// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using System.Diagnostics;
using System.Net.Security;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;

namespace Microsoft.Azure.Iot.Device.Authentication
{
    /// <summary>
    /// Device authentication for a device that provisions with a symmetric key and requests a client certificate
    /// during provisioning (or later from IoT Hub), then authenticates with IoT Hub using that issued certificate.
    /// </summary>
    /// <remarks>
    /// The device provisioning service is always authenticated with a SAS token derived from the symmetric key. IoT Hub
    /// is authenticated with the most recently issued client certificate, either paired with
    /// <see cref="IssuedCertificatePrivateKey"/> or as returned by <see cref="HandleCertificateSigningCompleteAsync"/>.
    /// Until such a certificate is available, IoT Hub is authenticated with a SAS token derived from the symmetric key.
    /// <para>
    /// When pairing with <see cref="IssuedCertificatePrivateKey"/> on platforms whose TLS stack cannot use an in-memory
    /// key (such as Windows SChannel), the paired certificate is round-tripped through an in-memory PKCS#12 blob. If the
    /// key cannot be exported (for example an HSM-backed key), the paired certificate is presented as-is.
    /// </para>
    /// </remarks>
    public class SymmetricKeyWithIssuedCertificateAuthenticationProvider : SymmetricKeyAuthenticationProvider
    {
        private readonly IssuedCertificateHandler _issuedCertificateHandler;

        /// <summary>
        /// Creates an instance of this class.
        /// </summary>
        /// <param name="registrationId">The registration Id to provision as.</param>
        /// <param name="idScope">The device provisioning service Id scope.</param>
        /// <param name="symmetricKey">The Base64-encoded symmetric key used to authenticate with the device provisioning service.</param>
        /// <param name="issuedCertificatePrivateKey">
        /// The private key that this device's certificate signing requests (during provisioning and with IoT Hub) are generated with.
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
            IssuedCertificatePrivateKey = issuedCertificatePrivateKey ?? throw new ArgumentNullException(nameof(issuedCertificatePrivateKey));
            HandleCertificateSigningCompleteAsync = IssuedCertificateHandler.CreateDefaultCallback(issuedCertificatePrivateKey);
            _issuedCertificateHandler = new IssuedCertificateHandler(HandleCertificateSigningCompleteAsync);
        }

        /// <summary>
        /// Creates an instance of this class that delegates handling of issued client certificates to a callback.
        /// </summary>
        /// <param name="registrationId">The registration Id to provision as.</param>
        /// <param name="idScope">The device provisioning service Id scope.</param>
        /// <param name="symmetricKey">The Base64-encoded symmetric key used to authenticate with the device provisioning service.</param>
        /// <param name="handleCertificateSigningCompleteAsync">
        /// Invoked when a client certificate chain is issued to this device, either by the device provisioning service
        /// during provisioning or by IoT Hub. It receives the issued chain (Base64-encoded, device certificate first) and
        /// returns the certificate, with its private key, to present to IoT Hub, so it can apply custom logic such as
        /// persisting the certificate or pairing it with an HSM-backed key. It is invoked once per issued chain.
        /// If it throws during provisioning, provisioning fails; if it throws for a certificate signing request sent to
        /// IoT Hub, that certificate signing operation fails.
        /// </param>
        /// <param name="remoteCertificateValidationCallback">
        /// An optional callback used to perform custom validation of the server (remote) certificate during the TLS handshake.
        /// When null, the platform's default validation is used.
        /// </param>
        /// <param name="localCertificateSelectionCallback">
        /// An optional callback used to select which client certificate to present during the TLS handshake with IoT Hub.
        /// When null, the certificate returned by <paramref name="handleCertificateSigningCompleteAsync"/> is presented.
        /// </param>
        public SymmetricKeyWithIssuedCertificateAuthenticationProvider(
            string registrationId,
            string idScope,
            string symmetricKey,
            Func<IReadOnlyList<string>, Task<X509Certificate2>> handleCertificateSigningCompleteAsync,
            RemoteCertificateValidationCallback? remoteCertificateValidationCallback = null,
            LocalCertificateSelectionCallback? localCertificateSelectionCallback = null)
            : base(registrationId, idScope, symmetricKey, remoteCertificateValidationCallback, localCertificateSelectionCallback)
        {
            HandleCertificateSigningCompleteAsync = handleCertificateSigningCompleteAsync
                ?? throw new ArgumentNullException(nameof(handleCertificateSigningCompleteAsync));
            _issuedCertificateHandler = new IssuedCertificateHandler(handleCertificateSigningCompleteAsync);
        }

        /// <summary>
        /// The private key that this device's certificate signing requests are generated with, if this provider was
        /// created with one. Every certificate signing request this device sends must be generated with this key; to
        /// rotate keys, create a new provider.
        /// </summary>
        public AsymmetricAlgorithm? IssuedCertificatePrivateKey { get; }

        /// <summary>
        /// Produces the certificate to present to IoT Hub from an issued client certificate chain. Either the callback
        /// this provider was created with or, by default, one that pairs the issued device certificate with
        /// <see cref="IssuedCertificatePrivateKey"/>.
        /// </summary>
        public Func<IReadOnlyList<string>, Task<X509Certificate2>> HandleCertificateSigningCompleteAsync { get; }

        public override Task HandleClientCertificatesIssuedAsync(IReadOnlyList<string> issuedClientCertificates)
        {
            return _issuedCertificateHandler.HandleAsync(issuedClientCertificates);
        }

        public override AuthenticationCredentials GetOperationalAuthenticationCredentials(string hostName, string deviceId, IReadOnlyList<string>? issuedClientCertificates)
        {
            X509Certificate2? issuedCertificate = issuedClientCertificates == null || issuedClientCertificates.Count == 0
                ? null
                : _issuedCertificateHandler.GetCertificate(issuedClientCertificates);

            if (issuedCertificate == null)
            {
                Trace.TraceInformation("No issued client certificate is available for this device yet, so it will authenticate with IoT Hub using its symmetric key.");
                return base.GetOperationalAuthenticationCredentials(hostName, deviceId, issuedClientCertificates);
            }

            return new AuthenticationCredentials()
            {
                ClientCertificate = issuedCertificate,
                RemoteCertificateValidationCallback = RemoteCertificateValidationCallback,
                LocalCertificateSelectionCallback = LocalCertificateSelectionCallback,
            };
        }
    }
}
