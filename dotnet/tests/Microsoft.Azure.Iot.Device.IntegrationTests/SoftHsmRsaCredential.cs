// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using Xunit;

namespace Microsoft.Azure.Iot.Device.IntegrationTests
{
    /// <summary>
    /// Builds an <see cref="X509AuthenticationProvider"/> whose device private key lives inside a PKCS#11 token
    /// (SoftHSM2 in CI) and genuinely never leaves it, using the outputs of <c>c/eng/setup-softhsm.sh</c>.
    /// <para>
    /// The key is opened through OpenSSL's PKCS#11 <b>provider</b> (or the legacy <c>engine_pkcs11</c> ENGINE) as a
    /// native <see cref="SafeEvpPKeyHandle"/>. That handle only <i>references</i> the token key: the provider/engine
    /// performs the signing inside the token, so the private key is never exported to managed memory. Binding that
    /// native handle with <see cref="RSACertificateExtensions.CopyWithPrivateKey"/> takes the
    /// <see cref="RSAOpenSsl"/> fast path, which duplicates the handle by reference (an <c>EVP_PKEY_up_ref</c>)
    /// instead of calling <c>ExportParameters(true)</c>.
    /// </para>
    /// <para>
    /// This deliberately does <b>not</b> use a managed custom <see cref="RSA"/> shim: on every OS,
    /// <c>CopyWithPrivateKey</c> extracts the key material from a non-<see cref="RSAOpenSsl"/> key via
    /// <c>ExportParameters(true)</c>, which both defeats HSM custody and fails for a non-exportable token key.
    /// </para>
    /// <para>
    /// Consumes the setup script's environment output:
    /// <list type="bullet">
    /// <item><c>PKCS11_PROVIDER_MODULE</c> - path to <c>libsofthsm2.so</c> (used only as the "SoftHSM is provisioned"
    /// gate; the OpenSSL provider itself is configured through <c>OPENSSL_MODULES</c> / <c>openssl.cnf</c>).</item>
    /// <item><c>AZ_IOT_CLIENT_KEY_URI</c> - an RFC 7512 URI, e.g.
    /// <c>pkcs11:token=aziot;object=device-key;type=private?pin-source=file:/tmp/token-pin</c>. The PIN is read by the
    /// provider from the URI's <c>pin-source</c>/<c>pin-value</c>.</item>
    /// </list>
    /// The device certificate is public and is still supplied as PEM; only the key is custodial.
    /// </para>
    /// </summary>
    public sealed class SoftHsmRsaCredential : IDisposable
    {
        private SoftHsmRsaCredential(X509Certificate2 certificate)
        {
            Certificate = certificate;
        }

        /// <summary>
        /// The device certificate whose private key is the token-held key. The certificate owns its own duplicated
        /// native key handle, so it remains usable for the lifetime of this credential.
        /// </summary>
        public X509Certificate2 Certificate { get; }

        public X509AuthenticationProvider CreateAuthenticationProvider() => new(Certificate);

        /// <summary>
        /// Skips the calling test when the SoftHSM-backed X.509 path cannot run on the current machine. CI runs the
        /// integration tests on both Windows and Ubuntu, but this path is Linux/macOS only:
        /// <list type="bullet">
        /// <item>It relies on OpenSSL's PKCS#11 provider/engine (<see cref="SafeEvpPKeyHandle"/> +
        /// <see cref="RSAOpenSsl"/>), which do not exist on Windows (SChannel would need a CNG Key Storage
        /// Provider instead).</item>
        /// <item>SoftHSM is provisioned by <c>c/eng/setup-softhsm.sh</c> (a bash script emitting <c>libsofthsm2.so</c>),
        /// so <c>PKCS11_PROVIDER_MODULE</c> is only present where that script has run.</item>
        /// </list>
        /// </summary>
        public static void SkipIfUnsupported()
        {
            if (OperatingSystem.IsWindows())
            {
                Assert.Skip(
                    "SoftHSM-backed X.509 authentication runs on Linux/macOS only: it uses the OpenSSL PKCS#11 "
                    + "provider/engine, which is unavailable on Windows (use a CNG Key Storage Provider there).");
            }

            if (string.IsNullOrEmpty(Environment.GetEnvironmentVariable("PKCS11_PROVIDER_MODULE")))
            {
                Assert.Fail("SoftHSM token not provisioned. Run c/eng/setup-softhsm.sh and eval its exports first.");
            }
        }

        /// <summary>
        /// Opens the token-held private key described by the setup script's environment output and binds it to the
        /// supplied device certificate without ever exporting the key from the token.
        /// </summary>
        /// <param name="certificatePem">The device certificate PEM (public; matches the token's key).</param>
        public static SoftHsmRsaCredential Load(string certificatePem)
        {
            string keyUri = Environment.GetEnvironmentVariable("AZ_IOT_CLIENT_KEY_URI")
                ?? Environment.GetEnvironmentVariable("AZ_IOT_TEST_PKCS11_KEY_URI")
                ?? throw new InvalidOperationException(
                    "Missing AZ_IOT_CLIENT_KEY_URI. Run c/eng/setup-softhsm.sh and eval its exports first.");

            using X509Certificate2 publicCertificate = X509Certificate2.CreateFromPem(certificatePem);
            X509Certificate2 certificateWithKey = BindTokenKeyToCertificate(publicCertificate, keyUri);

            return new SoftHsmRsaCredential(certificateWithKey);
        }

        public void Dispose()
        {
            Certificate.Dispose();
        }

        // Opens the token key as a native EVP_PKEY and attaches it to the certificate. Because the key is an
        // RSAOpenSsl (a native OpenSSL handle), CopyWithPrivateKey duplicates the handle by reference rather than
        // exporting the private key, so the key stays in the token and the provider/engine signs the handshake.
        private static X509Certificate2 BindTokenKeyToCertificate(X509Certificate2 publicCertificate, string keyUri)
        {
            if (OperatingSystem.IsWindows())
            {
                throw new PlatformNotSupportedException(
                    "The OpenSSL PKCS#11 provider/engine is Linux/macOS only. Call SkipIfUnsupported() before Load.");
            }

            using SafeEvpPKeyHandle tokenKeyHandle = OpenTokenPrivateKey(keyUri);
            using RSA tokenKey = new RSAOpenSsl(tokenKeyHandle);

            return publicCertificate.CopyWithPrivateKey(tokenKey);
        }

        private static SafeEvpPKeyHandle OpenTokenPrivateKey(string keyUri)
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
