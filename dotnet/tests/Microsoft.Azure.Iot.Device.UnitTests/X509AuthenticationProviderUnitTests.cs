// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Authentication;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using Xunit;

namespace Microsoft.Azure.Iot.Device.UnitTests
{
    public class X509AuthenticationProviderUnitTests
    {
        [Theory]
        [InlineData(0, true)]
        [InlineData(1, false)]
        [InlineData(2, false)]
        public async Task SecondaryCertificateFallbackDependsOnWhetherIssuedCertificatesArePresented(
            int constructorKind,
            bool expectsHubFallbackAfterIssuance)
        {
            using RSA primaryKey = RSA.Create(2048);
            using RSA secondaryKey = RSA.Create(2048);
            using RSA issuedKey = RSA.Create(2048);
            using X509Certificate2 primaryCertificate = CreateCertificate("primary", primaryKey);
            using X509Certificate2 secondaryCertificate = CreateCertificate("secondary", secondaryKey);
            using X509Certificate2 issuedCertificate = CreateCertificate("issued", issuedKey);
            string[] issuedChain = new[] { Convert.ToBase64String(issuedCertificate.RawData) };

            X509AuthenticationProvider authentication = constructorKind switch
            {
                0 => new X509AuthenticationProvider(primaryCertificate, secondaryCertificate),
                1 => new X509AuthenticationProvider(primaryCertificate, secondaryCertificate, issuedKey),
                2 => new X509AuthenticationProvider(primaryCertificate, secondaryCertificate, _ => Task.FromResult(issuedCertificate)),
                _ => throw new ArgumentOutOfRangeException(nameof(constructorKind)),
            };

            Assert.Same(secondaryCertificate, (await authentication.GetSecondaryBootAuthenticationCredentialsAsync(TestContext.Current.CancellationToken))!.ClientCertificate);
            Assert.Same(
                secondaryCertificate,
                (await authentication.GetSecondaryOperationalAuthenticationCredentialsAsync(
                    "some-hub.azure-devices.net", "device-id", null, TestContext.Current.CancellationToken))!.ClientCertificate);

            X509Certificate2 operationalCertificate = (await authentication.GetPrimaryOperationalAuthenticationCredentialsAsync(
                "some-hub.azure-devices.net", "device-id", issuedChain, TestContext.Current.CancellationToken)).ClientCertificate!;
            Assert.Equal(
                constructorKind == 0 ? primaryCertificate.Thumbprint : issuedCertificate.Thumbprint,
                operationalCertificate.Thumbprint);

            AuthenticationCredentials? issuedSecondaryCredentials = await authentication.GetSecondaryOperationalAuthenticationCredentialsAsync(
                "some-hub.azure-devices.net", "device-id", issuedChain, TestContext.Current.CancellationToken);
            if (expectsHubFallbackAfterIssuance)
            {
                Assert.Same(secondaryCertificate, issuedSecondaryCredentials!.ClientCertificate);
            }
            else
            {
                Assert.Null(issuedSecondaryCredentials);
            }
        }

        private static X509Certificate2 CreateCertificate(string commonName, RSA key)
        {
            CertificateRequest request = new($"CN={commonName}", key, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);
            return request.CreateSelfSigned(DateTimeOffset.UtcNow.AddDays(-1), DateTimeOffset.UtcNow.AddDays(1));
        }
    }
}
