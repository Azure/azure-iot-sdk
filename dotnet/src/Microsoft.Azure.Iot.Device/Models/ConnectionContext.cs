// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Authentication;

namespace Microsoft.Azure.Iot.Device.Models
{
    /// <summary>
    /// The context used by a provisioned device to connect to IoT Hub.
    /// </summary>
    public class ConnectionContext
    {
        /// <summary>
        /// The device Id to connect to IoT Hub as.
        /// </summary>
        public required string DeviceId { get; init; }

        /// <summary>
        /// The hostname of the IoT Hub to connect to.
        /// </summary>
        public required string IotHubHostName { get; init; }

        /// <summary>
        /// The type of IoT Hub that will be connected to.
        /// </summary>
        public required IotHubConnectionType ConnectionProfile { get; init; }

        /// <summary>
        /// The most recently issued client certificate chain (Base64-encoded, device certificate first), whether signed
        /// during provisioning or signed by IoT Hub. Passed to
        /// <see cref="AuthenticationProvider.GetPrimaryOperationalAuthenticationCredentialsAsync(string, string, IReadOnlyList{string}?)"/> on every
        /// connection attempt to IoT Hub. Null if no client certificate has been issued.
        /// </summary>
        public IReadOnlyList<string>? IssuedClientCertificates
        {
            get => _issuedClientCertificates;
            set => _issuedClientCertificates = value;
        }

        // Volatile because IoT Hub certificate signing responses update it on a connection callback thread while
        // reconnect attempts read it on another.
        private volatile IReadOnlyList<string>? _issuedClientCertificates;

        public required AuthenticationProvider AuthenticationProvider { get; set; }

        /// <summary>
        /// Whether connections to this IoT Hub authenticate with the secondary credentials of
        /// <see cref="AuthenticationProvider"/>, because IoT Hub rejected the primary ones.
        /// </summary>
        internal bool UsesSecondaryCredentials
        {
            get => _usesSecondaryCredentials;
            set => _usesSecondaryCredentials = value;
        }

        // Volatile because a connection fault switches it while file uploads may read it on another thread.
        private volatile bool _usesSecondaryCredentials;

        /// <summary>
        /// Get the credentials to present to this IoT Hub: the secondary ones when <see cref="UsesSecondaryCredentials"/>
        /// is set and the provider still has them, otherwise the primary ones.
        /// </summary>
        internal async Task<AuthenticationCredentials> GetOperationalAuthenticationCredentialsAsync(
            CancellationToken cancellationToken,
            bool updateSecondaryCredentialSelection)
        {
            AuthenticationProvider authentication = AuthenticationProvider;
            if (UsesSecondaryCredentials)
            {
                AuthenticationCredentials? secondaryCredentials = await authentication.GetSecondaryOperationalAuthenticationCredentialsAsync(
                    IotHubHostName,
                    DeviceId,
                    IssuedClientCertificates,
                    cancellationToken).ConfigureAwait(false);
                if (secondaryCredentials != null)
                {
                    return secondaryCredentials;
                }

                // The secondary credentials no longer apply, for example because a client certificate was issued since.
                if (updateSecondaryCredentialSelection)
                {
                    UsesSecondaryCredentials = false;
                }
            }

            return await authentication.GetPrimaryOperationalAuthenticationCredentialsAsync(
                IotHubHostName,
                DeviceId,
                IssuedClientCertificates,
                cancellationToken).ConfigureAwait(false);
        }

        /// <summary>
        /// Switch to the other set of credentials after IoT Hub rejected the current ones.
        /// </summary>
        /// <returns>False if the provider has no secondary credentials to switch between.</returns>
        internal async Task<bool> TrySwitchCredentialsAsync(CancellationToken cancellationToken)
        {
            if (UsesSecondaryCredentials)
            {
                UsesSecondaryCredentials = false;
                return true;
            }

            AuthenticationCredentials? secondaryCredentials = await AuthenticationProvider.GetSecondaryOperationalAuthenticationCredentialsAsync(
                IotHubHostName,
                DeviceId,
                IssuedClientCertificates,
                cancellationToken).ConfigureAwait(false);
            if (secondaryCredentials == null)
            {
                return false;
            }

            UsesSecondaryCredentials = true;
            return true;
        }
    }
}
