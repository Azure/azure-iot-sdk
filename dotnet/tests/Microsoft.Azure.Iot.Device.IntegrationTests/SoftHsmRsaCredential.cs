// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Net.Pkcs11Interop.Common;
using Net.Pkcs11Interop.HighLevelAPI;
using Net.Pkcs11Interop.HighLevelAPI.MechanismParams;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;

namespace Microsoft.Azure.Iot.Device.IntegrationTests
{
    /// <summary>
    /// Builds an <see cref="X509AuthenticationProvider"/> whose device private key lives inside a PKCS#11 token
    /// (SoftHSM2 in CI) and never leaves it, using the outputs of <c>c/eng/setup-softhsm.sh</c>.
    /// <para>
    /// That script imports the device key (the one matching the DPS enrollment certificate) into a token and prints:
    /// <list type="bullet">
    /// <item><c>PKCS11_PROVIDER_MODULE</c> - path to <c>libsofthsm2.so</c>.</item>
    /// <item><c>AZ_IOT_CLIENT_KEY_URI</c> - an RFC 7512 URI, e.g.
    /// <c>pkcs11:token=aziot;object=device-key;type=private?pin-source=file:/tmp/token-pin</c>.</item>
    /// </list>
    /// The device certificate itself is public and is still supplied as PEM; only the key is custodial. All signing
    /// during the TLS handshake is performed by the token through <see cref="Pkcs11Rsa"/>.
    /// </para>
    /// </summary>
    public sealed class SoftHsmRsaCredential : IDisposable
    {
        private readonly IPkcs11Library _library;
        private readonly ISession _session;

        private SoftHsmRsaCredential(IPkcs11Library library, ISession session, X509Certificate2 certificate)
        {
            _library = library;
            _session = session;
            Certificate = certificate;
        }

        /// <summary>
        /// The device certificate bound to the token-held private key. Keep this credential alive for as long as the
        /// certificate is in use: disposing it closes the PKCS#11 session the certificate signs through.
        /// </summary>
        public X509Certificate2 Certificate { get; }

        public X509AuthenticationProvider CreateAuthenticationProvider() => new(Certificate);

        /// <summary>
        /// Opens the SoftHSM token described by the setup script's environment output and binds the token-held private
        /// key to the supplied device certificate.
        /// </summary>
        /// <param name="certificatePem">The device certificate PEM (public; matches the token's key).</param>
        public static SoftHsmRsaCredential Load(string certificatePem)
        {
            string modulePath = Environment.GetEnvironmentVariable("PKCS11_PROVIDER_MODULE")
                ?? throw new InvalidOperationException(
                    "Missing PKCS11_PROVIDER_MODULE. Run c/eng/setup-softhsm.sh and eval its exports first.");

            string? keyUri = Environment.GetEnvironmentVariable("AZ_IOT_CLIENT_KEY_URI")
                ?? Environment.GetEnvironmentVariable("AZ_IOT_TEST_PKCS11_KEY_URI");
            Pkcs11KeyUri parsed = Pkcs11KeyUri.Parse(keyUri);

            string tokenLabel = parsed.Token
                ?? Environment.GetEnvironmentVariable("AZ_IOT_PKCS11_TOKEN_LABEL")
                ?? "aziot";
            string keyLabel = parsed.Object
                ?? Environment.GetEnvironmentVariable("AZ_IOT_PKCS11_KEY_LABEL")
                ?? "device-key";
            string pin = ReadPin(parsed.PinSourcePath)
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

                var searchTemplate = new List<IObjectAttribute>
                {
                    factories.ObjectAttributeFactory.Create(CKA.CKA_CLASS, CKO.CKO_PRIVATE_KEY),
                    factories.ObjectAttributeFactory.Create(CKA.CKA_KEY_TYPE, CKK.CKK_RSA),
                    factories.ObjectAttributeFactory.Create(CKA.CKA_LABEL, keyLabel),
                };

                IObjectHandle privateKeyHandle = session.FindAllObjects(searchTemplate).FirstOrDefault()
                    ?? throw new InvalidOperationException(
                        $"No RSA private key labeled '{keyLabel}' was found in token '{tokenLabel}'.");

                // The certificate is public; its RSA public key is the counterpart of the token-held private key.
                using X509Certificate2 publicCertificate = X509Certificate2.CreateFromPem(certificatePem);
                RSAParameters publicParameters;
                using (RSA certPublicKey = publicCertificate.GetRSAPublicKey()
                    ?? throw new InvalidOperationException("The device certificate does not carry an RSA public key."))
                {
                    publicParameters = certPublicKey.ExportParameters(includePrivateParameters: false);
                }

                var tokenBackedKey = new Pkcs11Rsa(session, privateKeyHandle, factories, publicParameters);

                // On the OpenSSL-based stacks (Linux/macOS, which is where SoftHSM runs) CopyWithPrivateKey keeps a
                // reference to the managed token-backed key rather than exporting it, so the private key stays in the
                // token while the handshake still signs through it.
                X509Certificate2 certificateWithKey = publicCertificate.CopyWithPrivateKey(tokenBackedKey);

                return new SoftHsmRsaCredential(library, session, certificateWithKey);
            }
            catch
            {
                session?.Dispose();
                library.Dispose();
                throw;
            }
        }

