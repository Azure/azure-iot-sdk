// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Net.Pkcs11Interop.Common;
using Net.Pkcs11Interop.HighLevelAPI;
using Net.Pkcs11Interop.HighLevelAPI.MechanismParams;
using System.Security.Cryptography;

namespace HsmAuthenticationSample
{
    /// <summary>
    /// An <see cref="IHardwareSecurityModule"/> backed by a real PKCS#11 token (SoftHSM2). The device private key is
    /// generated in, and never leaves, the token; only the public key and signature bytes cross the boundary.
    /// <para>
    /// This is the same module SoftHSM used by the SDK's <c>CertificateManagementIntegrationTests</c>. Configure it
    /// from the outputs of <c>c/eng/setup-softhsm.sh</c> (run it, then <c>eval</c> its exports):
    /// <list type="bullet">
    /// <item><c>PKCS11_PROVIDER_MODULE</c> - path to <c>libsofthsm2.so</c>.</item>
    /// <item><c>AZ_IOT_CLIENT_KEY_URI</c> - an RFC 7512 URI, e.g.
    /// <c>pkcs11:token=aziot;object=device-key;type=private?pin-source=file:/tmp/token-pin</c>.</item>
    /// </list>
    /// The token label, key label and PIN can also be supplied directly via
    /// <c>AZ_IOT_PKCS11_TOKEN_LABEL</c> / <c>AZ_IOT_PKCS11_KEY_LABEL</c> / <c>AZ_IOT_PKCS11_PIN</c>.
    /// </para>
    /// </summary>
    internal sealed class SoftHsmHardwareSecurityModule : IHardwareSecurityModule
    {
        // DigestInfo (DER) prefixes for RSASSA-PKCS1-v1_5, per RFC 8017 section 9.2.
        private static readonly byte[] Sha1DigestInfoPrefix =
            [0x30, 0x21, 0x30, 0x09, 0x06, 0x05, 0x2b, 0x0e, 0x03, 0x02, 0x1a, 0x05, 0x00, 0x04, 0x14];

        private static readonly byte[] Sha256DigestInfoPrefix =
            [0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20];

        private static readonly byte[] Sha384DigestInfoPrefix =
            [0x30, 0x41, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x02, 0x05, 0x00, 0x04, 0x30];

        private static readonly byte[] Sha512DigestInfoPrefix =
            [0x30, 0x51, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x03, 0x05, 0x00, 0x04, 0x40];

        private readonly IPkcs11Library _library;
        private readonly ISession _session;
        private readonly IObjectHandle _privateKeyHandle;
        private readonly Pkcs11InteropFactories _factories;
        private readonly RSAParameters _publicKey;

        private SoftHsmHardwareSecurityModule(
            IPkcs11Library library,
            ISession session,
            IObjectHandle privateKeyHandle,
            Pkcs11InteropFactories factories,
            RSAParameters publicKey)
        {
            _library = library;
            _session = session;
            _privateKeyHandle = privateKeyHandle;
            _factories = factories;
            _publicKey = publicKey;
        }

