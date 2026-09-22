// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Net.Pkcs11Interop.Common;
using Net.Pkcs11Interop.HighLevelAPI;
using System.Security.Cryptography;

namespace Microsoft.Azure.Iot.Device.IntegrationTests
{
    /// <summary>
    /// An <see cref="RSA"/> implementation whose key material lives in a real PKCS#11 token -- in this test suite,
    /// a SoftHSM2 software token -- rather than in managed memory. This is the real-HSM analog of
    /// <c>MockHsmRsa</c>: instead of wrapping an in-memory <see cref="RSA"/> instance, every private-key operation
    /// is forwarded to the token via <see href="https://github.com/Pkcs11Interop/Pkcs11Interop">Pkcs11Interop</see>,
    /// exactly as the C SDK's e2e "key custody" tests do against the same kind of SoftHSM2 token (see
    /// <c>c/tests/e2e/tests/e2e_custody_test.c</c> and <c>c/eng/setup-softhsm.sh</c> in this repository).
    ///
    /// IMPORTANT -- the same .NET composition limitation documented on <see cref="X509AuthenticationProvider"/>
    /// and <c>MockHsmRsa</c> applies here too: <see cref="X509Certificate2.CopyWithPrivateKey(RSA)"/> calls
    /// <see cref="ExportParameters(bool)"/> with <c>includePrivateParameters: true</c> in order to bind this key to
    /// a certificate. If the token key was provisioned with <c>CKA_EXTRACTABLE=CK_FALSE</c> (the security posture a
    /// real HSM deployment should use), the token itself will refuse to disclose the private attributes, and
    /// <see cref="ExportParameters(bool)"/> throws <see cref="NotSupportedException"/> -- which in turn makes
    /// certificate composition fail, the same failure observed with a non-exportable native CNG key. This is a
    /// genuine constraint of .NET's current certificate/key composition APIs, not a bug in this class or in the
    /// token. For a test run against a token whose key is truly non-extractable, provision the token key with
    /// <c>CKA_EXTRACTABLE=CK_TRUE</c> (still never written to disk, still never leaving the token except through
    /// this narrow .NET-mandated export path) -- matching the trade-off <c>MockHsmRsa</c> documents -- or use the
    /// certificate-store-based, non-composed path (the original <see cref="X509AuthenticationProvider"/>
    /// constructor) instead.
    /// </summary>
    public sealed class SoftHsmRsaKey : RSA
    {
        private readonly IPkcs11Library _pkcs11Library;
        private readonly ISession _session;
        private readonly IObjectHandle _publicKeyHandle;
        private readonly IObjectHandle _privateKeyHandle;
        private readonly int _keySizeBits;
        private bool _disposed;

        private SoftHsmRsaKey(IPkcs11Library pkcs11Library, ISession session, IObjectHandle publicKeyHandle, IObjectHandle privateKeyHandle, int keySizeBits)
        {
            _pkcs11Library = pkcs11Library;
            _session = session;
            _publicKeyHandle = publicKeyHandle;
            _privateKeyHandle = privateKeyHandle;
            _keySizeBits = keySizeBits;
        }

