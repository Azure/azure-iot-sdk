// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using Xunit;

namespace Microsoft.Azure.Iot.Device.IntegrationTests
{
    /// <summary>
    /// Fast, offline checks that <see cref="MockHsmRsa"/> forwards signing (and the private-key export
    /// composition requires -- see <see cref="MockHsmRsa"/>'s remarks) to the key it wraps, refuses key
    /// replacement, and composes correctly with <see cref="X509AuthenticationProvider"/>. These do not require
    /// DPS/Hub connectivity, unlike <see cref="Unified.CertificateManagementIntegrationTests"/>.
    /// </summary>
    public class MockHsmRsaTests
    {
        [Fact]
        public void ImportingParameters_Throws()
        {
            using RSA hsmHeldKey = RSA.Create(2048);
            using var mockHsmKey = new MockHsmRsa(hsmHeldKey);

            Assert.Throws<NotSupportedException>(() => mockHsmKey.ImportParameters(hsmHeldKey.ExportParameters(true)));
        }

        [Fact]
        public void ExportingThePublicKey_Succeeds()
        {
            using RSA hsmHeldKey = RSA.Create(2048);
            using var mockHsmKey = new MockHsmRsa(hsmHeldKey);

            RSAParameters publicParameters = mockHsmKey.ExportParameters(false);

            Assert.Equal(hsmHeldKey.ExportParameters(false).Modulus, publicParameters.Modulus);
        }

        [Fact]
        public void SigningThroughTheMock_ProducesAVerifiableSignature()
        {
            using RSA hsmHeldKey = RSA.Create(2048);
            using var mockHsmKey = new MockHsmRsa(hsmHeldKey);

            byte[] data = { 1, 2, 3, 4, 5 };
            byte[] signature = mockHsmKey.SignData(data, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);

            Assert.True(hsmHeldKey.VerifyData(data, signature, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1));
        }

        [Fact]
        public void CsrGeneratedThroughTheMock_IsSignedByTheHsmHeldKey()
        {
            using RSA hsmHeldKey = RSA.Create(2048);
            using var mockHsmKey = new MockHsmRsa(hsmHeldKey);

            string csrBase64 = CertificateUtilities.GenerateCsrWithPrivateKey("mock-hsm-device", mockHsmKey);

            var csr = CertificateRequest.LoadSigningRequest(
                Convert.FromBase64String(csrBase64),
                HashAlgorithmName.SHA256,
                CertificateRequestLoadOptions.Default,
                RSASignaturePadding.Pkcs1);

            // The CSR verifies against the HSM-held key's public half -- proof the mock signed with the real key,
            // not merely that CreateSigningRequest() did not throw.
            using RSA csrPublicKey = csr.PublicKey.GetRSAPublicKey()!;
            Assert.Equal(hsmHeldKey.ExportParameters(false).Modulus, csrPublicKey.ExportParameters(false).Modulus);
        }

        [Fact]
        public void ComposedWithAPublicOnlyCertificate_TheResultSignsAsTheHsmHeldKeyWould()
        {
            using RSA hsmHeldKey = RSA.Create(2048);
            using var mockHsmKey = new MockHsmRsa(hsmHeldKey);

            var certificateRequest = new CertificateRequest(
                "CN=mock-hsm-device",
                mockHsmKey,
                HashAlgorithmName.SHA256,
                RSASignaturePadding.Pkcs1);
            using X509Certificate2 certificateWithKey = certificateRequest.CreateSelfSigned(
                DateTimeOffset.UtcNow.AddDays(-1), DateTimeOffset.UtcNow.AddDays(1));
            using X509Certificate2 publicOnlyCertificate = X509CertificateLoader.LoadCertificate(
                certificateWithKey.Export(X509ContentType.Cert));

            var authentication = new X509AuthenticationProvider(publicOnlyCertificate, mockHsmKey);

            Assert.True(authentication.ClientCertificate.HasPrivateKey);

            byte[] data = { 9, 8, 7, 6 };
            byte[] signature = authentication.ClientCertificate
                .GetRSAPrivateKey()!
                .SignData(data, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);

            Assert.True(hsmHeldKey.VerifyData(data, signature, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1));
        }
    }
}