        /// <summary>
        /// Opens the SoftHSM token described by the setup script's environment output. Throws when
        /// <c>PKCS11_PROVIDER_MODULE</c> is not set, i.e. when the SoftHSM token has not been provisioned.
        /// </summary>
        public static SoftHsmHardwareSecurityModule Create()
        {
            string modulePath = Environment.GetEnvironmentVariable("PKCS11_PROVIDER_MODULE")
                ?? throw new InvalidOperationException(
                    "PKCS11_PROVIDER_MODULE is not set. Run c/eng/setup-softhsm.sh and 'eval' its exports first.");
            (string? uriToken, string? uriObject, string? pinSourcePath) = ParseKeyUri(
                Environment.GetEnvironmentVariable("AZ_IOT_CLIENT_KEY_URI")
                ?? Environment.GetEnvironmentVariable("AZ_IOT_TEST_PKCS11_KEY_URI"));

            string tokenLabel = uriToken
                ?? Environment.GetEnvironmentVariable("AZ_IOT_PKCS11_TOKEN_LABEL")
                ?? "aziot";
            string keyLabel = uriObject
                ?? Environment.GetEnvironmentVariable("AZ_IOT_PKCS11_KEY_LABEL")
                ?? "device-key";
            string pin = ReadPin(pinSourcePath)
                ?? Environment.GetEnvironmentVariable("AZ_IOT_PKCS11_PIN")
                ?? "1234";

            var factories = new Pkcs11InteropFactories();
            IPkcs11Library library = factories.Pkcs11LibraryFactory.LoadPkcs11Library(
                factories,
                modulePath,
                AppType.MultiThreaded);

            ISession? session = null;
            try
            {
                ISlot slot = library.GetSlotList(SlotsType.WithTokenPresent)
                    .FirstOrDefault(s => string.Equals(s.GetTokenInfo().Label?.Trim(), tokenLabel, StringComparison.Ordinal))
                    ?? throw new InvalidOperationException($"No PKCS#11 token labeled '{tokenLabel}' was found.");

                session = slot.OpenSession(SessionType.ReadOnly);
                session.Login(CKU.CKU_USER, pin);

                IObjectHandle privateKeyHandle = session.FindAllObjects(
                    [
                        factories.ObjectAttributeFactory.Create(CKA.CKA_CLASS, CKO.CKO_PRIVATE_KEY),
                        factories.ObjectAttributeFactory.Create(CKA.CKA_KEY_TYPE, CKK.CKK_RSA),
                        factories.ObjectAttributeFactory.Create(CKA.CKA_LABEL, keyLabel),
                    ]).FirstOrDefault()
                    ?? throw new InvalidOperationException(
                        $"No RSA private key labeled '{keyLabel}' was found in token '{tokenLabel}'.");

                RSAParameters publicKey = ReadPublicKey(session, factories, keyLabel, privateKeyHandle);

                return new SoftHsmHardwareSecurityModule(library, session, privateKeyHandle, factories, publicKey);
            }
            catch
            {
                session?.Dispose();
                library.Dispose();
                throw;
            }
        }

        public RSAParameters ExportPublicKey()
        {
            return new RSAParameters
            {
                Modulus = (byte[])_publicKey.Modulus!.Clone(),
                Exponent = (byte[])_publicKey.Exponent!.Clone(),
            };
        }

        public byte[] SignHash(byte[] hash, HashAlgorithmName hashAlgorithm, RSASignaturePadding padding)
        {
            IMechanism mechanism;
            byte[] dataToSign;

            if (padding == RSASignaturePadding.Pkcs1)
            {
                // CKM_RSA_PKCS signs a caller-supplied DigestInfo, so prepend the algorithm's DER prefix.
                mechanism = _factories.MechanismFactory.Create(CKM.CKM_RSA_PKCS);
                dataToSign = [.. GetDigestInfoPrefix(hashAlgorithm), .. hash];
            }
            else if (padding == RSASignaturePadding.Pss)
            {
                ICkRsaPkcsPssParams pssParams = _factories.MechanismParamsFactory.CreateCkRsaPkcsPssParams(
                    (ulong)GetPssHashMechanism(hashAlgorithm),
                    (ulong)GetPssMaskGenerationFunction(hashAlgorithm),
                    (ulong)hash.Length);
                mechanism = _factories.MechanismFactory.Create(CKM.CKM_RSA_PKCS_PSS, pssParams);
                dataToSign = hash;
            }
            else
            {
                throw new NotSupportedException($"Unsupported signature padding '{padding}'.");
            }

            return _session.Sign(mechanism, _privateKeyHandle, dataToSign);
        }

        public void Dispose()
        {
            _session.Dispose();
            _library.Dispose();
        }

        // Reads the token-held public key. Prefers the matching public-key object; if the token only exposes the
        // private-key object, RSA private keys still carry CKA_MODULUS and CKA_PUBLIC_EXPONENT in PKCS#11.
        private static RSAParameters ReadPublicKey(
            ISession session,
            Pkcs11InteropFactories factories,
            string keyLabel,
            IObjectHandle privateKeyHandle)
        {
            IObjectHandle? publicKeyHandle = session.FindAllObjects(
                [
                    factories.ObjectAttributeFactory.Create(CKA.CKA_CLASS, CKO.CKO_PUBLIC_KEY),
                    factories.ObjectAttributeFactory.Create(CKA.CKA_KEY_TYPE, CKK.CKK_RSA),
                    factories.ObjectAttributeFactory.Create(CKA.CKA_LABEL, keyLabel),
                ]).FirstOrDefault();

            IObjectHandle source = publicKeyHandle ?? privateKeyHandle;
            List<IObjectAttribute> attributes = session.GetAttributeValue(
                source,
                [CKA.CKA_MODULUS, CKA.CKA_PUBLIC_EXPONENT]);

            return new RSAParameters
            {
                Modulus = attributes[0].GetValueAsByteArray(),
                Exponent = attributes[1].GetValueAsByteArray(),
            };
        }

