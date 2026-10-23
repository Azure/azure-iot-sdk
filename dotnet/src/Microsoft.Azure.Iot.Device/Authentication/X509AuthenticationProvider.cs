// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

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
        /// Creates an instance of this class that always presents <paramref name="clientCertificate"/>, even if a client
        /// certificate is issued to this device. To present issued certificates, use a constructor that takes the
        /// certificate signing request's private key or a certificate signing complete callback.
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
            LocalCertificateSelectionCallback? localCertificateSelectionCallback = null) : base(remoteCertificateValidationCallback, localCertificateSelectionCallback)
        {
            ClientCertificate = clientCertificate
                ?? throw new ArgumentException("No certificate was found. To use certificate authentication certificate must be present.", nameof(clientCertificate));

            CertificateChain = certificateChain;
            HandleCertificateSigningCompleteAsync = PresentOriginalCertificateAsync;
            _issuedCertificateHandler = new IssuedCertificateHandler(HandleCertificateSigningCompleteAsync);
        }

        /// <summary>
        /// Creates an instance of this class that presents client certificates issued to this device, paired with the
        /// private key their certificate signing requests were generated with, to IoT Hub.
        /// </summary>
        /// <param name="clientCertificate">
        /// The client certificate used to authenticate with the device provisioning service, and with IoT Hub until a
        /// client certificate is issued to this device.
        /// </param>
        /// <param name="issuedCertificatePrivateKey">
        /// The private key that this device's certificate signing requests are generated with. When a client certificate
        /// is issued to this device (either by the device provisioning service during provisioning or by IoT Hub), the
        /// issued device certificate is paired with this key and presented to IoT Hub instead of <paramref name="clientCertificate"/>.
        /// </param>
        /// <param name="certificateChain">
        /// The certificate chain leading to the root certificate uploaded to the device provisioning service.
        /// </param>
        /// <param name="remoteCertificateValidationCallback">
        /// An optional callback used to perform custom validation of the server (remote) certificate during the TLS handshake.
        /// When null, the platform's default validation is used.
        /// </param>
        /// <param name="localCertificateSelectionCallback">
        /// An optional callback used to select which client certificate to present during the TLS handshake.
        /// </param>
        public X509AuthenticationProvider(
            X509Certificate2 clientCertificate,
            AsymmetricAlgorithm issuedCertificatePrivateKey,
            X509Certificate2Collection? certificateChain = null,
            RemoteCertificateValidationCallback? remoteCertificateValidationCallback = null,
            LocalCertificateSelectionCallback? localCertificateSelectionCallback = null) : base(remoteCertificateValidationCallback, localCertificateSelectionCallback)
        {
            ClientCertificate = clientCertificate
                ?? throw new ArgumentException("No certificate was found. To use certificate authentication certificate must be present.", nameof(clientCertificate));

            CertificateChain = certificateChain;
            IssuedCertificatePrivateKey = issuedCertificatePrivateKey ?? throw new ArgumentNullException(nameof(issuedCertificatePrivateKey));
            HandleCertificateSigningCompleteAsync = IssuedCertificateHandler.CreateDefaultCallback(issuedCertificatePrivateKey);
            _issuedCertificateHandler = new IssuedCertificateHandler(HandleCertificateSigningCompleteAsync);
        }

        /// <summary>
        /// Creates an instance of this class that delegates handling of issued client certificates to a callback.
        /// </summary>
        /// <param name="clientCertificate">
        /// The client certificate used to authenticate with the device provisioning service, and with IoT Hub until a
        /// client certificate is issued to this device.
        /// </param>
        /// <param name="handleCertificateSigningCompleteAsync">
        /// Invoked before connecting to IoT Hub with a new client certificate chain issued by the device provisioning
        /// service or IoT Hub. It receives the issued chain (Base64-encoded, device certificate first) and
        /// returns the certificate, with its private key, to present to IoT Hub, so it can apply custom logic such as
        /// persisting the certificate or pairing it with an HSM-backed key. It is invoked once per issued chain.
        /// If it throws, the connection attempt fails.
        /// </param>
        /// <param name="certificateChain">
        /// The certificate chain leading to the root certificate uploaded to the device provisioning service.
        /// </param>
        /// <param name="remoteCertificateValidationCallback">
        /// An optional callback used to perform custom validation of the server (remote) certificate during the TLS handshake.
        /// When null, the platform's default validation is used.
        /// </param>
        /// <param name="localCertificateSelectionCallback">
        /// An optional callback used to select which client certificate to present during the TLS handshake.
        /// </param>
        public X509AuthenticationProvider(
            X509Certificate2 clientCertificate,
            Func<IReadOnlyList<string>, Task<X509Certificate2>> handleCertificateSigningCompleteAsync,
            X509Certificate2Collection? certificateChain = null,
            RemoteCertificateValidationCallback? remoteCertificateValidationCallback = null,
            LocalCertificateSelectionCallback? localCertificateSelectionCallback = null) : base(remoteCertificateValidationCallback, localCertificateSelectionCallback)
        {
            ClientCertificate = clientCertificate
                ?? throw new ArgumentException("No certificate was found. To use certificate authentication certificate must be present.", nameof(clientCertificate));

            CertificateChain = certificateChain;
            HandleCertificateSigningCompleteAsync = handleCertificateSigningCompleteAsync
                ?? throw new ArgumentNullException(nameof(handleCertificateSigningCompleteAsync));
            _issuedCertificateHandler = new IssuedCertificateHandler(handleCertificateSigningCompleteAsync);
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

        /// <summary>
        /// Produces the certificate to present to IoT Hub from an issued client certificate chain. Either the callback
        /// this provider was created with or, by default, one that pairs the issued device certificate with
        /// <see cref="IssuedCertificatePrivateKey"/> (or presents <see cref="ClientCertificate"/> if there is no such key).
        /// </summary>
        public Func<IReadOnlyList<string>, Task<X509Certificate2>> HandleCertificateSigningCompleteAsync { get; }

        private readonly IssuedCertificateHandler _issuedCertificateHandler;

        public override async Task<AuthenticationCredentials> GetOperationalAuthenticationCredentialsAsync(string hostName, string deviceId, IReadOnlyList<string>? issuedClientCertificates, CancellationToken cancellationToken = default)
        {
            if (issuedClientCertificates == null || issuedClientCertificates.Count == 0)
            {
                return CreateCredentials(ClientCertificate);
            }

            X509Certificate2 certificate = await _issuedCertificateHandler.GetCertificateAsync(issuedClientCertificates, cancellationToken).ConfigureAwait(false);
            return CreateCredentials(certificate);
        }

        private Task<X509Certificate2> PresentOriginalCertificateAsync(IReadOnlyList<string> issuedClientCertificates)
        {
            Trace.TraceWarning("A client certificate was issued to this device, but this provider was created without an issuedCertificatePrivateKey or handleCertificateSigningCompleteAsync callback, so the original client certificate will be presented to IoT Hub instead.");
            return Task.FromResult(ClientCertificate);
        }

        public override AuthenticationCredentials GetBootAuthenticationCredentials()
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
