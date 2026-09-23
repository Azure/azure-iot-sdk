// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using System.Security.Cryptography;

namespace HsmAuthenticationSample
{
    /// <summary>
    /// Opens the device's private key from a PKCS#11 token (SoftHSM2) as a <b>native</b> OpenSSL key handle, so the
    /// key genuinely never leaves the token.
    /// <para>
    /// The key is opened through OpenSSL's PKCS#11 <b>provider</b> (or the legacy <c>engine_pkcs11</c> ENGINE) as a
    /// <see cref="SafeEvpPKeyHandle"/> and wrapped in an <see cref="RSAOpenSsl"/>. That handle only <i>references</i>
    /// the token key: the provider/engine performs every signing operation inside the token, so no private-key bytes
    /// are ever read into managed memory. Because the returned key is an <see cref="RSAOpenSsl"/>,
    /// <see cref="RSACertificateExtensions.CopyWithPrivateKey"/> binds it to a certificate by duplicating the handle
    /// (an <c>EVP_PKEY_up_ref</c>) instead of exporting the key.
    /// </para>
    /// <para>
    /// This is deliberately <b>not</b> a managed custom <see cref="RSA"/>: on every OS, <c>CopyWithPrivateKey</c>
    /// extracts the key material from a non-<see cref="RSAOpenSsl"/> key via <c>ExportParameters(true)</c>, which both
    /// defeats HSM custody and fails for a non-exportable token key.
    /// </para>
    /// <para>
    /// Configure it from the outputs of <c>c/eng/setup-softhsm.sh</c> (run it, then <c>eval</c> its exports). The
    /// signing PIN is read by the provider/engine from the URI's <c>pin-source</c>/<c>pin-value</c>:
    /// <list type="bullet">
    /// <item><c>AZ_IOT_CLIENT_KEY_URI</c> - an RFC 7512 URI, e.g.
    /// <c>pkcs11:token=aziot;object=device-key;type=private?pin-source=file:/tmp/token-pin</c>.</item>
    /// </list>
    /// This path uses OpenSSL and therefore runs on Linux/macOS only; on Windows the key must instead be surfaced
    /// through a CNG Key Storage Provider (see <c>Program.cs</c>).
    /// </para>
    /// </summary>
    internal static class SoftHsmKey
    {
        /// <summary>
        /// Opens the token-held RSA private key as a native OpenSSL key. The caller owns the returned
        /// <see cref="RSA"/> and must dispose it.
        /// </summary>
        public static RSA Open()
        {
            if (OperatingSystem.IsWindows())
            {
                throw new PlatformNotSupportedException(
                    "The OpenSSL PKCS#11 provider/engine is Linux/macOS only. On Windows, surface the key through a "
                    + "CNG Key Storage Provider and load the certificate from the store instead.");
            }

            string keyUri = Environment.GetEnvironmentVariable("AZ_IOT_CLIENT_KEY_URI")
                ?? Environment.GetEnvironmentVariable("AZ_IOT_TEST_PKCS11_KEY_URI")
                ?? throw new InvalidOperationException(
                    "AZ_IOT_CLIENT_KEY_URI is not set. Run c/eng/setup-softhsm.sh and 'eval' its exports first.");

            using SafeEvpPKeyHandle tokenKeyHandle = OpenHandle(keyUri);

            // RSAOpenSsl takes its own reference on the handle, so disposing the local handle above is safe.
            return new RSAOpenSsl(tokenKeyHandle);
        }

        private static SafeEvpPKeyHandle OpenHandle(string keyUri)
        {
            if (OperatingSystem.IsWindows())
            {
                throw new PlatformNotSupportedException("The OpenSSL PKCS#11 provider/engine is Linux/macOS only.");
            }

            // Prefer the OpenSSL 3 "pkcs11" provider (what c/eng/setup-pkcs11-provider.sh installs); fall back to the
            // legacy engine_pkcs11 ENGINE for environments that only expose that.
            try
            {
                return SafeEvpPKeyHandle.OpenKeyFromProvider("pkcs11", keyUri);
            }
            catch (Exception ex) when (
                ex is CryptographicException
                or PlatformNotSupportedException
                or DllNotFoundException
                or EntryPointNotFoundException)
            {
                return SafeEvpPKeyHandle.OpenPrivateKeyFromEngine("pkcs11", keyUri);
            }
        }
    }
}
