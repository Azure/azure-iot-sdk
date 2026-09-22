// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using System.Security.Cryptography;

namespace HsmAuthenticationSample
{
    /// <summary>
    /// An <see cref="RSA"/> whose private-key operations are delegated to an <see cref="IHardwareSecurityModule"/>.
    /// <para>
    /// The private key never exists in managed memory: this type holds only the public key plus a handle to the HSM,
    /// and forwards every signing request across the HSM boundary. .NET's TLS stack signs the client-authentication
    /// step of the handshake by calling into this object (rather than reading raw key bytes), which is why binding
    /// this key to the device certificate lets the device authenticate on any OS without the private key leaving the
    /// HSM. This is the same shape used by, for example, Azure Key Vault's <c>RSAKeyVault</c>.
    /// </para>
    /// </summary>
    internal sealed class HsmBackedRsa : RSA
    {
        private readonly IHardwareSecurityModule _hsm;
        private readonly RSAParameters _publicKey;

        public HsmBackedRsa(IHardwareSecurityModule hsm)
        {
            _hsm = hsm;
            _publicKey = hsm.ExportPublicKey();
            KeySizeValue = _publicKey.Modulus!.Length * 8;
        }

        public override RSAParameters ExportParameters(bool includePrivateParameters)
        {
            if (includePrivateParameters)
            {
                throw new CryptographicException("The private key is held in the HSM and cannot be exported.");
            }

            return new RSAParameters
            {
                Modulus = (byte[])_publicKey.Modulus!.Clone(),
                Exponent = (byte[])_publicKey.Exponent!.Clone(),
            };
        }

        public override void ImportParameters(RSAParameters parameters)
        {
            throw new NotSupportedException("An HSM-backed key cannot be imported.");
        }

        public override byte[] SignHash(byte[] hash, HashAlgorithmName hashAlgorithm, RSASignaturePadding padding)
        {
            return _hsm.SignHash(hash, hashAlgorithm, padding);
        }

        public override bool TrySignHash(
            ReadOnlySpan<byte> hash,
            Span<byte> destination,
            HashAlgorithmName hashAlgorithm,
            RSASignaturePadding padding,
            out int bytesWritten)
        {
            byte[] signature = _hsm.SignHash(hash.ToArray(), hashAlgorithm, padding);
            if (signature.Length > destination.Length)
            {
                bytesWritten = 0;
                return false;
            }

            signature.CopyTo(destination);
            bytesWritten = signature.Length;
            return true;
        }

        // Hashing is not secret, so it is done locally. Overriding these lets the base RSA.SignData compute the
        // digest before handing it to SignHash (and therefore to the HSM).
        protected override byte[] HashData(byte[] data, int offset, int count, HashAlgorithmName hashAlgorithm)
        {
            using IncrementalHash hasher = IncrementalHash.CreateHash(hashAlgorithm);
            hasher.AppendData(data, offset, count);
            return hasher.GetHashAndReset();
        }

        protected override byte[] HashData(Stream data, HashAlgorithmName hashAlgorithm)
        {
            using IncrementalHash hasher = IncrementalHash.CreateHash(hashAlgorithm);
            byte[] buffer = new byte[4096];
            int read;
            while ((read = data.Read(buffer, 0, buffer.Length)) > 0)
            {
                hasher.AppendData(buffer, 0, read);
            }

            return hasher.GetHashAndReset();
        }
    }
}
