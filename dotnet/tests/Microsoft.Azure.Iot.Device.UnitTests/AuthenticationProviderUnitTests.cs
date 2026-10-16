// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Authentication;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using Xunit;

namespace Microsoft.Azure.Iot.Device.UnitTests
{
    public class AuthenticationProviderUnitTests
    {
        private const string HostName = "hub.azure-devices.net";
        private const string DeviceId = "device";
        private const string RegistrationId = "registration";
        private const string IdScope = "0ne00000000";
        private static readonly string s_key = Convert.ToBase64String(Encoding.UTF8.GetBytes("device-key-for-unit-tests"));

        [Fact]
        public void HasCertificateSigningHandlerReflectsTheConstructorUsed()
        {
            using ECDsa key = ECDsa.Create(ECCurve.NamedCurves.nistP256);
            using X509Certificate2 certificate = CreateCertificate(key);
            Func<IReadOnlyList<string>, Task<X509Certificate2>> callback = _ => Task.FromResult(certificate);

            Assert.False(new SymmetricKeyAuthenticationProvider(RegistrationId, IdScope, s_key).HasCertificateSigningHandler);
            Assert.True(new SymmetricKeyAuthenticationProvider(RegistrationId, IdScope, s_key, key).HasCertificateSigningHandler);
            Assert.True(new SymmetricKeyAuthenticationProvider(RegistrationId, IdScope, s_key, callback).HasCertificateSigningHandler);

            Assert.False(new X509AuthenticationProvider(certificate).HasCertificateSigningHandler);
            Assert.True(new X509AuthenticationProvider(certificate, key).HasCertificateSigningHandler);
            Assert.True(new X509AuthenticationProvider(certificate, callback).HasCertificateSigningHandler);
        }

        [Fact]
        public async Task SymmetricKeyProviderAuthenticatesWithDpsUsingASasToken()
        {
            using ECDsa key = ECDsa.Create(ECCurve.NamedCurves.nistP256);
            var provider = new SymmetricKeyAuthenticationProvider(RegistrationId, IdScope, s_key, key);

            AuthenticationCredentials credentials = await provider.GetProvisioningCredentialsAsync(TestContext.Current.CancellationToken);

            Assert.Null(credentials.ClientCertificate);
            string token = Encoding.UTF8.GetString(credentials.Password!);
            Assert.StartsWith("SharedAccessSignature ", token);
            Assert.Contains("skn=registration", token);
            Assert.Equal(RegistrationId, provider.GetRegistrationId());
        }

        [Fact]
        public async Task NoIssuedCertificatesReturnsSasToken()
        {
            using ECDsa key = ECDsa.Create(ECCurve.NamedCurves.nistP256);
            var provider = new SymmetricKeyAuthenticationProvider(RegistrationId, IdScope, s_key, key);

            AuthenticationCredentials credentials = await provider.GetIotHubCredentialsAsync(HostName, DeviceId, null, TestContext.Current.CancellationToken);

            Assert.Null(credentials.ClientCertificate);
            Assert.Contains("SharedAccessSignature", Encoding.UTF8.GetString(credentials.Password!));
        }

        [Fact]
        public async Task IssuedCertificatesWithPrivateKeyReturnsX509Credentials()
        {
            using ECDsa key = ECDsa.Create(ECCurve.NamedCurves.nistP256);
            using X509Certificate2 issued = CreateCertificate(key);
            var provider = new SymmetricKeyAuthenticationProvider(RegistrationId, IdScope, s_key, key);

            AuthenticationCredentials credentials = await provider.GetIotHubCredentialsAsync(
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
            var provider = new SymmetricKeyAuthenticationProvider(RegistrationId, IdScope, s_key, chain =>
            {
                callCount++;
                return Task.FromResult(issued);
            });
            string[] chain = new[] { Convert.ToBase64String(issued.RawData) };

            AuthenticationCredentials first = await provider.GetIotHubCredentialsAsync(HostName, DeviceId, chain, TestContext.Current.CancellationToken);
            AuthenticationCredentials second = await provider.GetIotHubCredentialsAsync(HostName, DeviceId, chain, TestContext.Current.CancellationToken);

            Assert.Same(issued, first.ClientCertificate);
            Assert.Same(issued, second.ClientCertificate);
            Assert.Empty(first.Password ?? []);
            Assert.Equal(1, callCount);
        }

        [Fact]
        public async Task SymmetricKeyProviderWithoutHandlerReturnsSasTokenEvenWithIssuedCertificates()
        {
            using ECDsa key = ECDsa.Create(ECCurve.NamedCurves.nistP256);
            using X509Certificate2 issued = CreateCertificate(key);
            var provider = new SymmetricKeyAuthenticationProvider(RegistrationId, IdScope, s_key);

            AuthenticationCredentials credentials = await provider.GetIotHubCredentialsAsync(
                HostName, DeviceId, new[] { Convert.ToBase64String(issued.RawData) }, TestContext.Current.CancellationToken);

            Assert.Null(credentials.ClientCertificate);
            Assert.NotNull(credentials.Password);
        }

        [Fact]
        public async Task X509ProviderPresentsItsCertificateToDpsAndIssuedCertificateToIotHub()
        {
            using ECDsa key = ECDsa.Create(ECCurve.NamedCurves.nistP256);
            using X509Certificate2 certificate = CreateCertificate(key);
            using ECDsa issuedKey = ECDsa.Create(ECCurve.NamedCurves.nistP256);
            using X509Certificate2 issued = CreateCertificate(issuedKey);
            var provider = new X509AuthenticationProvider(certificate, issuedKey);
            string[] chain = new[] { Convert.ToBase64String(issued.RawData) };

            AuthenticationCredentials dps = await provider.GetProvisioningCredentialsAsync(TestContext.Current.CancellationToken);
            AuthenticationCredentials hubBeforeIssuance = await provider.GetIotHubCredentialsAsync(HostName, DeviceId, null, TestContext.Current.CancellationToken);
            AuthenticationCredentials hubAfterIssuance = await provider.GetIotHubCredentialsAsync(HostName, DeviceId, chain, TestContext.Current.CancellationToken);

            Assert.Same(certificate, dps.ClientCertificate);
            Assert.Same(certificate, hubBeforeIssuance.ClientCertificate);
            Assert.Equal(issued.Thumbprint, hubAfterIssuance.ClientCertificate!.Thumbprint);
            Assert.True(hubAfterIssuance.ClientCertificate.HasPrivateKey);
        }

        [Fact]
        public async Task X509ProviderWithoutHandlerKeepsPresentingItsCertificateToIotHub()
        {
            using ECDsa key = ECDsa.Create(ECCurve.NamedCurves.nistP256);
            using X509Certificate2 certificate = CreateCertificate(key);
            using X509Certificate2 issued = CreateCertificate(key);
            var provider = new X509AuthenticationProvider(certificate);

            AuthenticationCredentials credentials = await provider.GetIotHubCredentialsAsync(
                HostName, DeviceId, new[] { Convert.ToBase64String(issued.RawData) }, TestContext.Current.CancellationToken);

            Assert.Same(certificate, credentials.ClientCertificate);
        }

        private static X509Certificate2 CreateCertificate(ECDsa key)
        {
            var request = new CertificateRequest($"CN={DeviceId}", key, HashAlgorithmName.SHA256);
            return request.CreateSelfSigned(DateTimeOffset.UtcNow.AddMinutes(-5), DateTimeOffset.UtcNow.AddDays(1));
        }
    }
}
