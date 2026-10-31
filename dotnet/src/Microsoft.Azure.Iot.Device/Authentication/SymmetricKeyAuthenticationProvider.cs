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
    /// Authenticates with a symmetric key and optionally presents issued client certificates to IoT Hub.
    /// </summary>
    /// <remarks>
    /// DPS always uses a SAS token. IoT Hub uses a SAS token until a certificate is issued and this provider has
    /// a private key or callback to prepare it. Issued certificates are prepared and cached as with
    /// <see cref="X509AuthenticationProvider"/>.
    /// </remarks>
    public class SymmetricKeyAuthenticationProvider : AuthenticationProvider
    {
        private readonly byte[] _primarySymmetricKey;

        private readonly byte[]? _secondarySymmetricKey;

        private readonly IssuedCertificateHandler? _issuedCertificateHandler;

        private readonly TimeSpan _iotHubSasTokenTimeToLive;

        private readonly TimeSpan _dpsSasTokenTimeToLive;

        private string _registrationId;

        private string _idScope;

        /// <summary>
        /// Creates a provider that always authenticates with a symmetric key, even if a client certificate is issued.
        /// To present issued certificates to IoT Hub, use a constructor that takes their private key or a callback.
        /// </summary>
        /// <param name="registrationId">The registration Id to provision as.</param>
        /// <param name="idScope">The device provisioning service Id scope.</param>
        /// <param name="primarySymmetricKey">The raw primary symmetric key bytes, not a Base64-encoded string.</param>
        /// <param name="secondarySymmetricKey">
        /// The raw secondary symmetric key bytes, not a Base64-encoded string, or null if this device has none. When IoT
        /// Hub or the device provisioning service rejects the SAS token signed with <paramref name="primarySymmetricKey"/>, the
        /// connection client retries at once with one signed with this key. If IoT Hub rejects both, the connection
        /// client re-provisions the device when it can. Once a client certificate is issued and presented to IoT Hub
        /// instead of a SAS token, this key is only used for the device provisioning service.
        /// </param>
        /// <param name="remoteCertificateValidationCallback">An optional callback for validating the server certificate.</param>
        /// <param name="localCertificateSelectionCallback">An optional callback for selecting the client certificate.</param>
        /// <param name="iotHubSasTokenTimeToLive">The lifetime of IoT Hub SAS tokens. Must be positive. Defaults to one hour.</param>
        /// <param name="dpsSasTokenTimeToLive">The lifetime of device provisioning service SAS tokens. Must be positive. Defaults to ten minutes.</param>
        public SymmetricKeyAuthenticationProvider(
            string registrationId,
            string idScope,
            byte[] primarySymmetricKey,
            byte[]? secondarySymmetricKey,
            RemoteCertificateValidationCallback? remoteCertificateValidationCallback = null,
            LocalCertificateSelectionCallback? localCertificateSelectionCallback = null,
            TimeSpan? iotHubSasTokenTimeToLive = null,
            TimeSpan? dpsSasTokenTimeToLive = null)
            : base(remoteCertificateValidationCallback, localCertificateSelectionCallback)
        {
            ArgumentNullException.ThrowIfNull(primarySymmetricKey);
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

            _idScope = idScope;
            _registrationId = registrationId;
            // Note that the device will use the same symmetric key when deriving SAS tokens to authenticate with IoT Hub and with DPS
            _primarySymmetricKey = primarySymmetricKey;
            _secondarySymmetricKey = secondarySymmetricKey;
        }

        /// <summary>
        /// Creates a provider that pairs issued client certificates with their CSR private key for IoT Hub authentication.
        /// </summary>
        /// <param name="registrationId">The registration Id to provision as.</param>
        /// <param name="idScope">The device provisioning service Id scope.</param>
        /// <param name="primarySymmetricKey">The raw primary symmetric key bytes used for DPS and for IoT Hub until a certificate is issued.</param>
        /// <param name="secondarySymmetricKey">
        /// The raw secondary symmetric key bytes, not a Base64-encoded string, or null if this device has none. When IoT
        /// Hub or the device provisioning service rejects the SAS token signed with <paramref name="primarySymmetricKey"/>, the
        /// connection client retries at once with one signed with this key. If IoT Hub rejects both, the connection
        /// client re-provisions the device when it can. Once a client certificate is issued and presented to IoT Hub
        /// instead of a SAS token, this key is only used for the device provisioning service.
        /// </param>
        /// <param name="issuedCertificatePrivateKey">
        /// The private key used to generate this device's certificate signing requests. Issued device certificates
        /// are paired with this key and presented to IoT Hub instead of a SAS token.
        /// </param>
        /// <param name="remoteCertificateValidationCallback">An optional callback for validating the server certificate.</param>
        /// <param name="localCertificateSelectionCallback">An optional callback for selecting the client certificate.</param>
        /// <param name="iotHubSasTokenTimeToLive">The lifetime of IoT Hub SAS tokens used before a certificate is issued. Must be positive. Defaults to one hour.</param>
        /// <param name="dpsSasTokenTimeToLive">The lifetime of device provisioning service SAS tokens. Must be positive. Defaults to ten minutes.</param>
        public SymmetricKeyAuthenticationProvider(
            string registrationId,
            string idScope,
            byte[] primarySymmetricKey,
            byte[]? secondarySymmetricKey,
            AsymmetricAlgorithm issuedCertificatePrivateKey,
            RemoteCertificateValidationCallback? remoteCertificateValidationCallback = null,
            LocalCertificateSelectionCallback? localCertificateSelectionCallback = null,
            TimeSpan? iotHubSasTokenTimeToLive = null,
            TimeSpan? dpsSasTokenTimeToLive = null)
            : this(registrationId, idScope, primarySymmetricKey, secondarySymmetricKey, remoteCertificateValidationCallback, localCertificateSelectionCallback, iotHubSasTokenTimeToLive, dpsSasTokenTimeToLive)
        {
            IssuedCertificatePrivateKey = issuedCertificatePrivateKey ?? throw new ArgumentNullException(nameof(issuedCertificatePrivateKey));
            HandleCertificateSigningCompleteAsync = IssuedCertificateHandler.CreateDefaultCallback(issuedCertificatePrivateKey);
            _issuedCertificateHandler = new IssuedCertificateHandler(HandleCertificateSigningCompleteAsync);
        }

        /// <summary>
        /// Creates a provider that delegates preparation of issued client certificates to a callback.
        /// </summary>
        /// <param name="registrationId">The registration Id to provision as.</param>
        /// <param name="idScope">The device provisioning service Id scope.</param>
        /// <param name="primarySymmetricKey">The raw primary symmetric key bytes used for DPS and for IoT Hub until a certificate is issued.</param>
        /// <param name="secondarySymmetricKey">
        /// The raw secondary symmetric key bytes, not a Base64-encoded string, or null if this device has none. When IoT
        /// Hub or the device provisioning service rejects the SAS token signed with <paramref name="primarySymmetricKey"/>, the
        /// connection client retries at once with one signed with this key. If IoT Hub rejects both, the connection
        /// client re-provisions the device when it can. Once a client certificate is issued and presented to IoT Hub
        /// instead of a SAS token, this key is only used for the device provisioning service.
        /// </param>
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
        public SymmetricKeyAuthenticationProvider(
            string registrationId,
            string idScope,
            byte[] primarySymmetricKey,
            byte[]? secondarySymmetricKey,
            Func<IReadOnlyList<string>, Task<X509Certificate2>> handleCertificateSigningCompleteAsync,
            RemoteCertificateValidationCallback? remoteCertificateValidationCallback = null,
            LocalCertificateSelectionCallback? localCertificateSelectionCallback = null,
            TimeSpan? iotHubSasTokenTimeToLive = null,
            TimeSpan? dpsSasTokenTimeToLive = null)
            : this(registrationId, idScope, primarySymmetricKey, secondarySymmetricKey, remoteCertificateValidationCallback, localCertificateSelectionCallback, iotHubSasTokenTimeToLive, dpsSasTokenTimeToLive)
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
        /// or by pairing with <see cref="IssuedCertificatePrivateKey"/>. Null when this provider always uses SAS tokens.
        /// </summary>
        public Func<IReadOnlyList<string>, Task<X509Certificate2>>? HandleCertificateSigningCompleteAsync { get; }

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

                Trace.TraceWarning("A client certificate was issued to this device, but this provider was created without an issuedCertificatePrivateKey or handleCertificateSigningCompleteAsync callback, so it will authenticate with IoT Hub using its symmetric key.");
            }

            // Note that hostname + device id may change over time, so cannot just take them in constructor.
            return CreateOperationalCredentials(_primarySymmetricKey, hostName, deviceId);
        }

        public override Task<AuthenticationCredentials?> GetSecondaryOperationalAuthenticationCredentialsAsync(string hostName, string deviceId, IReadOnlyList<string>? issuedClientCertificates, CancellationToken cancellationToken = default)
        {
            if (_secondarySymmetricKey == null || (issuedClientCertificates is { Count: > 0 } && _issuedCertificateHandler != null))
            {
                // An issued certificate is presented instead of a SAS token, so there is no secondary to fall back to.
                return Task.FromResult<AuthenticationCredentials?>(null);
            }

            return Task.FromResult<AuthenticationCredentials?>(CreateOperationalCredentials(_secondarySymmetricKey, hostName, deviceId));
        }

        public override Task<AuthenticationCredentials> GetPrimaryBootAuthenticationCredentialsAsync(CancellationToken cancellationToken = default)
        {
            return Task.FromResult(CreateBootCredentials(_primarySymmetricKey));
        }

        public override Task<AuthenticationCredentials?> GetSecondaryBootAuthenticationCredentialsAsync(CancellationToken cancellationToken = default)
        {
            return Task.FromResult(_secondarySymmetricKey == null ? null : CreateBootCredentials(_secondarySymmetricKey));
        }

        private AuthenticationCredentials CreateOperationalCredentials(byte[] key, string hostName, string deviceId)
        {
            // Device-scoped token: no key name (skn) since the key is the device's own key
            string sasToken = BuildSasToken(string.Empty, key, string.Concat(hostName, "/devices/", deviceId), _iotHubSasTokenTimeToLive);
            return CreateCredentials(sasToken);
        }

        private AuthenticationCredentials CreateBootCredentials(byte[] key)
        {
            string sasToken = BuildSasToken("registration", key, string.Concat(_idScope, '/', "registrations", '/', _registrationId), _dpsSasTokenTimeToLive);
            return CreateCredentials(sasToken);
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

        public override string GetRegistrationId()
        {
            return _registrationId;
        }

        private static string BuildSasToken(string keyName, byte[] key, string target, TimeSpan timeToLive)
        {
            string expiresOn = BuildExpiresOn(timeToLive);
            string audience = WebUtility.UrlEncode(target);
            var fields = new List<string>
            {
                audience,
                expiresOn
            };

            // Example string to be signed:
            // dh://myiothub.azure-devices.net/a/b/c?myvalue1=a
            // <Value for ExpiresOn>

            string signature = HMACSign(string.Join("\n", fields), key);

            // Example returned string:
            // SharedAccessSignature sr=ENCODED(dh://myiothub.azure-devices.net/a/b/c?myvalue1=a)&sig=<Signature>&se=<ExpiresOnValue>[&skn=<KeyName>]

            var buffer = new StringBuilder();
            buffer.AppendFormat(CultureInfo.InvariantCulture, "{0} {1}={2}&{3}={4}&{5}={6}",
                "SharedAccessSignature",
                "sr", audience,
                "sig", WebUtility.UrlEncode(signature),
                "se", WebUtility.UrlEncode(expiresOn));

            if (!string.IsNullOrEmpty(keyName))
            {
                buffer.AppendFormat(CultureInfo.InvariantCulture, "&{0}={1}",
                    "skn", WebUtility.UrlEncode(keyName));
            }

            return buffer.ToString();
        }

        private static string BuildExpiresOn(TimeSpan timeToLive)
        {
            var epochTime = new DateTime(1970, 1, 1, 0, 0, 0, 0, DateTimeKind.Utc);
            DateTime expiresOn = DateTime.UtcNow.Add(timeToLive);
            TimeSpan secondsFromBaseTime = expiresOn.Subtract(epochTime);
            long seconds = Convert.ToInt64(secondsFromBaseTime.TotalSeconds, CultureInfo.InvariantCulture);
            return Convert.ToString(seconds, CultureInfo.InvariantCulture);
        }

        private static string HMACSign(string requestString, byte[] key)
        {
            using var hmac = new HMACSHA256(key);
            return Convert.ToBase64String(hmac.ComputeHash(Encoding.UTF8.GetBytes(requestString)));
        }
    }
}
