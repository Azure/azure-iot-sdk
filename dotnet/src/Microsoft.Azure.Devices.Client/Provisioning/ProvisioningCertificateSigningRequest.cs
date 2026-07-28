using System;
using System.Collections.Generic;
using System.Security.Cryptography;
using System.Text;

namespace Microsoft.Azure.Devices.Client.Provisioning
{
    public class ProvisioningCertificateSigningRequest
    {
        public AsymmetricAlgorithm PrivateKey { get; set; }

        public string Base64CertificateSigningRequest { get; set; }

        public ProvisioningCertificateSigningRequest(AsymmetricAlgorithm privateKey, string base64CertificateSigningRequest) 
        {
            ArgumentNullException.ThrowIfNull(privateKey, nameof(privateKey));
            ArgumentException.ThrowIfNullOrWhiteSpace(base64CertificateSigningRequest, nameof(base64CertificateSigningRequest));

            PrivateKey = privateKey;
            Base64CertificateSigningRequest = base64CertificateSigningRequest;
        }
    }
}