        /// <summary>
        /// Opens a session against a SoftHSM2 (or any PKCS#11-compliant) token and locates the public/private RSA
        /// key pair identified by <paramref name="keyLabel"/>.
        /// </summary>
        /// <param name="pkcs11LibraryPath">
        /// Path to the PKCS#11 provider library (e.g. SoftHSM2's <c>libsofthsm2.so</c>/<c>.dll</c>).
        /// </param>
        /// <param name="tokenLabel">The label of the token that owns the key pair, as reported by the provider.</param>
        /// <param name="keyLabel">The shared <c>CKA_LABEL</c> of the public and private key objects.</param>
        /// <param name="userPin">The token's user PIN.</param>
        public static SoftHsmRsaKey Open(string pkcs11LibraryPath, string tokenLabel, string keyLabel, string userPin)
        {
            ArgumentException.ThrowIfNullOrWhiteSpace(pkcs11LibraryPath);
            ArgumentException.ThrowIfNullOrWhiteSpace(tokenLabel);
            ArgumentException.ThrowIfNullOrWhiteSpace(keyLabel);
            ArgumentException.ThrowIfNullOrWhiteSpace(userPin);

            var factories = new Pkcs11InteropFactories();
            IPkcs11Library pkcs11Library = factories.Pkcs11LibraryFactory.LoadPkcs11Library(factories, pkcs11LibraryPath, AppType.SingleThreaded);

            try
            {
                ISlot slot = pkcs11Library.GetSlotList(SlotsType.WithTokenPresent)
                    .FirstOrDefault(s => s.GetTokenInfo().Label == tokenLabel)
                    ?? throw new InvalidOperationException($"No PKCS#11 token with label '{tokenLabel}' was found via '{pkcs11LibraryPath}'.");

                ISession session = slot.OpenSession(SessionType.ReadOnly);
                try
                {
                    session.Login(CKU.CKU_USER, userPin);

                    IObjectHandle publicKeyHandle = FindKey(session, factories, CKO.CKO_PUBLIC_KEY, keyLabel);
                    IObjectHandle privateKeyHandle = FindKey(session, factories, CKO.CKO_PRIVATE_KEY, keyLabel);

                    List<IObjectAttribute> modulusAttribute = session.GetAttributeValue(publicKeyHandle, [CKA.CKA_MODULUS]);
                    int keySizeBits = modulusAttribute[0].GetValueAsByteArray().Length * 8;

                    return new SoftHsmRsaKey(pkcs11Library, session, publicKeyHandle, privateKeyHandle, keySizeBits);
                }
                catch
                {
                    session.CloseSession();
                    throw;
                }
            }
            catch
            {
                pkcs11Library.Dispose();
                throw;
            }
        }

        private static IObjectHandle FindKey(ISession session, Pkcs11InteropFactories factories, CKO objectClass, string keyLabel)
        {
            List<IObjectAttribute> template =
            [
                factories.ObjectAttributeFactory.Create(CKA.CKA_CLASS, objectClass),
                factories.ObjectAttributeFactory.Create(CKA.CKA_LABEL, keyLabel),
            ];

            List<IObjectHandle> matches = session.FindAllObjects(template);
            return matches.Count switch
            {
                0 => throw new InvalidOperationException($"No {objectClass} object labeled '{keyLabel}' was found on the token."),
                1 => matches[0],
                _ => throw new InvalidOperationException($"Multiple {objectClass} objects labeled '{keyLabel}' were found on the token; the label must be unique."),
            };
        }

        // The base KeySize setter validates against LegalKeySizes, which this wrapper never populates -- it holds
        // no local key material to size. The token-reported size is used instead.
        public override int KeySize => _keySizeBits;

        public override RSAParameters ExportParameters(bool includePrivateParameters)
        {
            List<IObjectAttribute> publicAttributes = _session.GetAttributeValue(_publicKeyHandle, [CKA.CKA_MODULUS, CKA.CKA_PUBLIC_EXPONENT]);

            var parameters = new RSAParameters
            {
                Modulus = publicAttributes[0].GetValueAsByteArray(),
                Exponent = publicAttributes[1].GetValueAsByteArray(),
            };

            if (!includePrivateParameters)
            {
                return parameters;
            }

            // See the class remarks: this is only reachable -- and only succeeds -- when the token key was
            // provisioned as CKA_EXTRACTABLE. A genuinely non-extractable token key makes the token refuse these
            // attributes, which surfaces here as CannotBeRead and is translated into NotSupportedException, exactly
            // as CopyWithPrivateKey requires callers to signal an unsupported export.
            List<CKA> privateAttributeTypes =
            [
                CKA.CKA_PRIVATE_EXPONENT,
                CKA.CKA_PRIME_1,
                CKA.CKA_PRIME_2,
                CKA.CKA_EXPONENT_1,
                CKA.CKA_EXPONENT_2,
                CKA.CKA_COEFFICIENT,
            ];

            List<IObjectAttribute> privateAttributes;
            try
            {
                privateAttributes = _session.GetAttributeValue(_privateKeyHandle, privateAttributeTypes);
            }
            catch (Pkcs11Exception ex)
            {
                throw new NotSupportedException(
                    "The token refused to disclose the private key attributes for this key. This is expected -- and " +
                    "the correct, secure outcome -- for a key provisioned with CKA_EXTRACTABLE=CK_FALSE. Use the " +
                    "certificate-store-based X509AuthenticationProvider constructor for such keys instead.", ex);
            }

            if (privateAttributes.Any(a => a.CannotBeRead))
            {
                throw new NotSupportedException(
                    "The token marked one or more private key attributes as unreadable for this key. This is expected " +
                    "-- and the correct, secure outcome -- for a key provisioned with CKA_EXTRACTABLE=CK_FALSE. Use the " +
                    "certificate-store-based X509AuthenticationProvider constructor for such keys instead.");
            }

            parameters.D = privateAttributes[0].GetValueAsByteArray();
            parameters.P = privateAttributes[1].GetValueAsByteArray();
            parameters.Q = privateAttributes[2].GetValueAsByteArray();
            parameters.DP = privateAttributes[3].GetValueAsByteArray();
            parameters.DQ = privateAttributes[4].GetValueAsByteArray();
            parameters.InverseQ = privateAttributes[5].GetValueAsByteArray();

            return parameters;
        }

