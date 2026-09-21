// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using System;
using System.Collections.Generic;
using System.Security.Cryptography;
using System.Text;

namespace Microsoft.Azure.Iot.Device.Provisioning.Models
{
    public class ProvisioningCertificateSigningRequest
    {
        public AsymmetricAlgorithm PrivateKey { get; set; } //TODO feels weird to ask for this, but it is necessary, right?

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