// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using System.Net.Security;
using System.Security.Cryptography.X509Certificates;

namespace Microsoft.Azure.Iot.Device.Authentication
{
    /// <summary>
    /// Device Provisioning Service boot authentication using an X509 certificate.
    /// Issued operational certificates never replace this provider's boot certificate.
    /// </summary>
    public class X509BootAuthenticationProvider : BootAuthenticationProvider
    {
        /// <summary>
        /// Creates a boot provider using the supplied certificate and its private key.
        /// The key need not be exportable; HSM-backed certificates are supported and the SDK never exports the key.
        /// </summary>
        /// <param name="clientCertificate">The boot client certificate used with DPS.</param>
        /// <param name="certificateChain">The chain leading to the root certificate uploaded to DPS.</param>
        /// <param name="remoteCertificateValidationCallback">Optional custom validation of the remote TLS certificate.</param>
        /// <param name="localCertificateSelectionCallback">Optional selection of the boot client TLS certificate.</param>
        public X509BootAuthenticationProvider(
            X509Certificate2 clientCertificate,
            X509Certificate2Collection? certificateChain = null,
            RemoteCertificateValidationCallback? remoteCertificateValidationCallback = null,
            LocalCertificateSelectionCallback? localCertificateSelectionCallback = null)
            : base(remoteCertificateValidationCallback, localCertificateSelectionCallback)
        {
            ClientCertificate = clientCertificate ?? throw new ArgumentNullException(nameof(clientCertificate));
            CertificateChain = certificateChain;
        }

        /// <summary>
        /// The boot client certificate used for TLS authentication with DPS.
        /// </summary>
        public X509Certificate2 ClientCertificate { get; }

        /// <summary>
        /// The boot certificate chain leading to the root certificate installed in DPS.
        /// </summary>
        public X509Certificate2Collection? CertificateChain { get; }

        public override AuthenticationCredentials GetDeviceProvisioningServiceAuthentication()
        {
            return new AuthenticationCredentials()
            {
                ClientCertificate = ClientCertificate,
                RemoteCertificateValidationCallback = RemoteCertificateValidationCallback,
                LocalCertificateSelectionCallback = LocalCertificateSelectionCallback,
            };
        }

        public override string GetRegistrationId()
        {
            return ClientCertificate.GetNameInfo(X509NameType.DnsName, false);
        }
    }
}