        private static string? ReadPin(string? pinSourcePath)
        {
            if (string.IsNullOrEmpty(pinSourcePath) || !File.Exists(pinSourcePath))
            {
                return null;
            }

            return File.ReadAllText(pinSourcePath).Trim();
        }

        // A minimal parser for the subset of RFC 7512 pkcs11: URIs emitted by the setup script.
        private static (string? Token, string? Object, string? PinSourcePath) ParseKeyUri(string? uri)
        {
            if (string.IsNullOrEmpty(uri) || !uri.StartsWith("pkcs11:", StringComparison.Ordinal))
            {
                return (null, null, null);
            }

            string body = uri.Substring("pkcs11:".Length);
            string pathPart = body;
            string queryPart = string.Empty;

            int queryIndex = body.IndexOf('?');
            if (queryIndex >= 0)
            {
                pathPart = body.Substring(0, queryIndex);
                queryPart = body.Substring(queryIndex + 1);
            }

            Dictionary<string, string> pathAttributes = SplitAttributes(pathPart, ';');
            Dictionary<string, string> queryAttributes = SplitAttributes(queryPart, '&');

            string? pinSource = null;
            if (queryAttributes.TryGetValue("pin-source", out string? rawPinSource))
            {
                pinSource = rawPinSource.StartsWith("file:", StringComparison.Ordinal)
                    ? rawPinSource.Substring("file:".Length)
                    : rawPinSource;
            }

            return (
                pathAttributes.GetValueOrDefault("token"),
                pathAttributes.GetValueOrDefault("object"),
                pinSource);
        }

        private static Dictionary<string, string> SplitAttributes(string value, char separator)
        {
            var attributes = new Dictionary<string, string>(StringComparer.Ordinal);
            if (string.IsNullOrEmpty(value))
            {
                return attributes;
            }

            foreach (string pair in value.Split(separator, StringSplitOptions.RemoveEmptyEntries))
            {
                int equalsIndex = pair.IndexOf('=');
                if (equalsIndex <= 0)
                {
                    continue;
                }

                string key = pair.Substring(0, equalsIndex);
                string attributeValue = Uri.UnescapeDataString(pair.Substring(equalsIndex + 1));
                attributes[key] = attributeValue;
            }

            return attributes;
        }

        private static byte[] GetDigestInfoPrefix(HashAlgorithmName hashAlgorithm)
        {
            return hashAlgorithm.Name switch
            {
                nameof(HashAlgorithmName.SHA1) => Sha1DigestInfoPrefix,
                nameof(HashAlgorithmName.SHA256) => Sha256DigestInfoPrefix,
                nameof(HashAlgorithmName.SHA384) => Sha384DigestInfoPrefix,
                nameof(HashAlgorithmName.SHA512) => Sha512DigestInfoPrefix,
                _ => throw new NotSupportedException($"Unsupported hash algorithm '{hashAlgorithm.Name}'."),
            };
        }

        private static CKM GetPssHashMechanism(HashAlgorithmName hashAlgorithm)
        {
            return hashAlgorithm.Name switch
            {
                nameof(HashAlgorithmName.SHA1) => CKM.CKM_SHA_1,
                nameof(HashAlgorithmName.SHA256) => CKM.CKM_SHA256,
                nameof(HashAlgorithmName.SHA384) => CKM.CKM_SHA384,
                nameof(HashAlgorithmName.SHA512) => CKM.CKM_SHA512,
                _ => throw new NotSupportedException($"Unsupported hash algorithm '{hashAlgorithm.Name}'."),
            };
        }

        private static CKG GetPssMaskGenerationFunction(HashAlgorithmName hashAlgorithm)
        {
            return hashAlgorithm.Name switch
            {
                nameof(HashAlgorithmName.SHA1) => CKG.CKG_MGF1_SHA1,
                nameof(HashAlgorithmName.SHA256) => CKG.CKG_MGF1_SHA256,
                nameof(HashAlgorithmName.SHA384) => CKG.CKG_MGF1_SHA384,
                nameof(HashAlgorithmName.SHA512) => CKG.CKG_MGF1_SHA512,
                _ => throw new NotSupportedException($"Unsupported hash algorithm '{hashAlgorithm.Name}'."),
            };
        }
    }
}
