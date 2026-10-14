// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using System.Globalization;
using System.Net;
using System.Net.Security;
using System.Security.Cryptography;
using System.Text;

namespace Microsoft.Azure.Iot.Device.Authentication
{
    public class SymmetricKeyAuthenticationProvider : AuthenticationProvider
    {
        private readonly string _symmetricKey; //TODO byte[]
        
        private readonly string? _registrationId;

        private readonly string? _idScope;

        /// <summary>
        /// Creates an operational provider using the device's Base64-encoded IoT Hub symmetric key.
        /// The assigned hostname and device Id are supplied on each connection attempt.
        /// </summary>
        public SymmetricKeyAuthenticationProvider(
            string symmetricKey,
            RemoteCertificateValidationCallback? remoteCertificateValidationCallback = null,
            LocalCertificateSelectionCallback? localCertificateSelectionCallback = null)
            : base(remoteCertificateValidationCallback, localCertificateSelectionCallback)
        {
            ArgumentException.ThrowIfNullOrEmpty(symmetricKey);
            _symmetricKey = symmetricKey;
        }

        /// <summary>
        /// Creates a boot provider using a DPS registration Id, Id scope and Base64-encoded symmetric key.
        /// </summary>
        public SymmetricKeyAuthenticationProvider(
            string registrationId,
            string idScope,
            string symmetricKey,
            RemoteCertificateValidationCallback? remoteCertificateValidationCallback = null,
            LocalCertificateSelectionCallback? localCertificateSelectionCallback = null) //TODO primary + secondary keys?
            : this(symmetricKey, remoteCertificateValidationCallback, localCertificateSelectionCallback)
        { 
            ArgumentException.ThrowIfNullOrEmpty(registrationId);
            ArgumentException.ThrowIfNullOrEmpty(idScope);
            _idScope = idScope;
            _registrationId = registrationId;
        }

        public override AuthenticationCredentials GetIotHubAuthentication(string hostName, string deviceId, IReadOnlyList<string>? issuedClientCertificates)
        {
            // Note that hostname + device id may change over time, so cannot just take them in constructor.
            // Device-scoped token: no key name (skn) since the key is the device's own key
            string sasToken = BuildSasToken(string.Empty, _symmetricKey, string.Concat(hostName, "/devices/", deviceId), TimeSpan.FromHours(1)); //TODO configurable timespan
            return CreateCredentials(sasToken);
        }

        public override AuthenticationCredentials GetDeviceProvisioningServiceAuthentication()
        {
            if (_idScope == null)
            {
                throw new InvalidOperationException("DPS authentication requires a boot provider with a registration Id and Id scope.");
            }

            string sasToken = BuildSasToken("registration", _symmetricKey, string.Concat(_idScope, '/', "registrations", '/', _registrationId), TimeSpan.FromHours(1)); //TODO configurable timespan
            return CreateCredentials(sasToken);
        }

        private AuthenticationCredentials CreateCredentials(string sasToken)
        {
            return new AuthenticationCredentials() //TODO want to re-use the same instance to persist certificate signing results 
            {
                Password = Encoding.UTF8.GetBytes(sasToken),
                RemoteCertificateValidationCallback = RemoteCertificateValidationCallback,
                LocalCertificateSelectionCallback = LocalCertificateSelectionCallback,
            };
        }

        public override string GetRegistrationId()
        {
            return _registrationId
                ?? throw new InvalidOperationException("An operational symmetric key provider has no DPS registration Id.");
        }

        private static string BuildSasToken(string keyName, string key, string target, TimeSpan timeToLive)
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

        private static string HMACSign(string requestString, string key)
        {
            using var hmac = new HMACSHA256(Convert.FromBase64String(key));
            return Convert.ToBase64String(hmac.ComputeHash(Encoding.UTF8.GetBytes(requestString)));
        }
    }
}
