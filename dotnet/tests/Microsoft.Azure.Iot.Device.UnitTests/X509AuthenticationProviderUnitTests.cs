// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using System;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using Xunit;

namespace Microsoft.Azure.Iot.Device.UnitTests
{
    /// <summary>
    /// Tests for <see cref="X509AuthenticationProvider"/>, including the HSM-backed constructors that bind a
    /// public-only certificate to an externally held RSA/ECDsa private key (for example a CNG/TPM/HSM key handle)
    /// instead of requiring the certificate to carry the private key itself.
    /// </summary>
    public class X509AuthenticationProviderUnitTests
    {
        private const string RegistrationId = "someRegistrationId";

        [Fact]
        public void Constructor_NullCertificate_Throws()
        {
            Assert.Throws<ArgumentException>(() => new X509AuthenticationProvider(null!));
        }

        [Fact]
        public void Constructor_CertificateWithPrivateKey_UsesEmbeddedKey()
        {
            using RSA key = RSA.Create(2048);
            using X509Certificate2 certificateWithKey = CreateSelfSignedCertificate(key);

            var authentication = new X509AuthenticationProvider(certificateWithKey);

            Assert.True(authentication.ClientCertificate.HasPrivateKey);
        }

        [Fact]
        public void Constructor_Rsa_NullCertificate_Throws()
        {
            using RSA key = RSA.Create(2048);

            Assert.Throws<ArgumentException>(() => new X509AuthenticationProvider(null!, key));
        }

        [Fact]
        public void Constructor_Rsa_NullHsmKey_Throws()
        {
            using RSA key = RSA.Create(2048);
            using X509Certificate2 certificateWithKey = CreateSelfSignedCertificate(key);
            using X509Certificate2 publicOnlyCertificate = ToPublicOnlyCertificate(certificateWithKey);

            Assert.Throws<ArgumentException>(() => new X509AuthenticationProvider(publicOnlyCertificate, (RSA)null!));
        }

        [Fact]
        public void Constructor_Rsa_CertificateAlreadyHasPrivateKey_Throws()
        {
            using RSA key = RSA.Create(2048);
            using X509Certificate2 certificateWithKey = CreateSelfSignedCertificate(key);
            using RSA hsmKey = RSA.Create(2048);

            Assert.Throws<ArgumentException>(() => new X509AuthenticationProvider(certificateWithKey, hsmKey));
        }

        [Fact]
        public void Constructor_Rsa_ComposesPublicCertificateWithHsmKey()
        {
            // The public key embedded in the certificate has to match the HSM key's public half -- exactly like a
            // real device, whose certificate is issued for the public key the HSM already holds. What is under
            // test is that the certificate never needs its OWN copy of the private key: only the public-only
            // certificate and a reference to the HSM key are supplied here.
            using RSA hsmKey = RSA.Create(2048);
            using X509Certificate2 certificateWithKey = CreateSelfSignedCertificate(hsmKey);
            using X509Certificate2 publicOnlyCertificate = ToPublicOnlyCertificate(certificateWithKey);

            var authentication = new X509AuthenticationProvider(publicOnlyCertificate, hsmKey);

            Assert.True(authentication.ClientCertificate.HasPrivateKey);

            byte[] data = new byte[] { 1, 2, 3, 4 };
            byte[] signature = authentication.ClientCertificate
                .GetRSAPrivateKey()!
                .SignData(data, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);

            Assert.True(hsmKey.VerifyData(data, signature, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1));
        }

        [Fact]
        public void Constructor_ECDsa_NullCertificate_Throws()
        {
            using ECDsa key = ECDsa.Create(ECCurve.NamedCurves.nistP256);

            Assert.Throws<ArgumentException>(() => new X509AuthenticationProvider(null!, key));
        }

        [Fact]
        public void Constructor_ECDsa_NullHsmKey_Throws()
        {
            using ECDsa key = ECDsa.Create(ECCurve.NamedCurves.nistP256);
            using X509Certificate2 certificateWithKey = CreateSelfSignedCertificate(key);
            using X509Certificate2 publicOnlyCertificate = ToPublicOnlyCertificate(certificateWithKey);

            Assert.Throws<ArgumentException>(() => new X509AuthenticationProvider(publicOnlyCertificate, (ECDsa)null!));
        }

        [Fact]
        public void Constructor_ECDsa_CertificateAlreadyHasPrivateKey_Throws()
        {
            using ECDsa key = ECDsa.Create(ECCurve.NamedCurves.nistP256);
            using X509Certificate2 certificateWithKey = CreateSelfSignedCertificate(key);
            using ECDsa hsmKey = ECDsa.Create(ECCurve.NamedCurves.nistP256);

            Assert.Throws<ArgumentException>(() => new X509AuthenticationProvider(certificateWithKey, hsmKey));
        }

        [Fact]
        public void Constructor_ECDsa_ComposesPublicCertificateWithHsmKey()
        {
            using ECDsa hsmKey = ECDsa.Create(ECCurve.NamedCurves.nistP256);
            using X509Certificate2 certificateWithKey = CreateSelfSignedCertificate(hsmKey);
            using X509Certificate2 publicOnlyCertificate = ToPublicOnlyCertificate(certificateWithKey);

            var authentication = new X509AuthenticationProvider(publicOnlyCertificate, hsmKey);

            Assert.True(authentication.ClientCertificate.HasPrivateKey);

            byte[] data = new byte[] { 1, 2, 3, 4 };
            byte[] signature = authentication.ClientCertificate.GetECDsaPrivateKey()!.SignData(data, HashAlgorithmName.SHA256);

            Assert.True(hsmKey.VerifyData(data, signature, HashAlgorithmName.SHA256));
        }

        private static X509Certificate2 CreateSelfSignedCertificate(RSA key)
        {
            var certificateRequest = new CertificateRequest($"CN={RegistrationId}", key, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);
            return certificateRequest.CreateSelfSigned(DateTimeOffset.UtcNow.AddDays(-1), DateTimeOffset.UtcNow.AddDays(1));
        }

        private static X509Certificate2 CreateSelfSignedCertificate(ECDsa key)
        {
            var certificateRequest = new CertificateRequest($"CN={RegistrationId}", key, HashAlgorithmName.SHA256);
            return certificateRequest.CreateSelfSigned(DateTimeOffset.UtcNow.AddDays(-1), DateTimeOffset.UtcNow.AddDays(1));
        }

        // Strips the private key, leaving the certificate exactly as an HSM-backed caller would supply it: the
        // public certificate on its own, with the private key living only in the HSM key handle.
        private static X509Certificate2 ToPublicOnlyCertificate(X509Certificate2 certificateWithKey)
        {
            return X509CertificateLoader.LoadCertificate(certificateWithKey.Export(X509ContentType.Cert));
        }
    }
}
