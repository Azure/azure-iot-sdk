// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using System.Net.Security;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text;

namespace Microsoft.Azure.Iot.Device.Authentication
{
    /// <summary>
    /// Authenticates with Device Provisioning Service and IoT Hub using SAS tokens derived from a device-scoped symmetric key.
    /// </summary>
    /// <remarks>
    /// Device Provisioning Service is always authenticated with a SAS token. IoT Hub is presented the most recently issued
    /// client certificate if this provider has a certificate signing handler (see
    /// <see cref="AuthenticationProvider.HasCertificateSigningHandler"/>), and a SAS token otherwise. A standard
    /// symmetric-key enrollment assigns the same device key to IoT Hub, so the same key is used with both services.
    /// </remarks>
    public class SymmetricKeyAuthenticationProvider : AuthenticationProvider
    {
        private readonly string _registrationId;
        private readonly string _idScope;
        private readonly string _symmetricKey;

        /// <summary>
        /// Creates a provider that always authenticates with SAS tokens and cannot use issued client certificates.
        /// </summary>
        /// <param name="registrationId">The Device Provisioning Service registration Id.</param>
        /// <param name="idScope">The Device Provisioning Service Id scope.</param>
        /// <param name="symmetricKey">The Base64-encoded device symmetric key.</param>
        /// <param name="remoteCertificateValidationCallback">Optional custom validation of the remote TLS certificate.</param>
        /// <param name="localCertificateSelectionCallback">Optional selection of the client TLS certificate.</param>
        public SymmetricKeyAuthenticationProvider(
            string registrationId,
            string idScope,
            string symmetricKey,
            RemoteCertificateValidationCallback? remoteCertificateValidationCallback = null,
            LocalCertificateSelectionCallback? localCertificateSelectionCallback = null)
            : base(remoteCertificateValidationCallback, localCertificateSelectionCallback)
        {
            (_registrationId, _idScope, _symmetricKey) = Validate(registrationId, idScope, symmetricKey);
        }

        /// <summary>
        /// Creates a provider that authenticates with IoT Hub using a SAS token until a client certificate is issued to
        /// this device, then presents the issued certificate paired with <paramref name="issuedCertificatePrivateKey"/>.
        /// </summary>
        /// <param name="registrationId">The Device Provisioning Service registration Id.</param>
        /// <param name="idScope">The Device Provisioning Service Id scope.</param>
        /// <param name="symmetricKey">The Base64-encoded device symmetric key.</param>
        /// <param name="issuedCertificatePrivateKey">
        /// The private key that this device's certificate signing requests are generated with.
        /// </param>
        /// <param name="remoteCertificateValidationCallback">Optional custom validation of the remote TLS certificate.</param>
        /// <param name="localCertificateSelectionCallback">Optional selection of the client TLS certificate.</param>
        public SymmetricKeyAuthenticationProvider(
            string registrationId,
            string idScope,
            string symmetricKey,
            AsymmetricAlgorithm issuedCertificatePrivateKey,
            RemoteCertificateValidationCallback? remoteCertificateValidationCallback = null,
            LocalCertificateSelectionCallback? localCertificateSelectionCallback = null)
            : base(issuedCertificatePrivateKey, remoteCertificateValidationCallback, localCertificateSelectionCallback)
        {
            (_registrationId, _idScope, _symmetricKey) = Validate(registrationId, idScope, symmetricKey);
        }

        /// <summary>
        /// Creates a provider that authenticates with IoT Hub using a SAS token until a client certificate is issued to
        /// this device, then delegates turning the issued chain into the certificate presented to IoT Hub to a callback.
        /// </summary>
        /// <param name="registrationId">The Device Provisioning Service registration Id.</param>
        /// <param name="idScope">The Device Provisioning Service Id scope.</param>
        /// <param name="symmetricKey">The Base64-encoded device symmetric key.</param>
        /// <param name="handleCertificateSigningCompleteAsync">
        /// Invoked when a client certificate chain is issued to this device. See
        /// <see cref="AuthenticationProvider(Func{IReadOnlyList{string}, Task{X509Certificate2}}, RemoteCertificateValidationCallback?, LocalCertificateSelectionCallback?)"/>.
        /// </param>
        /// <param name="remoteCertificateValidationCallback">Optional custom validation of the remote TLS certificate.</param>
        /// <param name="localCertificateSelectionCallback">Optional selection of the client TLS certificate.</param>
        public SymmetricKeyAuthenticationProvider(
            string registrationId,
            string idScope,
            string symmetricKey,
            Func<IReadOnlyList<string>, Task<X509Certificate2>> handleCertificateSigningCompleteAsync,
            RemoteCertificateValidationCallback? remoteCertificateValidationCallback = null,
            LocalCertificateSelectionCallback? localCertificateSelectionCallback = null)
            : base(handleCertificateSigningCompleteAsync, remoteCertificateValidationCallback, localCertificateSelectionCallback)
        {
            (_registrationId, _idScope, _symmetricKey) = Validate(registrationId, idScope, symmetricKey);
        }

        /// <inheritdoc/>
        public override string GetRegistrationId() => _registrationId;

        /// <inheritdoc/>
        public override Task<AuthenticationCredentials> GetProvisioningCredentialsAsync(CancellationToken cancellationToken = default)
        {
            cancellationToken.ThrowIfCancellationRequested();
            return Task.FromResult(CreateSasTokenCredentials("registration", $"{_idScope}/registrations/{_registrationId}"));
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
            if (issuedCertificate != null)
            {
                return new AuthenticationCredentials()
                {
                    ClientCertificate = issuedCertificate,
                    RemoteCertificateValidationCallback = RemoteCertificateValidationCallback,
                    LocalCertificateSelectionCallback = LocalCertificateSelectionCallback,
                };
            }

            return CreateSasTokenCredentials(string.Empty, $"{hostName}/devices/{deviceId}");
        }

        private AuthenticationCredentials CreateSasTokenCredentials(string keyName, string resourceUri)
        {
            string token = SasTokenBuilder.BuildSasToken(keyName, _symmetricKey, resourceUri, TimeSpan.FromHours(1));
            return new AuthenticationCredentials()
            {
                Password = Encoding.UTF8.GetBytes(token),
                RemoteCertificateValidationCallback = RemoteCertificateValidationCallback,
                LocalCertificateSelectionCallback = LocalCertificateSelectionCallback,
            };
        }

        private static (string, string, string) Validate(string registrationId, string idScope, string symmetricKey)
        {
            ArgumentException.ThrowIfNullOrEmpty(registrationId);
            ArgumentException.ThrowIfNullOrEmpty(idScope);
            ArgumentException.ThrowIfNullOrEmpty(symmetricKey);
            return (registrationId, idScope, symmetricKey);
        }
    }
}
