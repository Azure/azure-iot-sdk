// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using System.Security.Cryptography;

namespace HsmAuthenticationSample
{
    /// <summary>
    /// Abstraction over the boundary of a hardware security module (HSM) that holds the device's private key.
    /// <para>
    /// Only public-key material and signing results ever cross this boundary; the private key stays inside the
    /// module. On a real device an implementation would wrap a PKCS#11 session (for example via Pkcs11Interop),
    /// a TPM, a secure element, or a cloud key such as Azure Key Vault.
    /// </para>
    /// </summary>
    internal interface IHardwareSecurityModule : IDisposable
    {
        /// <summary>
        /// Returns the public portion of the key pair (modulus and exponent only).
        /// </summary>
        RSAParameters ExportPublicKey();

        /// <summary>
        /// Signs the supplied hash inside the HSM and returns the signature. This is the only operation that uses
        /// the private key, and it never exposes the key material.
        /// </summary>
        byte[] SignHash(byte[] hash, HashAlgorithmName hashAlgorithm, RSASignaturePadding padding);
    }
}
