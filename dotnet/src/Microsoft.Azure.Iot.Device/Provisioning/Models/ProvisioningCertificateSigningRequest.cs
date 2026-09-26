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
        /// <summary>
        /// The private key corresponding to the public key in <see cref="Base64CertificateSigningRequest"/>, used to
        /// prove possession of the key by signing the certificate signing request and, after issuance, to authenticate
        /// the resulting certificate over TLS.
        /// <para>
        /// This is the key object itself, not its exported bytes. Providing an <see cref="AsymmetricAlgorithm"/> whose
        /// signing operation is delegated to a hardware security module (HSM) — for example a PKCS#11 or Key Vault
        /// backed <see cref="RSA"/> or <see cref="System.Security.Cryptography.ECDsa"/> — keeps the private key inside
        /// the HSM; the SDK only invokes it to sign and never exports the key material.
        /// </para>
        /// </summary>
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