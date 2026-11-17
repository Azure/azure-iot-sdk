// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Authentication;
using Microsoft.Azure.Iot.Device.Models;

namespace Microsoft.Azure.Iot.Device.Provisioning
{
    /// <summary>
    /// The inputs and cancellation of one provisioning run, whichever transport carries it.
    /// </summary>
    internal sealed class ProvisioningRun : IDisposable
    {
        private readonly CancellationTokenSource _cancellation;

        public ProvisioningRun(ProvisioningSettings settings, AuthenticationProvider authentication, CancellationToken cancellationToken)
        {
            Settings = settings;
            Authentication = authentication;
            RequestPayload = new RegistrationRequestPayload()
            {
                ClientCertificateSigningRequest = settings.CertificateSigningRequest?.Base64CertificateSigningRequest,
                Payload = settings.ProvisioningPayload,
            };

            _cancellation = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
            Token = _cancellation.Token;
        }

        public ProvisioningSettings Settings { get; }

        /// <summary>
        /// The authentication to present to Device Provisioning Service.
        /// </summary>
        public AuthenticationProvider Authentication { get; }

        /// <summary>
        /// The registration request to send.
        /// </summary>
        public RegistrationRequestPayload RequestPayload { get; }

        /// <summary>
        /// Cancelled when the caller cancels or when this run is cancelled through <see cref="Cancel"/>.
        /// </summary>
        public CancellationToken Token { get; }

        /// <summary>
        /// Whether this run was cancelled, either by the caller or through <see cref="Cancel"/>.
        /// </summary>
        public bool IsCancellationRequested => Token.IsCancellationRequested;

        /// <summary>
        /// Cancel this run, for instance because the connection client was deliberately disconnected or disposed.
        /// </summary>
        public void Cancel()
        {
            try
            {
                _cancellation.Cancel();
            }
            catch (ObjectDisposedException)
            {
                // The run already ended, so there is nothing left to cancel.
            }
        }

        public void Dispose()
        {
            _cancellation.Dispose();
        }
    }
}
