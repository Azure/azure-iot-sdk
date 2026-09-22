// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using System.Security.Cryptography;

namespace HsmAuthenticationSample
{
    /// <summary>
    /// A stand-in for a real HSM so that this sample is runnable on any machine. It generates an RSA key pair and
    /// keeps the private key entirely inside this object, exposing only the public key and a signing operation.
    /// <para>
    /// On a real device, replace this with an implementation that talks to your module. For example, key generation
    /// maps to a PKCS#11 <c>C_GenerateKeyPair</c> with the private key marked non-extractable, and
    /// <see cref="SignHash"/> maps to <c>C_Sign</c>. Either way the private key never leaves the hardware.
    /// </para>
    /// </summary>
    internal sealed class SimulatedHardwareSecurityModule : IHardwareSecurityModule
    {
        // Represents the key pair generated on, and sealed inside, the HSM. In real hardware this handle would be a
        // PKCS#11 object handle, not managed key bytes.
        private readonly RSA _tokenKey;

        public SimulatedHardwareSecurityModule(int keySizeInBits = 2048)
        {
            _tokenKey = RSA.Create(keySizeInBits);
        }

        public RSAParameters ExportPublicKey()
        {
            return _tokenKey.ExportParameters(includePrivateParameters: false);
        }

        public byte[] SignHash(byte[] hash, HashAlgorithmName hashAlgorithm, RSASignaturePadding padding)
        {
            return _tokenKey.SignHash(hash, hashAlgorithm, padding);
        }

        public void Dispose()
        {
            _tokenKey.Dispose();
        }
    }
}
