// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using System.Diagnostics;
using System.Globalization;
using System.Net;
using System.Net.Security;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text;

namespace Microsoft.Azure.Iot.Device.Authentication
{
    /// <summary>
    /// Authenticates with a key held in a Trusted Platform Module (TPM) 2.0.
    /// </summary>
    /// <remarks>
    /// Device Provisioning Service only supports TPM attestation over HTTPS, so provisioning with this provider requires
    /// <see cref="Models.ProvisioningSettings.Transport"/> to be <see cref="Provisioning.Models.ProvisioningTransport.Http"/>.
    /// <para>
    /// During registration, Device Provisioning Service challenges the device with a key encrypted with the TPM's
    /// endorsement key, which only that TPM can import. Every SAS token, for Device Provisioning Service and then for IoT
    /// Hub, is signed by the TPM with the key it imported, so the key never leaves the TPM. A TPM has a single key, so this
    /// provider has no secondary credentials.
    /// </para>
    /// <para>
    /// When created with a private key or callback for issued client certificates, and the device's enrollment issues
    /// one in response to <see cref="Models.ProvisioningSettings.CertificateSigningRequest"/>, the issued certificate is
    /// presented to IoT Hub instead of a TPM-signed SAS token. The TPM is then only used for the device provisioning
    /// service. Issued certificates are prepared and cached as with <see cref="X509AuthenticationProvider"/>.
    /// </para>
    /// </remarks>
    public class TpmAuthenticationProvider : AuthenticationProvider
    {
        private const string DefaultProvisioningKeyName = "registration";

        private readonly ITpmProvider _tpm;
        private readonly IssuedCertificateHandler? _issuedCertificateHandler;
        private readonly SemaphoreSlim _tpmLock = new(1, 1);
        private readonly string _registrationId;
        private readonly string _idScope;
        private readonly TimeSpan _iotHubSasTokenTimeToLive;
        private readonly TimeSpan _dpsSasTokenTimeToLive;

        /// <summary>
        /// Creates a provider that authenticates with a TPM.
        /// </summary>
        /// <param name="registrationId">The registration Id to provision as. This is the registration Id of the device's individual enrollment.</param>
        /// <param name="idScope">The device provisioning service Id scope.</param>
        /// <param name="tpm">The TPM to authenticate with.</param>
        /// <param name="remoteCertificateValidationCallback">An optional callback for validating the server certificate.</param>
        /// <param name="localCertificateSelectionCallback">An optional callback for selecting the client certificate.</param>
        /// <param name="iotHubSasTokenTimeToLive">The lifetime of IoT Hub SAS tokens. Must be positive. Defaults to one hour.</param>
        /// <param name="dpsSasTokenTimeToLive">The lifetime of device provisioning service SAS tokens. Must be positive. Defaults to ten minutes.</param>
        public TpmAuthenticationProvider(
            string registrationId,
            string idScope,
            ITpmProvider tpm,
            RemoteCertificateValidationCallback? remoteCertificateValidationCallback = null,
            LocalCertificateSelectionCallback? localCertificateSelectionCallback = null,
            TimeSpan? iotHubSasTokenTimeToLive = null,
            TimeSpan? dpsSasTokenTimeToLive = null)
            : base(remoteCertificateValidationCallback, localCertificateSelectionCallback)
        {
            ArgumentException.ThrowIfNullOrEmpty(registrationId);
            ArgumentException.ThrowIfNullOrEmpty(idScope);
            ArgumentNullException.ThrowIfNull(tpm);

            _iotHubSasTokenTimeToLive = iotHubSasTokenTimeToLive ?? TimeSpan.FromHours(1);
            _dpsSasTokenTimeToLive = dpsSasTokenTimeToLive ?? TimeSpan.FromMinutes(10);
            if (_iotHubSasTokenTimeToLive <= TimeSpan.Zero)
            {
                throw new ArgumentOutOfRangeException(nameof(iotHubSasTokenTimeToLive), "SAS token lifetime must be positive.");
            }

            if (_dpsSasTokenTimeToLive <= TimeSpan.Zero)
            {
                throw new ArgumentOutOfRangeException(nameof(dpsSasTokenTimeToLive), "SAS token lifetime must be positive.");
            }

            _registrationId = registrationId;
            _idScope = idScope;
            _tpm = tpm;
        }

