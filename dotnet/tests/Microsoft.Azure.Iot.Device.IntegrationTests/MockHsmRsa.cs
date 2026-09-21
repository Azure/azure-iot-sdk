// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;

namespace Microsoft.Azure.Iot.Device.IntegrationTests
{
    /// <summary>
    /// Stands in for a real hardware security module (HSM) in tests that exercise the HSM-backed
    /// <see cref="X509AuthenticationProvider"/> constructors, since CI has no physical HSM/TPM/PKCS#11 token
    /// available. It wraps an ordinary in-memory RSA key and forwards signing (and, for composition, one
    /// necessary export path -- see the note below) to it. Callers only ever obtain this wrapper, never the
    /// hidden key itself.
    ///
    /// IMPORTANT, and the reason this is a *mock* rather than a template for a real HSM adapter:
    /// <see cref="X509Certificate2.CopyWithPrivateKey(RSA)"/> -- used internally by the HSM-backed
    /// <see cref="X509AuthenticationProvider"/> constructors to bind a key to a certificate -- calls
    /// <see cref="ExportParameters(bool)"/> with <c>includePrivateParameters: true</c> as part of composing the
    /// certificate, on every platform this was verified against, even for a native CNG key created with
    /// <c>CngExportPolicies.None</c>. There is currently no supported way in .NET to compose a certificate with a
    /// key whose <c>ExportParameters(true)</c> call itself refuses to complete. Genuinely hardware-enforced,
    /// non-exportable keys (e.g. a TPM/HSM-backed CNG key on Windows) are instead consumed by loading the
    /// certificate directly from the platform certificate store, where the OS has already associated the
    /// non-exportable key with the certificate -- that path needs no composition at all, and is already served by
    /// the original, certificate-only <see cref="X509AuthenticationProvider"/> constructor.
    ///
    /// What this mock -- and the HSM-backed constructors -- remain useful for is key material that is held
    /// separately from the certificate (so it is never written to a PFX/PEM file alongside it) but whose owning
    /// SDK does support the parameter export .NET's composition step requires, such as many PKCS#11 provider
    /// wrappers and software key-storage providers. <see cref="ImportParameters"/> is still refused: overwriting
    /// the key material of a key an HSM is meant to own would never be legitimate.
    /// </summary>
    public sealed class MockHsmRsa : RSA
    {
        private readonly RSA _hsmHeldKey;
        private bool _disposed;

        public MockHsmRsa(RSA hsmHeldKey)
        {
            ArgumentNullException.ThrowIfNull(hsmHeldKey);

            _hsmHeldKey = hsmHeldKey;
        }

        // The base KeySize setter validates against LegalKeySizes, which this wrapper never populates (it holds
        // no key material of its own to size). Reporting the HSM-held key's size directly sidesteps that and
        // keeps this property accurate for callers that inspect it (e.g. logging, diagnostics).
        public override int KeySize => _hsmHeldKey.KeySize;

        public override RSAParameters ExportParameters(bool includePrivateParameters)
        {
            // See the class remarks: exporting the private parameters here is what lets this wrapper be composed
            // with a certificate via CopyWithPrivateKey at all -- it is not a gap in the mock, it is a limitation
            // of that composition API that a real, hardware-enforced non-exportable key cannot satisfy.
            return _hsmHeldKey.ExportParameters(includePrivateParameters);
        }

        public override void ImportParameters(RSAParameters parameters)
        {
            throw new NotSupportedException("The (mock) HSM-backed key cannot be overwritten by imported key material.");
        }

        public override byte[] Encrypt(byte[] data, RSAEncryptionPadding padding)
        {
            return _hsmHeldKey.Encrypt(data, padding);
        }

        public override byte[] Decrypt(byte[] data, RSAEncryptionPadding padding)
        {
            // Decryption, like signing, is delegated to the HSM-held key rather than performed against exported
            // key material.
            return _hsmHeldKey.Decrypt(data, padding);
        }

        public override byte[] SignHash(byte[] hash, HashAlgorithmName hashAlgorithm, RSASignaturePadding padding)
        {
            // The one operation this class exists to allow: signing without ever exposing the private key.
            return _hsmHeldKey.SignHash(hash, hashAlgorithm, padding);
        }

        public override bool VerifyHash(byte[] hash, byte[] signature, HashAlgorithmName hashAlgorithm, RSASignaturePadding padding)
        {
            return _hsmHeldKey.VerifyHash(hash, signature, hashAlgorithm, padding);
        }

        protected override void Dispose(bool disposing)
        {
            if (disposing && !_disposed)
            {
                _hsmHeldKey.Dispose();
                _disposed = true;
            }

            base.Dispose(disposing);
        }
    }
}
