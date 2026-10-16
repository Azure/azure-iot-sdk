// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using System.Diagnostics;
using System.Net.Security;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;

namespace Microsoft.Azure.Iot.Device.Authentication
{
    /// <summary>
    /// Supplies the authentication material a device uses with both Device Provisioning Service and IoT Hub, and
    /// optionally turns client certificates issued to the device (by Device Provisioning Service or IoT Hub) into the
    /// certificate it presents to IoT Hub.
    /// </summary>
    /// <remarks>
    /// A provider can only use issued client certificates when it was created with either the private key that this
    /// device's certificate signing requests are generated with or a callback that turns an issued certificate chain into
    /// an <see cref="X509Certificate2"/>. <see cref="HasCertificateSigningHandler"/> reports whether it was, and certificate
    /// signing requests can only be sent alongside a provider for which it is <c>true</c>.
    /// </remarks>
    public abstract class AuthenticationProvider
    {
        private readonly IssuedCertificateHandler? _issuedCertificateHandler;

        /// <summary>
        /// Initializes a provider that cannot use issued client certificates.
        /// </summary>
        /// <param name="remoteCertificateValidationCallback">
        /// An optional callback used to perform custom validation of the server (remote) certificate during the TLS handshake.
        /// When null, the platform's default validation is used.
        /// </param>
        /// <param name="localCertificateSelectionCallback">
        /// An optional callback used to select which client certificate to present during the TLS handshake.
        /// </param>
        protected AuthenticationProvider(
            RemoteCertificateValidationCallback? remoteCertificateValidationCallback = null,
            LocalCertificateSelectionCallback? localCertificateSelectionCallback = null)
        {
            RemoteCertificateValidationCallback = remoteCertificateValidationCallback;
            LocalCertificateSelectionCallback = localCertificateSelectionCallback;
        }

        /// <summary>
        /// Initializes a provider that pairs each issued device certificate with the private key that this device's
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
        protected AuthenticationProvider(
            AsymmetricAlgorithm issuedCertificatePrivateKey,
            RemoteCertificateValidationCallback? remoteCertificateValidationCallback = null,
            LocalCertificateSelectionCallback? localCertificateSelectionCallback = null)
            : this(remoteCertificateValidationCallback, localCertificateSelectionCallback)
        {
            IssuedCertificatePrivateKey = issuedCertificatePrivateKey ?? throw new ArgumentNullException(nameof(issuedCertificatePrivateKey));
            HandleCertificateSigningCompleteAsync = IssuedCertificateHandler.CreateDefaultCallback(issuedCertificatePrivateKey);
            _issuedCertificateHandler = new IssuedCertificateHandler(HandleCertificateSigningCompleteAsync);
        }

        /// <summary>
        /// Initializes a provider that delegates turning each issued client certificate chain into a certificate to a callback.
        /// </summary>
        /// <param name="handleCertificateSigningCompleteAsync">
        /// Invoked when a client certificate chain is issued to this device, either by Device Provisioning Service during
        /// provisioning or by IoT Hub. It receives the issued chain (Base64-encoded, device certificate first) and returns
        /// the certificate, with its private key, to present to IoT Hub, so it can apply custom logic such as persisting
        /// the certificate or pairing it with an HSM-backed key. It is invoked from <see cref="GetIotHubCredentialsAsync"/>
        /// once per issued chain. If it throws during provisioning, provisioning fails; if it throws for a certificate
        /// signing request sent to IoT Hub, that certificate signing operation fails.
        /// </param>
        /// <param name="remoteCertificateValidationCallback">
        /// An optional callback used to perform custom validation of the server (remote) certificate during the TLS handshake.
        /// When null, the platform's default validation is used.
        /// </param>
        /// <param name="localCertificateSelectionCallback">
        /// An optional callback used to select which client certificate to present during the TLS handshake.
        /// </param>
        protected AuthenticationProvider(
            Func<IReadOnlyList<string>, Task<X509Certificate2>> handleCertificateSigningCompleteAsync,
            RemoteCertificateValidationCallback? remoteCertificateValidationCallback = null,
            LocalCertificateSelectionCallback? localCertificateSelectionCallback = null)
            : this(remoteCertificateValidationCallback, localCertificateSelectionCallback)
        {
            HandleCertificateSigningCompleteAsync = handleCertificateSigningCompleteAsync
                ?? throw new ArgumentNullException(nameof(handleCertificateSigningCompleteAsync));
            _issuedCertificateHandler = new IssuedCertificateHandler(handleCertificateSigningCompleteAsync);
        }