        /// <summary>
        /// Creates a provider that authenticates with a TPM for the device provisioning service, and presents issued
        /// client certificates, paired with their CSR private key, to IoT Hub.
        /// </summary>
        /// <param name="registrationId">The registration Id to provision as. This is the registration Id of the device's individual enrollment.</param>
        /// <param name="idScope">The device provisioning service Id scope.</param>
        /// <param name="tpm">The TPM to authenticate with.</param>
        /// <param name="issuedCertificatePrivateKey">
        /// The private key used to generate this device's certificate signing requests. Issued device certificates
        /// are paired with this key and presented to IoT Hub instead of a TPM-signed SAS token.
        /// </param>
        /// <param name="remoteCertificateValidationCallback">An optional callback for validating the server certificate.</param>
        /// <param name="localCertificateSelectionCallback">An optional callback for selecting the client certificate.</param>
        /// <param name="iotHubSasTokenTimeToLive">The lifetime of IoT Hub SAS tokens used before a certificate is issued. Must be positive. Defaults to one hour.</param>
        /// <param name="dpsSasTokenTimeToLive">The lifetime of device provisioning service SAS tokens. Must be positive. Defaults to ten minutes.</param>
        public TpmAuthenticationProvider(
            string registrationId,
            string idScope,
            ITpmProvider tpm,
            AsymmetricAlgorithm issuedCertificatePrivateKey,
            RemoteCertificateValidationCallback? remoteCertificateValidationCallback = null,
            LocalCertificateSelectionCallback? localCertificateSelectionCallback = null,
            TimeSpan? iotHubSasTokenTimeToLive = null,
            TimeSpan? dpsSasTokenTimeToLive = null)
            : this(registrationId, idScope, tpm, remoteCertificateValidationCallback, localCertificateSelectionCallback, iotHubSasTokenTimeToLive, dpsSasTokenTimeToLive)
        {
            IssuedCertificatePrivateKey = issuedCertificatePrivateKey ?? throw new ArgumentNullException(nameof(issuedCertificatePrivateKey));
            HandleCertificateSigningCompleteAsync = IssuedCertificateHandler.CreateDefaultCallback(issuedCertificatePrivateKey);
            _issuedCertificateHandler = new IssuedCertificateHandler(HandleCertificateSigningCompleteAsync);
        }

