// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using System.Net.Security;
using System.Text;

namespace Microsoft.Azure.Iot.Device.Authentication.Boot
{
    /// <summary>
    /// Device Provisioning Service boot authentication using a device-scoped symmetric key.
    /// </summary>
    public class SymmetricKeyBootAuthenticationProvider : BootAuthenticationProvider
    {
        private readonly string _registrationId;
        private readonly string _idScope;
        private readonly string _symmetricKey;

        /// <summary>
        /// Creates a boot provider using a DPS registration Id, Id scope and Base64-encoded device symmetric key.
        /// </summary>
        public SymmetricKeyBootAuthenticationProvider(
            string registrationId,
            string idScope,
            string symmetricKey,
            RemoteCertificateValidationCallback? remoteCertificateValidationCallback = null,
            LocalCertificateSelectionCallback? localCertificateSelectionCallback = null)
            : base(remoteCertificateValidationCallback, localCertificateSelectionCallback)
        {
            ArgumentException.ThrowIfNullOrEmpty(registrationId);
            ArgumentException.ThrowIfNullOrEmpty(idScope);
            ArgumentException.ThrowIfNullOrEmpty(symmetricKey);
            _registrationId = registrationId;
            _idScope = idScope;
            _symmetricKey = symmetricKey;
        }

        public override AuthenticationCredentials GetCredentials()
        {
            string token = SasTokenBuilder.BuildSasToken(
                "registration", _symmetricKey, $"{_idScope}/registrations/{_registrationId}", TimeSpan.FromHours(1));
            return new AuthenticationCredentials()
            {
                Password = Encoding.UTF8.GetBytes(token),
                RemoteCertificateValidationCallback = RemoteCertificateValidationCallback,
                LocalCertificateSelectionCallback = LocalCertificateSelectionCallback,
            };
        }

        public override string GetRegistrationId() => _registrationId;
    }
}
