// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Authentication;

namespace Microsoft.Azure.Iot.Device.Provisioning.Models
{
    /// <summary>
    /// A certificate signing request sent to Device Provisioning Service while provisioning, asking it to issue a client
    /// certificate for this device to present to IoT Hub.
    /// </summary>
    /// <remarks>
    /// Supplied to <see cref="AbstractConnectionClient.ProvisionAndConnectAsync(Device.Models.ProvisioningSettings, AuthenticationProvider, ProvisioningCertificateSigningRequest, bool, CancellationToken)"/>
    /// together with an <see cref="AuthenticationProvider"/> whose <see cref="AuthenticationProvider.HasCertificateSigningHandler"/> is true, i.e. which holds the private key or
    /// callback that turns the issued client certificate chain into the certificate presented to IoT Hub.
    /// </remarks>
    public class ProvisioningCertificateSigningRequest
    {
        /// <summary>
        /// Creates a certificate signing request to send to Device Provisioning Service.
        /// </summary>
        /// <param name="base64CertificateSigningRequest">
        /// The Base64-encoded, DER-formatted PKCS#10 certificate signing request. It must be generated with the private
        /// key that the authentication provider pairs issued certificates with.
        /// </param>
        public ProvisioningCertificateSigningRequest(string base64CertificateSigningRequest)
        {
            ArgumentException.ThrowIfNullOrWhiteSpace(base64CertificateSigningRequest, nameof(base64CertificateSigningRequest));

            Base64CertificateSigningRequest = base64CertificateSigningRequest;
        }

        /// <summary>
        /// The Base64-encoded, DER-formatted PKCS#10 certificate signing request.
        /// </summary>
        public string Base64CertificateSigningRequest { get; }
    }
}