        /// <summary>
        /// Creates a provider that authenticates with a TPM for the device provisioning service, and delegates
        /// preparation of issued client certificates for IoT Hub to a callback.
        /// </summary>
        /// <param name="registrationId">The registration Id to provision as. This is the registration Id of the device's individual enrollment.</param>
        /// <param name="idScope">The device provisioning service Id scope.</param>
        /// <param name="tpm">The TPM to authenticate with.</param>
        /// <param name="handleCertificateSigningCompleteAsync">
        /// Invoked before connecting to IoT Hub with a new certificate chain issued by DPS or IoT Hub.
        /// Receives the Base64-encoded chain, device certificate first, and returns the certificate with its private key
        /// to present to IoT Hub. A successful result is cached for the chain. If the callback throws, the connection
        /// attempt fails.
        /// </param>
        /// <param name="remoteCertificateValidationCallback">An optional callback for validating the server certificate.</param>
        /// <param name="localCertificateSelectionCallback">An optional callback for selecting the client certificate.</param>
        /// <param name="iotHubSasTokenTimeToLive">The lifetime of IoT Hub SAS tokens used before a certificate is issued. Must be positive. Defaults to one hour.</param>
        /// <param name="dpsSasTokenTimeToLive">The lifetime of device provisioning service SAS tokens. Must be positive. Defaults to ten minutes.</param>
        public TpmAuthenticationProvider(
            string registrationId,
            string idScope,
            ITpmProvider tpm,
            Func<IReadOnlyList<string>, Task<X509Certificate2>> handleCertificateSigningCompleteAsync,
            RemoteCertificateValidationCallback? remoteCertificateValidationCallback = null,
            LocalCertificateSelectionCallback? localCertificateSelectionCallback = null,
            TimeSpan? iotHubSasTokenTimeToLive = null,
            TimeSpan? dpsSasTokenTimeToLive = null)
            : this(registrationId, idScope, tpm, remoteCertificateValidationCallback, localCertificateSelectionCallback, iotHubSasTokenTimeToLive, dpsSasTokenTimeToLive)
        {
            HandleCertificateSigningCompleteAsync = handleCertificateSigningCompleteAsync
                ?? throw new ArgumentNullException(nameof(handleCertificateSigningCompleteAsync));
            _issuedCertificateHandler = new IssuedCertificateHandler(handleCertificateSigningCompleteAsync);
        }

        /// <summary>
        /// The private key used for this device's certificate signing requests, if configured. To rotate keys,
        /// create a new provider. Pairing and non-exportable key handling follow <see cref="X509AuthenticationProvider"/>.
        /// </summary>
        public AsymmetricAlgorithm? IssuedCertificatePrivateKey { get; }

        /// <summary>
        /// Produces the certificate to present to IoT Hub from an issued chain, either using the configured callback
        /// or by pairing with <see cref="IssuedCertificatePrivateKey"/>. Null when this provider always uses TPM-signed SAS tokens.
        /// </summary>
        public Func<IReadOnlyList<string>, Task<X509Certificate2>>? HandleCertificateSigningCompleteAsync { get; }

        /// <summary>
        /// True if this provider presents issued client certificates to IoT Hub rather than TPM-signed SAS tokens.
        /// </summary>
        internal bool PresentsIssuedCertificates => _issuedCertificateHandler != null;

        public override string GetRegistrationId()
        {
            return _registrationId;
        }

        public override async Task<AuthenticationCredentials> GetPrimaryOperationalAuthenticationCredentialsAsync(string hostName, string deviceId, IReadOnlyList<string>? issuedClientCertificates, CancellationToken cancellationToken = default)
        {
            if (issuedClientCertificates is { Count: > 0 })
            {
                if (_issuedCertificateHandler != null)
                {
                    X509Certificate2 certificate = await _issuedCertificateHandler.GetCertificateAsync(issuedClientCertificates, cancellationToken).ConfigureAwait(false);
                    return new AuthenticationCredentials()
                    {
                        ClientCertificate = certificate,
                        RemoteCertificateValidationCallback = RemoteCertificateValidationCallback,
                        LocalCertificateSelectionCallback = LocalCertificateSelectionCallback,
                    };
                }

                Trace.TraceWarning("A client certificate was issued to this device, but this provider was created without an issuedCertificatePrivateKey or handleCertificateSigningCompleteAsync callback, so it will authenticate with IoT Hub using its TPM.");
            }

            // Device-scoped token: no key name (skn) since the key is the device's own key.
            string sasToken = await BuildSasTokenAsync(string.Empty, string.Concat(hostName, "/devices/", deviceId), _iotHubSasTokenTimeToLive, cancellationToken).ConfigureAwait(false);
            return CreateCredentials(sasToken);
        }

