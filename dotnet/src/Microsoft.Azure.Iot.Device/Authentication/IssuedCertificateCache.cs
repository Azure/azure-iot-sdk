// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;

namespace Microsoft.Azure.Iot.Device.Authentication
{
    /// <summary>
    /// Pairs an issued client certificate chain with the private key its certificate signing request was generated
    /// with, reusing the result until either the chain or the key changes.
    /// </summary>
    internal sealed class IssuedCertificateCache
    {
        private readonly object _lock = new();
        private IReadOnlyList<string>? _cachedChain;
        private AsymmetricAlgorithm? _cachedPrivateKey;
        private X509Certificate2? _cachedCertificate;

        public X509Certificate2 GetOrCreate(IReadOnlyList<string> issuedClientCertificates, AsymmetricAlgorithm privateKey)
        {
            lock (_lock)
            {
                if (_cachedCertificate != null
                    && ReferenceEquals(_cachedChain, issuedClientCertificates)
                    && ReferenceEquals(_cachedPrivateKey, privateKey))
                {
                    return _cachedCertificate;
                }

                _cachedCertificate = CreateCertificateWithPrivateKey(issuedClientCertificates, privateKey);
                _cachedChain = issuedClientCertificates;
                _cachedPrivateKey = privateKey;
                return _cachedCertificate;
            }
        }

        // The first certificate in the issued chain is the device certificate; the rest are intermediates.
        private static X509Certificate2 CreateCertificateWithPrivateKey(IReadOnlyList<string> issuedClientCertificates, AsymmetricAlgorithm privateKey)
        {
            using X509Certificate2 deviceCertificate = X509CertificateLoader.LoadCertificate(Convert.FromBase64String(issuedClientCertificates[0]));

            X509Certificate2 deviceCertificateWithKey = privateKey switch
            {
                ECDsa ecdsa => deviceCertificate.CopyWithPrivateKey(ecdsa),
                RSA rsa => deviceCertificate.CopyWithPrivateKey(rsa),
                _ => throw new NotSupportedException($"Unsupported private key type: {privateKey.GetType()}"),
            };

            try
            {
                // Some TLS stacks (Windows SChannel) cannot use an ephemeral key, so export and reimport it.
                byte[] pfxBytes = deviceCertificateWithKey.Export(X509ContentType.Pfx);
                X509Certificate2 reimported = X509CertificateLoader.LoadPkcs12(pfxBytes, (string?)null, X509KeyStorageFlags.Exportable);
                deviceCertificateWithKey.Dispose();
                return reimported;
            }
            catch (CryptographicException)
            {
                // Non-exportable (for example HSM-backed) keys are presented as-is.
                return deviceCertificateWithKey;
            }
        }
    }
}
