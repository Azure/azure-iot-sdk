// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using System.Diagnostics;
using System.Net.Security;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text;

namespace Microsoft.Azure.Iot.Device.Authentication.Operational
{
    /// <summary>
    /// IoT Hub operational authentication using a device-scoped symmetric key, optionally switching to X509
    /// authentication once a client certificate is issued to this device.
    /// </summary>
    public class SymmetricKeyOperationalAuthenticationProvider : OperationalAuthenticationProvider
    {
        private readonly string _symmetricKey;
        private readonly IssuedCertificateHandler? _issuedCertificateHandler;

        /// <summary>
        /// Creates an operational provider using the Base64-encoded IoT Hub device key.
        /// The assigned hostname and device Id are supplied on each connection attempt.
        /// This provider always authenticates with the symmetric key, even if a client certificate is issued to this
        /// device. To present issued certificates, use a constructor that takes the certificate signing request's
        /// private key or a certificate signing complete callback.
        /// </summary>
        /// <param name="symmetricKey">The Base64-encoded IoT Hub device key.</param>
        /// <param name="remoteCertificateValidationCallback">
        /// An optional callback used to perform custom validation of the server (remote) certificate during the TLS handshake.
        /// When null, the platform's default validation is used.
        /// </param>
        /// <param name="localCertificateSelectionCallback">
        /// An optional callback used to select which client certificate to present during the TLS handshake.
        /// </param>
        public SymmetricKeyOperationalAuthenticationProvider(
            string symmetricKey,
            RemoteCertificateValidationCallback? remoteCertificateValidationCallback = null,
            LocalCertificateSelectionCallback? localCertificateSelectionCallback = null)
            : base(remoteCertificateValidationCallback, localCertificateSelectionCallback)
        {
            ArgumentException.ThrowIfNullOrEmpty(symmetricKey);
            _symmetricKey = symmetricKey;
        }

        /// <summary>
        /// Creates an operational provider that authenticates with the symmetric key until a client certificate is issued
        /// to this device, then presents the issued certificate paired with the private key its certificate signing
        /// request was generated with.
        /// </summary>
        /// <param name="symmetricKey">
        /// The Base64-encoded IoT Hub device key, used until a client certificate is issued to this device.
        /// </param>
        /// <param name="issuedCertificatePrivateKey">
        /// The private key that this device's certificate signing requests are generated with. When a client certificate
        /// is issued to this device (either by the device provisioning service during provisioning or by IoT Hub), the
        /// issued device certificate is paired with this key and presented to IoT Hub instead of a SAS token.
        /// </param>
        /// <param name="remoteCertificateValidationCallback">
        /// An optional callback used to perform custom validation of the server (remote) certificate during the TLS handshake.
        /// When null, the platform's default validation is used.
        /// </param>
        /// <param name="localCertificateSelectionCallback">
        /// An optional callback used to select which client certificate to present during the TLS handshake.
        /// </param>
        public SymmetricKeyOperationalAuthenticationProvider(
            string symmetricKey,
            AsymmetricAlgorithm issuedCertificatePrivateKey,
            RemoteCertificateValidationCallback? remoteCertificateValidationCallback = null,
            LocalCertificateSelectionCallback? localCertificateSelectionCallback = null)
            : base(remoteCertificateValidationCallback, localCertificateSelectionCallback)
        {
            ArgumentException.ThrowIfNullOrEmpty(symmetricKey);
            _symmetricKey = symmetricKey;
            IssuedCertificatePrivateKey = issuedCertificatePrivateKey ?? throw new ArgumentNullException(nameof(issuedCertificatePrivateKey));
            HandleCertificateSigningCompleteAsync = IssuedCertificateHandler.CreateDefaultCallback(issuedCertificatePrivateKey);
            _issuedCertificateHandler = new IssuedCertificateHandler(HandleCertificateSigningCompleteAsync);
        }