        public override void ImportParameters(RSAParameters parameters)
        {
            throw new NotSupportedException("The HSM-backed key cannot be overwritten by imported key material.");
        }

        public override byte[] SignHash(byte[] hash, HashAlgorithmName hashAlgorithm, RSASignaturePadding padding)
        {
            ArgumentNullException.ThrowIfNull(hash);
            ArgumentNullException.ThrowIfNull(padding);

            if (padding != RSASignaturePadding.Pkcs1)
            {
                throw new CryptographicException("Only PKCS#1 v1.5 signature padding is supported by this token wrapper.");
            }

            // CKM_RSA_PKCS performs the raw RSA private-key operation plus PKCS#1 v1.5 padding, but expects the
            // caller to supply the DER-encoded DigestInfo (AlgorithmIdentifier + digest) rather than the bare hash
            // -- the same structure .NET's own PKCS#1 signing builds internally. Getting this wrong would produce
            // signatures no standard verifier (including .NET's own VerifyHash) accepts.
            byte[] digestInfo = BuildDigestInfo(hashAlgorithm, hash);

            var factories = new Pkcs11InteropFactories();
            IMechanism mechanism = factories.MechanismFactory.Create(CKM.CKM_RSA_PKCS);
            return _session.Sign(mechanism, _privateKeyHandle, digestInfo);
        }

        public override bool VerifyHash(byte[] hash, byte[] signature, HashAlgorithmName hashAlgorithm, RSASignaturePadding padding)
        {
            ArgumentNullException.ThrowIfNull(hash);
            ArgumentNullException.ThrowIfNull(signature);
            ArgumentNullException.ThrowIfNull(padding);

            // Verification only needs the public key, which never requires token access; do it entirely in-process.
            using RSA publicOnly = RSA.Create();
            publicOnly.ImportParameters(ExportParameters(false));
            return publicOnly.VerifyHash(hash, signature, hashAlgorithm, padding);
        }

        // DER-encoded DigestInfo prefixes for the hash algorithms the SDK's CSR/TLS signing paths use, per RFC 8017
        // (PKCS #1 v2.2) appendix. Each prefix is the ASN.1 encoding of the AlgorithmIdentifier preceding the raw
        // digest bytes.
        private static byte[] BuildDigestInfo(HashAlgorithmName hashAlgorithm, byte[] hash)
        {
            byte[] prefix = hashAlgorithm.Name switch
            {
                "SHA1" => [0x30, 0x21, 0x30, 0x09, 0x06, 0x05, 0x2b, 0x0e, 0x03, 0x02, 0x1a, 0x05, 0x00, 0x04, 0x14],
                "SHA256" => [0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20],
                "SHA384" => [0x30, 0x41, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x02, 0x05, 0x00, 0x04, 0x30],
                "SHA512" => [0x30, 0x51, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x03, 0x05, 0x00, 0x04, 0x40],
                _ => throw new CryptographicException($"Unsupported hash algorithm '{hashAlgorithm.Name}' for PKCS#11 RSA signing."),
            };

            return [.. prefix, .. hash];
        }

        protected override void Dispose(bool disposing)
        {
            if (disposing && !_disposed)
            {
                try
                {
                    _session.Logout();
                }
                catch (Pkcs11Exception)
                {
                    // Best-effort: the session/token may already be in a state where logout is a no-op or invalid.
                }

                _session.CloseSession();
                _pkcs11Library.Dispose();
                _disposed = true;
            }

            base.Dispose(disposing);
        }
    }
}
