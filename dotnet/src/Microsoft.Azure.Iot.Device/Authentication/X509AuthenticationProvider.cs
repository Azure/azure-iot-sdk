// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using System.Net.Security;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;

namespace Microsoft.Azure.Iot.Device.Authentication
{
    /// <summary>
    /// Authenticates with Device Provisioning Service and IoT Hub using an X509 client certificate.
    /// </summary>
    /// <remarks>
    /// <see cref="ClientCertificate"/> is always presented to Device Provisioning Service. IoT Hub is presented the most
    /// recently issued client certificate if this provider has a certificate signing handler (see
    /// <see cref="AuthenticationProvider.HasCertificateSigningHandler"/>), and <see cref="ClientCertificate"/> otherwise.
    /// </remarks>
    public class X509AuthenticationProvider : AuthenticationProvider
    {
        /// <summary>
        /// Creates a provider that always presents <paramref name="clientCertificate"/> and cannot use issued client certificates.
        /// The key need not be exportable; HSM-backed certificates are supported and the SDK never exports the key.
        /// </summary>
        /// <param name="clientCertificate">The client certificate, with its private key.</param>
        /// <param name="certificateChain">The chain leading to the root certificate uploaded to the service.</param>
        /// <param name="remoteCertificateValidationCallback">Optional custom validation of the remote TLS certificate.</param>
        /// <param name="localCertificateSelectionCallback">Optional selection of the client TLS certificate.</param>
        public X509AuthenticationProvider(
            X509Certificate2 clientCertificate,
            X509Certificate2Collection? certificateChain = null,
            RemoteCertificateValidationCallback? remoteCertificateValidationCallback = null,
            LocalCertificateSelectionCallback? localCertificateSelectionCallback = null)
            : base(remoteCertificateValidationCallback, localCertificateSelectionCallback)
        {
            ClientCertificate = clientCertificate ?? throw new ArgumentNullException(nameof(clientCertificate));
            CertificateChain = certificateChain;
        }

        /// <summary>
        /// Creates a provider that presents <paramref name="clientCertificate"/> until a client certificate is issued to
        /// this device, then presents the issued certificate to IoT Hub paired with <paramref name="issuedCertificatePrivateKey"/>.
        /// </summary>
        /// <param name="clientCertificate">The client certificate, with its private key.</param>
        /// <param name="issuedCertificatePrivateKey">
        /// The private key that this device's certificate signing requests are generated with.
        /// </param>
        /// <param name="certificateChain">The chain leading to the root certificate uploaded to the service.</param>
        /// <param name="remoteCertificateValidationCallback">Optional custom validation of the remote TLS certificate.</param>
        /// <param name="localCertificateSelectionCallback">Optional selection of the client TLS certificate.</param>
        public X509AuthenticationProvider(
            X509Certificate2 clientCertificate,
            AsymmetricAlgorithm issuedCertificatePrivateKey,
            X509Certificate2Collection? certificateChain = null,
            RemoteCertificateValidationCallback? remoteCertificateValidationCallback = null,
            LocalCertificateSelectionCallback? localCertificateSelectionCallback = null)
            : base(issuedCertificatePrivateKey, remoteCertificateValidationCallback, localCertificateSelectionCallback)
        {
            ClientCertificate = clientCertificate ?? throw new ArgumentNullException(nameof(clientCertificate));
            CertificateChain = certificateChain;
        }

        /// <summary>
        /// Creates a provider that presents <paramref name="clientCertificate"/> until a client certificate is issued to
        /// this device, then delegates turning the issued chain into the certificate presented to IoT Hub to a callback.
        /// </summary>
        /// <param name="clientCertificate">The client certificate, with its private key.</param>
        /// <param name="handleCertificateSigningCompleteAsync">
        /// Invoked when a client certificate chain is issued to this device. See
        /// <see cref="AuthenticationProvider(Func{IReadOnlyList{string}, Task{X509Certificate2}}, RemoteCertificateValidationCallback?, LocalCertificateSelectionCallback?)"/>.
        /// </param>
        /// <param name="certificateChain">The chain leading to the root certificate uploaded to the service.</param>
        /// <param name="remoteCertificateValidationCallback">Optional custom validation of the remote TLS certificate.</param>
        /// <param name="localCertificateSelectionCallback">Optional selection of the client TLS certificate.</param>
        public X509AuthenticationProvider(
            X509Certificate2 clientCertificate,
            Func<IReadOnlyList<string>, Task<X509Certificate2>> handleCertificateSigningCompleteAsync,
            X509Certificate2Collection? certificateChain = null,
            RemoteCertificateValidationCallback? remoteCertificateValidationCallback = null,
            LocalCertificateSelectionCallback? localCertificateSelectionCallback = null)
            : base(handleCertificateSigningCompleteAsync, remoteCertificateValidationCallback, localCertificateSelectionCallback)
        {
            ClientCertificate = clientCertificate ?? throw new ArgumentNullException(nameof(clientCertificate));
            CertificateChain = certificateChain;
        }

        /// <summary>
        /// The client certificate presented to Device Provisioning Service, and to IoT Hub until a client certificate is issued.
        /// </summary>
        public X509Certificate2 ClientCertificate { get; }

        /// <summary>
        /// The certificate trust chain of <see cref="ClientCertificate"/> that ends in the trusted root installed on the server side.
        /// </summary>
        public X509Certificate2Collection? CertificateChain { get; }

        /// <inheritdoc/>
        /// <remarks>The registration Id is the DNS name of <see cref="ClientCertificate"/>.</remarks>
        public override string GetRegistrationId()
        {
            return ClientCertificate.GetNameInfo(X509NameType.DnsName, false);
        }

        /// <inheritdoc/>
        public override Task<AuthenticationCredentials> GetProvisioningCredentialsAsync(CancellationToken cancellationToken = default)
        {
            cancellationToken.ThrowIfCancellationRequested();
            return Task.FromResult(CreateCredentials(ClientCertificate));
        }

        /// <inheritdoc/>
        public override async Task<AuthenticationCredentials> GetIotHubCredentialsAsync(
            string hostName,
            string deviceId,
            IReadOnlyList<string>? issuedClientCertificates,
            CancellationToken cancellationToken = default)
        {
            cancellationToken.ThrowIfCancellationRequested();
            X509Certificate2? issuedCertificate = await GetIssuedCertificateAsync(issuedClientCertificates, cancellationToken).ConfigureAwait(false);
            return CreateCredentials(issuedCertificate ?? ClientCertificate);
        }

        private AuthenticationCredentials CreateCredentials(X509Certificate2 certificate)
        {
            return new AuthenticationCredentials()
            {
                ClientCertificate = certificate,
                RemoteCertificateValidationCallback = RemoteCertificateValidationCallback,
                LocalCertificateSelectionCallback = LocalCertificateSelectionCallback,
            };
        }
    }
}