        public void Dispose()
        {
            Certificate.Dispose();
            _session.Dispose();
            _library.Dispose();
        }

        private static string? ReadPin(string? pinSourcePath)
        {
            if (string.IsNullOrEmpty(pinSourcePath) || !File.Exists(pinSourcePath))
            {
                return null;
            }

            return File.ReadAllText(pinSourcePath).Trim();
        }

        /// <summary>
        /// A minimal parser for the subset of RFC 7512 <c>pkcs11:</c> URIs emitted by the setup script.
        /// </summary>
        private sealed class Pkcs11KeyUri
        {
            public string? Token { get; private init; }

            public string? Object { get; private init; }

            public string? PinSourcePath { get; private init; }

            public static Pkcs11KeyUri Parse(string? uri)
            {
                if (string.IsNullOrEmpty(uri) || !uri.StartsWith("pkcs11:", StringComparison.Ordinal))
                {
                    return new Pkcs11KeyUri();
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

                return new Pkcs11KeyUri
                {
                    Token = pathAttributes.GetValueOrDefault("token"),
                    Object = pathAttributes.GetValueOrDefault("object"),
                    PinSourcePath = pinSource,
                };
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
        }

        /// <summary>
        /// An <see cref="RSA"/> that forwards every signing operation to a PKCS#11 token. The private key never enters
        /// managed memory; only the public key and signature bytes cross the boundary.
        /// </summary>
        private sealed class Pkcs11Rsa : RSA
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

            private readonly ISession _session;
            private readonly IObjectHandle _privateKeyHandle;
            private readonly Pkcs11InteropFactories _factories;
            private readonly RSAParameters _publicParameters;

            public Pkcs11Rsa(
                ISession session,
                IObjectHandle privateKeyHandle,
                Pkcs11InteropFactories factories,
                RSAParameters publicParameters)
            {
                _session = session;
                _privateKeyHandle = privateKeyHandle;
                _factories = factories;
                _publicParameters = publicParameters;
                KeySizeValue = publicParameters.Modulus!.Length * 8;
            }

            public override RSAParameters ExportParameters(bool includePrivateParameters)
            {
                if (includePrivateParameters)
                {
                    throw new CryptographicException("The private key is held in the PKCS#11 token and cannot be exported.");
                }

                return new RSAParameters
                {
                    Modulus = (byte[])_publicParameters.Modulus!.Clone(),
                    Exponent = (byte[])_publicParameters.Exponent!.Clone(),
                };
            }

            public override void ImportParameters(RSAParameters parameters)
            {
                throw new NotSupportedException("A token-backed key cannot be imported.");
            }

            public override byte[] SignHash(byte[] hash, HashAlgorithmName hashAlgorithm, RSASignaturePadding padding)
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

            public override bool TrySignHash(
                ReadOnlySpan<byte> hash,
                Span<byte> destination,
                HashAlgorithmName hashAlgorithm,
                RSASignaturePadding padding,
                out int bytesWritten)
            {
                byte[] signature = SignHash(hash.ToArray(), hashAlgorithm, padding);
                if (signature.Length > destination.Length)
                {
                    bytesWritten = 0;
                    return false;
                }

                signature.CopyTo(destination);
                bytesWritten = signature.Length;
                return true;
            }

            // Hashing is not secret, so it is performed locally. Overriding these lets the base RSA.SignData compute
            // the digest before handing it to SignHash (and therefore to the token).
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
}
