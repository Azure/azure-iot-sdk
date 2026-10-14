// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Authentication;
using Microsoft.Azure.Iot.Device.Authentication.Operational;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using Xunit;

namespace Microsoft.Azure.Iot.Device.UnitTests
{
    public class SymmetricKeyOperationalAuthenticationProviderUnitTests
    {
        private const string HostName = "hub.azure-devices.net";
        private const string DeviceId = "device";
        private static readonly string s_key = Convert.ToBase64String(Encoding.UTF8.GetBytes("operational-key-for-unit-tests"));

        [Fact]
        public async Task NoIssuedCertificatesReturnsSasToken()
        {
            using ECDsa key = ECDsa.Create(ECCurve.NamedCurves.nistP256);
            var provider = new SymmetricKeyOperationalAuthenticationProvider(s_key, key);

            AuthenticationCredentials credentials = await provider.GetCredentialsAsync(HostName, DeviceId, null, TestContext.Current.CancellationToken);

            Assert.Null(credentials.ClientCertificate);
            Assert.Contains("SharedAccessSignature", Encoding.UTF8.GetString(credentials.Password!));
        }

        [Fact]
        public async Task IssuedCertificatesWithPrivateKeyReturnsX509Credentials()
        {
            using ECDsa key = ECDsa.Create(ECCurve.NamedCurves.nistP256);
            using X509Certificate2 issued = CreateCertificate(key);
            var provider = new SymmetricKeyOperationalAuthenticationProvider(s_key, key);

            AuthenticationCredentials credentials = await provider.GetCredentialsAsync(
                HostName, DeviceId, new[] { Convert.ToBase64String(issued.RawData) }, TestContext.Current.CancellationToken);

            Assert.Empty(credentials.Password ?? []);
            Assert.NotNull(credentials.ClientCertificate);
            Assert.Equal(issued.Thumbprint, credentials.ClientCertificate!.Thumbprint);
            Assert.True(credentials.ClientCertificate.HasPrivateKey);
        }

        [Fact]
        public async Task IssuedCertificatesWithCallbackReturnsCallbackCertificateOncePerChain()
        {
            using ECDsa key = ECDsa.Create(ECCurve.NamedCurves.nistP256);
            using X509Certificate2 issued = CreateCertificate(key);
            int callCount = 0;
            var provider = new SymmetricKeyOperationalAuthenticationProvider(s_key, chain =>
            {
                callCount++;
                return Task.FromResult(issued);
            });
            string[] chain = new[] { Convert.ToBase64String(issued.RawData) };

            AuthenticationCredentials first = await provider.GetCredentialsAsync(HostName, DeviceId, chain, TestContext.Current.CancellationToken);
            AuthenticationCredentials second = await provider.GetCredentialsAsync(HostName, DeviceId, chain, TestContext.Current.CancellationToken);

            Assert.Same(issued, first.ClientCertificate);
            Assert.Same(issued, second.ClientCertificate);
            Assert.Empty(first.Password ?? []);
            Assert.Equal(1, callCount);
        }

        [Fact]
        public async Task IssuedCertificatesWithoutKeyOrCallbackReturnsSasToken()
        {
            using ECDsa key = ECDsa.Create(ECCurve.NamedCurves.nistP256);
            using X509Certificate2 issued = CreateCertificate(key);
            var provider = new SymmetricKeyOperationalAuthenticationProvider(s_key);

            AuthenticationCredentials credentials = await provider.GetCredentialsAsync(
                HostName, DeviceId, new[] { Convert.ToBase64String(issued.RawData) }, TestContext.Current.CancellationToken);

            Assert.Null(credentials.ClientCertificate);
            Assert.NotNull(credentials.Password);
        }

        private static X509Certificate2 CreateCertificate(ECDsa key)
        {
            var request = new CertificateRequest($"CN={DeviceId}", key, HashAlgorithmName.SHA256);
            return request.CreateSelfSigned(DateTimeOffset.UtcNow.AddMinutes(-5), DateTimeOffset.UtcNow.AddDays(1));
        }
    }
}
