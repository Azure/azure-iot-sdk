// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using System.Net.Security;
using System.Text;

namespace Microsoft.Azure.Iot.Device.Authentication.Operational
{
    /// <summary>
    /// IoT Hub operational authentication using a device-scoped symmetric key.
    /// </summary>
    public class SymmetricKeyOperationalAuthenticationProvider : OperationalAuthenticationProvider
    {
        private readonly string _symmetricKey;

        /// <summary>
        /// Creates an operational provider using the Base64-encoded IoT Hub device key.
        /// The assigned hostname and device Id are supplied on each connection attempt.
        /// </summary>
        public SymmetricKeyOperationalAuthenticationProvider(
            string symmetricKey,
            RemoteCertificateValidationCallback? remoteCertificateValidationCallback = null,
            LocalCertificateSelectionCallback? localCertificateSelectionCallback = null)
            : base(remoteCertificateValidationCallback, localCertificateSelectionCallback)
        {
            ArgumentException.ThrowIfNullOrEmpty(symmetricKey);
            _symmetricKey = symmetricKey;
        }

        public override Task<AuthenticationCredentials> GetCredentialsAsync(
            string hostName,
            string deviceId,
            IReadOnlyList<string>? issuedClientCertificates,
            CancellationToken cancellationToken = default)
        {
            cancellationToken.ThrowIfCancellationRequested();
            //TODO should actually use the issued client certificates to do x509 auth if configured. Maybe a constructor-time flag for this
            string token = SasTokenBuilder.BuildSasToken(
                string.Empty, _symmetricKey, $"{hostName}/devices/{deviceId}", TimeSpan.FromHours(1));
            return Task.FromResult(new AuthenticationCredentials()
            {
                Password = Encoding.UTF8.GetBytes(token),
                RemoteCertificateValidationCallback = RemoteCertificateValidationCallback,
                LocalCertificateSelectionCallback = LocalCertificateSelectionCallback,
            });
        }
    }
}
