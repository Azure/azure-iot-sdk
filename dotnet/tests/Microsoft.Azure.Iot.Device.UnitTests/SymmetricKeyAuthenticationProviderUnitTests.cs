// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Authentication;
using System.Globalization;
using System.Net;
using System.Security.Cryptography;
using System.Text;
using Xunit;

namespace Microsoft.Azure.Iot.Device.UnitTests
{
    public class SymmetricKeyAuthenticationProviderUnitTests
    {
        [Theory]
        [InlineData(0, null, null)]
        [InlineData(0, 10, 90)]
        [InlineData(0, 10, null)]
        [InlineData(0, null, 90)]
        [InlineData(1, null, null)]
        [InlineData(1, 10, 90)]
        [InlineData(1, 10, null)]
        [InlineData(1, null, 90)]
        [InlineData(2, null, null)]
        [InlineData(2, 10, 90)]
        [InlineData(2, 10, null)]
        [InlineData(2, null, 90)]
        public async Task CredentialsUseRawKeyBytesAndEndpointSpecificLifetimes(int providerKind, int? iotHubTtlMinutes, int? dpsTtlMinutes)
        {
            const string registrationId = "registration-id";
            const string idScope = "0ne00000000";
            const string hostName = "some-hub.azure-devices.net";
            const string deviceId = "device-id";
            byte[] key = new byte[] { 0x00, 0xFF, 0x80, 0x01, 0x42, 0x0A, 0xFE, 0x7F };
            TimeSpan? iotHubTimeToLive = iotHubTtlMinutes.HasValue ? TimeSpan.FromMinutes(iotHubTtlMinutes.Value) : null;
            TimeSpan? dpsTimeToLive = dpsTtlMinutes.HasValue ? TimeSpan.FromMinutes(dpsTtlMinutes.Value) : null;
            using RSA privateKey = RSA.Create(2048);
            AuthenticationProvider authentication = providerKind switch
            {
                0 => new SymmetricKeyAuthenticationProvider(
                    registrationId, idScope, key,
                    iotHubSasTokenTimeToLive: iotHubTimeToLive, dpsSasTokenTimeToLive: dpsTimeToLive),
                1 => new SymmetricKeyWithIssuedCertificateAuthenticationProvider(
                    registrationId, idScope, key, privateKey,
                    iotHubSasTokenTimeToLive: iotHubTimeToLive, dpsSasTokenTimeToLive: dpsTimeToLive),
                2 => new SymmetricKeyWithIssuedCertificateAuthenticationProvider(
                    registrationId, idScope, key, _ => throw new InvalidOperationException("No certificate has been issued."),
                    iotHubSasTokenTimeToLive: iotHubTimeToLive, dpsSasTokenTimeToLive: dpsTimeToLive),
                _ => throw new ArgumentOutOfRangeException(nameof(providerKind)),
            };

            var bootCredentials = authentication.GetBootAuthenticationCredentials();
            Assert.Null(bootCredentials.ClientCertificate);
            AssertSasToken(bootCredentials.Password, key, $"{idScope}/registrations/{registrationId}", "registration", dpsTimeToLive ?? TimeSpan.FromHours(1));

            var operationalCredentials = await authentication.GetOperationalAuthenticationCredentialsAsync(hostName, deviceId, null);
            Assert.Null(operationalCredentials.ClientCertificate);
            AssertSasToken(operationalCredentials.Password, key, $"{hostName}/devices/{deviceId}", null, iotHubTimeToLive ?? TimeSpan.FromHours(1));
        }

        [Fact]
        public void NullKeyIsRejected()
        {
            ArgumentNullException exception = Assert.Throws<ArgumentNullException>(
                () => new SymmetricKeyAuthenticationProvider("registration-id", "0ne00000000", null!));
            Assert.Equal("symmetricKey", exception.ParamName);
        }

        [Theory]
        [InlineData(true, 0)]
        [InlineData(true, -1)]
        [InlineData(false, 0)]
        [InlineData(false, -1)]
        public void NonPositiveTokenLifetimeIsRejected(bool configureIotHub, int seconds)
        {
            TimeSpan timeToLive = TimeSpan.FromSeconds(seconds);
            ArgumentOutOfRangeException exception = Assert.Throws<ArgumentOutOfRangeException>(
                () => new SymmetricKeyAuthenticationProvider(
                    "registration-id", "0ne00000000", new byte[] { 1 },
                    iotHubSasTokenTimeToLive: configureIotHub ? timeToLive : null,
                    dpsSasTokenTimeToLive: configureIotHub ? null : timeToLive));

            Assert.Equal(configureIotHub ? "iotHubSasTokenTimeToLive" : "dpsSasTokenTimeToLive", exception.ParamName);
        }

        private static void AssertSasToken(byte[]? password, byte[] key, string target, string? keyName, TimeSpan expectedTimeToLive)
        {
            Assert.NotNull(password);
            string token = Encoding.UTF8.GetString(password);
            const string prefix = "SharedAccessSignature ";
            Assert.StartsWith(prefix, token);
            Dictionary<string, string> fields = token[prefix.Length..].Split('&')
                .Select(field => field.Split('=', 2))
                .ToDictionary(field => field[0], field => field[1]);

            Assert.Equal(WebUtility.UrlEncode(target), fields["sr"]);
            long expiry = long.Parse(fields["se"], CultureInfo.InvariantCulture);
            DateTimeOffset expectedExpiry = DateTimeOffset.UtcNow.Add(expectedTimeToLive);
            Assert.InRange(DateTimeOffset.FromUnixTimeSeconds(expiry), expectedExpiry.AddSeconds(-2), expectedExpiry.AddSeconds(2));

            byte[] signature = HMACSHA256.HashData(key, Encoding.UTF8.GetBytes($"{fields["sr"]}\n{fields["se"]}"));
            Assert.Equal(WebUtility.UrlEncode(Convert.ToBase64String(signature)), fields["sig"]);
            if (keyName == null)
            {
                Assert.False(fields.ContainsKey("skn"));
            }
            else
            {
                Assert.Equal(WebUtility.UrlEncode(keyName), fields["skn"]);
            }
        }
    }
}
