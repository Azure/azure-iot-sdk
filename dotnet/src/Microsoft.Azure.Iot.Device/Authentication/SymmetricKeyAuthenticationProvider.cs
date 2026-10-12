// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using System.Globalization;
using System.Net;
using System.Security.Cryptography;
using System.Text;

namespace Microsoft.Azure.Iot.Device.Authentication
{
    public class SymmetricKeyAuthenticationProvider : AuthenticationProvider
    {
        private string _symmetricKey; //TODO byte[]
        
        private string _registrationId;

        private string _idScope;

        public SymmetricKeyAuthenticationProvider(string registrationId, string idScope, string symmetricKey) //TODO primary + secondary keys?
        { 
            _idScope = idScope;
            _registrationId = registrationId;
            // Note that the device will use the same symmetric key when deriving SAS tokens to authenticate with IoT Hub and with DPS
            _symmetricKey = symmetricKey;
        }

        public override AuthenticationCredentials GetIotHubAuthentication(string hostName, string deviceId)
        {
            // Note that hostname + device id may change over time, so cannot just take them in constructor.
            // Device-scoped token: no key name (skn) since the key is the device's own key
            string sasToken = BuildSasToken(string.Empty, _symmetricKey, string.Concat(hostName, "/devices/", deviceId), TimeSpan.FromHours(1)); //TODO configurable timespan
            return CreateCredentials(sasToken);
        }

        public override AuthenticationCredentials GetDeviceProvisioningServiceAuthentication()
        {
            string sasToken = BuildSasToken("registration", _symmetricKey, string.Concat(_idScope, '/', "registrations", '/', _registrationId), TimeSpan.FromHours(1)); //TODO configurable timespan
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