        /// <summary>
        /// True if this provider was created with a private key or callback that turns issued client certificates into an
        /// <see cref="X509Certificate2"/>, and so can use certificates issued for this device's certificate signing requests.
        /// </summary>
        public bool HasCertificateSigningHandler => HandleCertificateSigningCompleteAsync != null;

        /// <summary>
        /// The private key that this device's certificate signing requests are generated with, or null if this provider
        /// was created with a certificate signing complete callback or without certificate signing handling.
        /// </summary>
        /// <remarks>
        /// On platforms whose TLS stack cannot use an in-memory key (such as Windows SChannel), the paired certificate is
        /// round-tripped through an in-memory PKCS#12 blob. If the key cannot be exported (for example an HSM-backed key),
        /// the paired certificate is presented as-is.
        /// </remarks>
        public AsymmetricAlgorithm? IssuedCertificatePrivateKey { get; }

        /// <summary>
        /// Produces the certificate to present to IoT Hub from an issued client certificate chain: either the callback
        /// this provider was created with or one that pairs the issued device certificate with
        /// <see cref="IssuedCertificatePrivateKey"/>. Null if <see cref="HasCertificateSigningHandler"/> is <c>false</c>.
        /// </summary>
        public Func<IReadOnlyList<string>, Task<X509Certificate2>>? HandleCertificateSigningCompleteAsync { get; }

        /// <summary>
        /// An optional callback for validating the remote TLS certificate. Null uses platform validation.
        /// </summary>
        public RemoteCertificateValidationCallback? RemoteCertificateValidationCallback { get; }

        /// <summary>
        /// An optional callback for selecting the client TLS certificate.
        /// Null presents the certificate returned in the authentication credentials.
        /// </summary>
        public LocalCertificateSelectionCallback? LocalCertificateSelectionCallback { get; }

        /// <summary>
        /// Gets the Device Provisioning Service registration Id.
        /// </summary>
        public abstract string GetRegistrationId();

        /// <summary>
        /// Gets the protocol-agnostic authentication material (client certificate, password/token, and TLS callbacks)
        /// for connecting to Device Provisioning Service. Issued client certificates are never used with Device
        /// Provisioning Service.
        /// </summary>
        /// <param name="cancellationToken">The cancellation token.</param>
        public abstract Task<AuthenticationCredentials> GetProvisioningCredentialsAsync(CancellationToken cancellationToken = default);

        /// <summary>
        /// Gets protocol-agnostic authentication material for connecting to IoT Hub.
        /// The caller maps it onto its own transport (MQTT, HTTP, etc.).
        /// </summary>
        /// <param name="hostName">The assigned IoT Hub host name.</param>
        /// <param name="deviceId">The assigned device Id.</param>
        /// <param name="issuedClientCertificates">
        /// The most recently issued certificate chain from DPS or IoT Hub, Base64-encoded and device certificate
        /// first, followed by intermediates. Null if no certificate has been issued.
        /// </param>
        /// <param name="cancellationToken">The cancellation token.</param>
        /// <remarks>
        /// Called before each connection attempt to IoT Hub, always with the latest issued certificate chain.
        /// Implementations can use <see cref="GetIssuedCertificateAsync"/> to turn that chain into the certificate to present.
        /// </remarks>
        public abstract Task<AuthenticationCredentials> GetIotHubCredentialsAsync(
            string hostName,
            string deviceId,
            IReadOnlyList<string>? issuedClientCertificates,
            CancellationToken cancellationToken = default);

        /// <summary>
        /// Gets the certificate to present to IoT Hub for <paramref name="issuedClientCertificates"/>, invoking
        /// <see cref="HandleCertificateSigningCompleteAsync"/> only the first time each chain is seen.
        /// </summary>
        /// <returns>
        /// The certificate to present, or null if no certificate has been issued or this provider has no certificate
        /// signing handler (in which case the issued certificate is ignored).
        /// </returns>
        protected async Task<X509Certificate2?> GetIssuedCertificateAsync(
            IReadOnlyList<string>? issuedClientCertificates,
            CancellationToken cancellationToken)
        {
            if (issuedClientCertificates is not { Count: > 0 })
            {
                return null;
            }

            if (_issuedCertificateHandler == null)
            {
                Trace.TraceWarning("A client certificate was issued to this device, but this authentication provider was created without a private key or certificate signing complete callback, so it cannot present it.");
                return null;
            }

            return await _issuedCertificateHandler.GetCertificateAsync(issuedClientCertificates, cancellationToken).ConfigureAwait(false);
        }
    }
}