        /// <summary>
        /// Gets the credentials that the first register request to the device provisioning service is sent with.
        /// </summary>
        /// <remarks>
        /// These carry only the TLS callbacks: the first register request is unauthenticated, and Device Provisioning
        /// Service answers it with the key that later requests are signed with.
        /// </remarks>
        public override Task<AuthenticationCredentials> GetPrimaryBootAuthenticationCredentialsAsync(CancellationToken cancellationToken = default)
        {
            return Task.FromResult(new AuthenticationCredentials()
            {
                RemoteCertificateValidationCallback = RemoteCertificateValidationCallback,
                LocalCertificateSelectionCallback = LocalCertificateSelectionCallback,
            });
        }

        internal Task<byte[]> GetEndorsementKeyAsync(CancellationToken cancellationToken)
        {
            return WithTpmAsync(() => _tpm.GetEndorsementKeyAsync(cancellationToken), cancellationToken);
        }

        internal Task<byte[]> GetStorageRootKeyAsync(CancellationToken cancellationToken)
        {
            return WithTpmAsync(() => _tpm.GetStorageRootKeyAsync(cancellationToken), cancellationToken);
        }

        internal Task ImportKeyAsync(byte[] encryptedKey, CancellationToken cancellationToken)
        {
            return WithTpmAsync(async () =>
            {
                await _tpm.ImportKeyAsync(encryptedKey, cancellationToken).ConfigureAwait(false);
                return true;
            }, cancellationToken);
        }

        /// <summary>
        /// Builds the SAS token for the device provisioning service, signed with the key imported last.
        /// </summary>
        /// <param name="keyName">The key name Device Provisioning Service sent with its challenge, if any.</param>
        /// <param name="cancellationToken">The cancellation token.</param>
        internal Task<string> BuildProvisioningSasTokenAsync(string? keyName, CancellationToken cancellationToken)
        {
            return BuildSasTokenAsync(
                string.IsNullOrEmpty(keyName) ? DefaultProvisioningKeyName : keyName,
                string.Concat(_idScope, "/registrations/", _registrationId),
                _dpsSasTokenTimeToLive,
                cancellationToken);
        }

        private async Task<string> BuildSasTokenAsync(string keyName, string target, TimeSpan timeToLive, CancellationToken cancellationToken)
        {
            string expiresOn = DateTimeOffset.UtcNow.Add(timeToLive).ToUnixTimeSeconds().ToString(CultureInfo.InvariantCulture);
            string audience = WebUtility.UrlEncode(target);
            byte[] toSign = Encoding.UTF8.GetBytes(string.Concat(audience, "\n", expiresOn));

            byte[] hmac = await WithTpmAsync(() => _tpm.SignAsync(toSign, cancellationToken), cancellationToken).ConfigureAwait(false);
            string signature = Convert.ToBase64String(hmac);

            var buffer = new StringBuilder();
            buffer.AppendFormat(CultureInfo.InvariantCulture, "SharedAccessSignature sr={0}&sig={1}&se={2}",
                audience,
                WebUtility.UrlEncode(signature),
                expiresOn);

            if (!string.IsNullOrEmpty(keyName))
            {
                buffer.AppendFormat(CultureInfo.InvariantCulture, "&skn={0}", WebUtility.UrlEncode(keyName));
            }

            return buffer.ToString();
        }

        private AuthenticationCredentials CreateCredentials(string sasToken)
        {
            return new AuthenticationCredentials()
            {
                Password = Encoding.UTF8.GetBytes(sasToken),
                RemoteCertificateValidationCallback = RemoteCertificateValidationCallback,
                LocalCertificateSelectionCallback = LocalCertificateSelectionCallback,
            };
        }

        // A TPM runs one command at a time, and importing a key must not interleave with signing with the key it replaces.
        private async Task<T> WithTpmAsync<T>(Func<Task<T>> operation, CancellationToken cancellationToken)
        {
            await _tpmLock.WaitAsync(cancellationToken).ConfigureAwait(false);
            try
            {
                return await operation().ConfigureAwait(false);
            }
            finally
            {
                _tpmLock.Release();
            }
        }
    }
}
