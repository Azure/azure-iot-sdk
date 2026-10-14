// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using System.Diagnostics;
using System.Net.Security;
using System.Security.Authentication;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;

namespace Microsoft.Azure.Iot.Device.Authentication.Operational
{
    /// <summary>
    /// IoT Hub operational authentication that only ever presents client certificates issued to this device, either by
    /// the device provisioning service during provisioning or by IoT Hub.
    /// </summary>
    /// <remarks>
    /// A certificate must be issued by DPS or restored in the connection context before connecting to IoT Hub. This
    /// provider has no original certificate to fall back to: if no client certificate has been issued,
    /// <see cref="GetCredentialsAsync"/> throws an <see cref="AuthenticationException"/>. It also never falls back to
    /// boot credentials.
    /// </remarks>
    public class IssuedCertificateOperationalAuthenticationProvider : OperationalAuthenticationProvider
    {
        private readonly IssuedCertificateHandler _issuedCertificateHandler;

        /// <summary>
        /// Creates a provider that pairs each issued device certificate with the private key that this device's
        /// certificate signing requests are generated with.
        /// </summary>
        /// <param name="issuedCertificatePrivateKey">
        /// The private key that this device's certificate signing requests are generated with. Every certificate signing
        /// request this device sends must be generated with this key; to rotate keys, create a new provider.
        /// </param>
        /// <param name="remoteCertificateValidationCallback">
        /// An optional callback used to perform custom validation of the server (remote) certificate during the TLS handshake.
        /// When null, the platform's default validation is used.
        /// </param>
        /// <param name="localCertificateSelectionCallback">
        /// An optional callback used to select which client certificate to present during the TLS handshake.
        /// </param>
        public IssuedCertificateOperationalAuthenticationProvider(
            AsymmetricAlgorithm issuedCertificatePrivateKey,
            RemoteCertificateValidationCallback? remoteCertificateValidationCallback = null,
            LocalCertificateSelectionCallback? localCertificateSelectionCallback = null)
            : base(remoteCertificateValidationCallback, localCertificateSelectionCallback)
        {
            IssuedCertificatePrivateKey = issuedCertificatePrivateKey ?? throw new ArgumentNullException(nameof(issuedCertificatePrivateKey));
            HandleCertificateSigningCompleteAsync = IssuedCertificateHandler.CreateDefaultCallback(issuedCertificatePrivateKey);
            _issuedCertificateHandler = new IssuedCertificateHandler(HandleCertificateSigningCompleteAsync);
        }

        /// <summary>
        /// Creates a provider that delegates turning each issued client certificate chain into a certificate to a callback.
        /// </summary>
        /// <param name="handleCertificateSigningCompleteAsync">
        /// Receives the issued chain (Base64-encoded, device certificate first) and returns the certificate, with its
        /// private key, to present to IoT Hub, so it can apply custom logic such as persisting the certificate or pairing
        /// it with an HSM-backed key. It is invoked from <see cref="GetCredentialsAsync"/> once per issued chain. If it
        /// throws during provisioning, provisioning fails; if it throws for a certificate signing request sent to IoT Hub,
        /// that certificate signing operation fails.
        /// </param>
        /// <param name="remoteCertificateValidationCallback">
        /// An optional callback used to perform custom validation of the server (remote) certificate during the TLS handshake.
        /// When null, the platform's default validation is used.
        /// </param>
        /// <param name="localCertificateSelectionCallback">
        /// An optional callback used to select which client certificate to present during the TLS handshake.
        /// </param>
        public IssuedCertificateOperationalAuthenticationProvider(
            Func<IReadOnlyList<string>, Task<X509Certificate2>> handleCertificateSigningCompleteAsync,
            RemoteCertificateValidationCallback? remoteCertificateValidationCallback = null,
            LocalCertificateSelectionCallback? localCertificateSelectionCallback = null)
            : base(remoteCertificateValidationCallback, localCertificateSelectionCallback)
        {
            HandleCertificateSigningCompleteAsync = handleCertificateSigningCompleteAsync
                ?? throw new ArgumentNullException(nameof(handleCertificateSigningCompleteAsync));
            _issuedCertificateHandler = new IssuedCertificateHandler(handleCertificateSigningCompleteAsync);
        }

        /// <summary>
        /// The private key that this device's certificate signing requests are generated with, or null if this provider
        /// was created with a certificate signing complete callback.
        /// </summary>
        /// <remarks>
        /// On platforms whose TLS stack cannot use an in-memory key (such as Windows SChannel), the paired certificate is
        /// round-tripped through an in-memory PKCS#12 blob. If the key cannot be exported (for example an HSM-backed key),
        /// the paired certificate is presented as-is.
        /// </remarks>
        public AsymmetricAlgorithm? IssuedCertificatePrivateKey { get; }

        /// <summary>
        /// Produces the certificate to present to IoT Hub from an issued client certificate chain. Either the callback
        /// this provider was created with or one that pairs the issued device certificate with
        /// <see cref="IssuedCertificatePrivateKey"/>.
        /// </summary>
        public Func<IReadOnlyList<string>, Task<X509Certificate2>> HandleCertificateSigningCompleteAsync { get; }

        /// <inheritdoc/>
        /// <remarks>
        /// The certificate to present is produced from <paramref name="issuedClientCertificates"/> by
        /// <see cref="HandleCertificateSigningCompleteAsync"/>, which is invoked only the first time each chain is seen.
        /// </remarks>
        /// <exception cref="AuthenticationException">No client certificate has been issued to this device.</exception>
        public override Task<AuthenticationCredentials> GetCredentialsAsync(
            string hostName,
            string deviceId,
            IReadOnlyList<string>? issuedClientCertificates,
            CancellationToken cancellationToken = default)
        {
            Trace.TraceWarning("IssuedCertificateOperationalAuthenticationProvider was not given any issued client certificates."); //TODO maybe a way to make API prevent this? Or throw at ProvisionAndConnectAsync start time
            return _issuedCertificateHandler.GetCredentialsAsync(
                issuedClientCertificates,
                fallbackCertificate: null,
                RemoteCertificateValidationCallback,
                LocalCertificateSelectionCallback,
                cancellationToken);
        }
    }
}
