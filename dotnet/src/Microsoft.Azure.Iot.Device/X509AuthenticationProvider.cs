// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using System;
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
    }
}
