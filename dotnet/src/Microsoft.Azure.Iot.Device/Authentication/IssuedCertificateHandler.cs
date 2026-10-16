// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;

namespace Microsoft.Azure.Iot.Device.Authentication
{
    /// <summary>
    /// Turns an issued client certificate chain into the certificate to present to IoT Hub by invoking a certificate
    /// signing complete callback once per issued chain and remembering the certificate it returns.
    /// </summary>
    internal sealed class IssuedCertificateHandler
    {
        private readonly Func<IReadOnlyList<string>, Task<X509Certificate2>> _callback;
        private readonly SemaphoreSlim _callbackLock = new(1, 1);
        private volatile HandledChain? _lastHandled;

        public IssuedCertificateHandler(Func<IReadOnlyList<string>, Task<X509Certificate2>> callback)
        {
            _callback = callback;
        }

        /// <summary>
        /// The default certificate signing complete callback: pairs the issued device certificate (the first in the
        /// chain) with the private key its certificate signing request was generated with.
        /// </summary>
        public static Func<IReadOnlyList<string>, Task<X509Certificate2>> CreateDefaultCallback(AsymmetricAlgorithm privateKey)
        {
            return issuedClientCertificates => Task.FromResult(CreateCertificateWithPrivateKey(issuedClientCertificates, privateKey));
        }

        /// <summary>
        /// Gets the certificate for this chain, invoking the callback if it has not already been handled.
        /// </summary>
        public async Task<X509Certificate2> GetCertificateAsync(IReadOnlyList<string> issuedClientCertificates)
        {
            HandledChain? lastHandled = _lastHandled;
            if (ReferenceEquals(lastHandled?.Chain, issuedClientCertificates))
            {
                return lastHandled.Certificate;
            }

            await _callbackLock.WaitAsync().ConfigureAwait(false);
            try
            {
                lastHandled = _lastHandled;
                if (ReferenceEquals(lastHandled?.Chain, issuedClientCertificates))
                {
                    return lastHandled.Certificate;
                }

                X509Certificate2 certificate = await _callback(issuedClientCertificates).ConfigureAwait(false)
                    ?? throw new InvalidOperationException("The certificate signing complete callback returned a null certificate.");

                _lastHandled = new HandledChain(issuedClientCertificates, certificate);
                return certificate;
            }
            finally
            {
                _callbackLock.Release();
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

        private sealed record HandledChain(IReadOnlyList<string> Chain, X509Certificate2 Certificate);
    }
}