        /// <summary>
        /// Creates an operational provider that authenticates with the symmetric key until a client certificate is issued
        /// to this device, then delegates turning the issued chain into a certificate to a callback.
        /// </summary>
        /// <param name="symmetricKey">
        /// The Base64-encoded IoT Hub device key, used until a client certificate is issued to this device.
        /// </param>
        /// <param name="handleCertificateSigningCompleteAsync">
        /// Invoked when a client certificate chain is issued to this device, either by the device provisioning service
        /// during provisioning or by IoT Hub. It receives the issued chain (Base64-encoded, device certificate first) and
        /// returns the certificate, with its private key, to present to IoT Hub, so it can apply custom logic such as
        /// persisting the certificate or pairing it with an HSM-backed key. It is invoked from
        /// <see cref="GetCredentialsAsync"/> once per issued chain. If it throws during provisioning, provisioning fails; if
        /// it throws for a certificate signing request sent to IoT Hub, that certificate signing operation fails.
        /// </param>
        /// <param name="remoteCertificateValidationCallback">
        /// An optional callback used to perform custom validation of the server (remote) certificate during the TLS handshake.
        /// When null, the platform's default validation is used.
        /// </param>
        /// <param name="localCertificateSelectionCallback">
        /// An optional callback used to select which client certificate to present during the TLS handshake.
        /// </param>
        public SymmetricKeyOperationalAuthenticationProvider(
            string symmetricKey,
            Func<IReadOnlyList<string>, Task<X509Certificate2>> handleCertificateSigningCompleteAsync,
            RemoteCertificateValidationCallback? remoteCertificateValidationCallback = null,
            LocalCertificateSelectionCallback? localCertificateSelectionCallback = null)
            : base(remoteCertificateValidationCallback, localCertificateSelectionCallback)
        {
            ArgumentException.ThrowIfNullOrEmpty(symmetricKey);
            _symmetricKey = symmetricKey;
            HandleCertificateSigningCompleteAsync = handleCertificateSigningCompleteAsync
                ?? throw new ArgumentNullException(nameof(handleCertificateSigningCompleteAsync));
            _issuedCertificateHandler = new IssuedCertificateHandler(handleCertificateSigningCompleteAsync);
        }

        /// <summary>
        /// The private key that this device's certificate signing requests are generated with, if any.
        /// </summary>
        /// <remarks>
        /// When set, and a client certificate has been issued to this device, the issued device certificate is paired
        /// with this key and presented to IoT Hub instead of a SAS token. Every certificate signing request this device
        /// sends must be generated with this key; to rotate keys, create a new provider.
        /// <para>
        /// On platforms whose TLS stack cannot use an in-memory key (such as Windows SChannel), the paired certificate is
        /// round-tripped through an in-memory PKCS#12 blob. If the key cannot be exported (for example an HSM-backed key),
        /// the paired certificate is presented as-is.
        /// </para>
        /// </remarks>
        public AsymmetricAlgorithm? IssuedCertificatePrivateKey { get; }

        /// <summary>
        /// Produces the certificate to present to IoT Hub from an issued client certificate chain, or null if this
        /// provider only authenticates with the symmetric key. Either the callback this provider was created with or one
        /// that pairs the issued device certificate with <see cref="IssuedCertificatePrivateKey"/>.
        /// </summary>
        public Func<IReadOnlyList<string>, Task<X509Certificate2>>? HandleCertificateSigningCompleteAsync { get; }

        /// <inheritdoc/>
        /// <remarks>
        /// When <paramref name="issuedClientCertificates"/> is non-empty and this provider was created with an issued
        /// certificate private key or certificate signing complete callback, X509 credentials are returned using the
        /// certificate produced by <see cref="HandleCertificateSigningCompleteAsync"/>, which is invoked only the first
        /// time each chain is seen. Otherwise, SAS token credentials derived from the symmetric key are returned.
        /// </remarks>
        public override Task<AuthenticationCredentials> GetCredentialsAsync(
            string hostName,
            string deviceId,
            IReadOnlyList<string>? issuedClientCertificates,
            CancellationToken cancellationToken = default)
        {
            cancellationToken.ThrowIfCancellationRequested();

            if (issuedClientCertificates is { Count: > 0 })
            {
                if (_issuedCertificateHandler != null)
                {
                    return _issuedCertificateHandler.GetCredentialsAsync(
                        issuedClientCertificates,
                        fallbackCertificate: null,
                        RemoteCertificateValidationCallback,
                        LocalCertificateSelectionCallback,
                        cancellationToken);
                }

                Trace.TraceWarning("A client certificate was issued to this device, but this provider was created without an issuedCertificatePrivateKey or handleCertificateSigningCompleteAsync callback, so symmetric key authentication will be used with IoT Hub instead.");
            }

            string token = SasTokenBuilder.BuildSasToken(
                string.Empty, _symmetricKey, $"{hostName}/devices/{deviceId}", TimeSpan.FromHours(1));
            return Task.FromResult(new AuthenticationCredentials()
            {
                Password = Encoding.UTF8.GetBytes(token),
                RemoteCertificateValidationCallback = RemoteCertificateValidationCallback,
                LocalCertificateSelectionCallback = LocalCertificateSelectionCallback,
            });
        }
    }
}
