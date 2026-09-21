// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using System;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;

namespace Microsoft.Azure.Iot.Device
{
    /// <summary>
    /// The device authentication for using an X509 certificate object.
    /// </summary>
    public class X509AuthenticationProvider
    {
        /// <summary>
        /// Creates an instance of this class.
        /// </summary>
        /// <param name="clientCertificate">
        /// The client certificate used for authentication. The private key should be available in the <see cref="X509Certificate2"/> object,
        /// or should be available in the certificate store of the system where the client will be authenticated from.
        /// </param>
        /// <param name="certificateChain">
        /// The certificate chain leading to the root certificate uploaded to the device provisioning service.
        /// </param>
        public X509AuthenticationProvider(
            X509Certificate2 clientCertificate,
            X509Certificate2Collection? certificateChain = null)
        {
            ClientCertificate = clientCertificate
                ?? throw new ArgumentException("No certificate was found. To use certificate authentication certificate must be present.", nameof(clientCertificate));

            CertificateChain = certificateChain;
        }

        /// <summary>
        /// Creates an instance of this class from a certificate whose private key is held separately, for
        /// example by a PKCS#11 module, cloud HSM SDK, or other key store that keeps the key out of the process
        /// and out of any PFX/PEM file written alongside the certificate.
        /// </summary>
        /// <param name="clientCertificate">
        /// The client certificate used for authentication. This certificate must NOT itself carry a private key;
        /// the private key operations are instead delegated to <paramref name="hsmPrivateKey"/>.
        /// </param>
        /// <param name="hsmPrivateKey">
        /// An <see cref="RSA"/> instance backed by the HSM (or other separately held key store) that performs
        /// signing on behalf of <paramref name="clientCertificate"/> during the TLS handshake.
        /// <para>
        /// Note: binding this key to <paramref name="clientCertificate"/> uses
        /// <see cref="RSACertificateExtensions.CopyWithPrivateKey(X509Certificate2, RSA)"/>, which calls
        /// <c>ExportParameters(true)</c> on <paramref name="hsmPrivateKey"/> as part of composing the certificate.
        /// A key whose <c>ExportParameters(true)</c> call itself refuses to complete -- for example a Windows CNG
        /// key created with <c>CngExportPolicies.None</c> -- cannot be used here; there is currently no supported
        /// way in .NET to compose a certificate with such a key. For a genuinely hardware-enforced,
        /// non-exportable key, load the certificate directly from the platform certificate store instead, where
        /// the OS has already associated the key with it, and use the certificate-only constructor above.
        /// </para>
        /// </param>
        /// <param name="certificateChain">
        /// The certificate chain leading to the root certificate uploaded to the device provisioning service.
        /// </param>
        public X509AuthenticationProvider(
            X509Certificate2 clientCertificate,
            RSA hsmPrivateKey,
            X509Certificate2Collection? certificateChain = null)
        {
            ClientCertificate = ComposeWithHsmKey(clientCertificate, hsmPrivateKey, cert => cert.CopyWithPrivateKey(hsmPrivateKey));
            CertificateChain = certificateChain;
        }

        /// <summary>
        /// Creates an instance of this class from a certificate whose private key is held separately, for
        /// example by a PKCS#11 module, cloud HSM SDK, or other key store that keeps the key out of the process
        /// and out of any PFX/PEM file written alongside the certificate.
        /// </summary>
        /// <param name="clientCertificate">
        /// The client certificate used for authentication. This certificate must NOT itself carry a private key;
        /// the private key operations are instead delegated to <paramref name="hsmPrivateKey"/>.
        /// </param>
        /// <param name="hsmPrivateKey">
        /// An <see cref="ECDsa"/> instance backed by the HSM (or other separately held key store) that performs
        /// signing on behalf of <paramref name="clientCertificate"/> during the TLS handshake.
        /// <para>
        /// Note: binding this key to <paramref name="clientCertificate"/> uses
        /// <see cref="ECDsaCertificateExtensions.CopyWithPrivateKey(X509Certificate2, ECDsa)"/>, which calls
        /// <c>ExportParameters(true)</c> on <paramref name="hsmPrivateKey"/> as part of composing the certificate.
        /// A key whose <c>ExportParameters(true)</c> call itself refuses to complete -- for example a Windows CNG
        /// key created with <c>CngExportPolicies.None</c> -- cannot be used here; there is currently no supported
        /// way in .NET to compose a certificate with such a key. For a genuinely hardware-enforced,
        /// non-exportable key, load the certificate directly from the platform certificate store instead, where
        /// the OS has already associated the key with it, and use the certificate-only constructor above.
        /// </para>
        /// </param>
        /// <param name="certificateChain">
        /// The certificate chain leading to the root certificate uploaded to the device provisioning service.
        /// </param>
        public X509AuthenticationProvider(
            X509Certificate2 clientCertificate,
            ECDsa hsmPrivateKey,
            X509Certificate2Collection? certificateChain = null)
        {
            ClientCertificate = ComposeWithHsmKey(clientCertificate, hsmPrivateKey, cert => cert.CopyWithPrivateKey(hsmPrivateKey));
            CertificateChain = certificateChain;
        }

        /// <summary>
        /// The client certificate used for TLS device authentication.
        /// </summary>
        public X509Certificate2 ClientCertificate { get; }

        /// <summary>
        /// The certificate trust chain that will end in the Trusted Root installed on the server side.
        /// </summary>
        public X509Certificate2Collection? CertificateChain { get; }

        internal string GetRegistrationId()
        {
            return ClientCertificate.GetNameInfo(X509NameType.DnsName, false);
        }

        // Validates the inputs for the HSM-backed constructors and binds the HSM key to the certificate.
        // Kept generic over the key type so the RSA and ECDsa overloads share one implementation and one
        // set of error messages.
        private static X509Certificate2 ComposeWithHsmKey<TKey>(
            X509Certificate2 clientCertificate,
            TKey hsmPrivateKey,
            Func<X509Certificate2, X509Certificate2> copyWithPrivateKey)
            where TKey : AsymmetricAlgorithm
        {
            if (clientCertificate == null)
            {
                throw new ArgumentException("No certificate was found. To use certificate authentication certificate must be present.", nameof(clientCertificate));
            }

            if (hsmPrivateKey == null)
            {
                throw new ArgumentException("No HSM-backed private key was provided. To use HSM-backed certificate authentication, a private key handle must be present.", nameof(hsmPrivateKey));
            }

            if (clientCertificate.HasPrivateKey)
            {
                // Ambiguous and a signal of a mistaken caller: either the certificate is meant to carry its own
                // private key (use the other constructor) or the private key lives in the HSM, but not both.
                throw new ArgumentException(
                    "The certificate already carries a private key. When authenticating with an HSM-backed key, supply a public-only certificate.",
                    nameof(clientCertificate));
            }

            return copyWithPrivateKey(clientCertificate);
        }
    }
}
