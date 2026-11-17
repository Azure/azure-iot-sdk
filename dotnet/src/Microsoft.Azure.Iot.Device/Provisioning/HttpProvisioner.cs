// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Authentication;
using Microsoft.Azure.Iot.Device.Exceptions;
using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Provisioning.Models;
using Microsoft.Azure.Iot.Device.Retry;
using System.Diagnostics;

namespace Microsoft.Azure.Iot.Device.Provisioning
{
    /// <summary>
    /// Registers a device with Device Provisioning Service over its HTTPS REST API, retrying under the connection retry
    /// policy. Device Provisioning Service only supports TPM attestation over HTTPS.
    /// </summary>
    /// <remarks>
    /// With no MQTT connection to Device Provisioning Service, the connection layer cannot retry this, so this class
    /// does: a request that fails transiently is retried under the retry policy, floored by any Retry-After the service
    /// sent. A TPM has no secondary credentials, so a rejected registration is not retried with other credentials.
    /// </remarks>
    internal sealed class HttpProvisioner
    {
        private readonly Func<AuthenticationCredentials, HttpMessageHandler> _createHandler;
        private readonly IRetryPolicy _retryPolicy;
        private readonly TimeSpan _requestTimeout;
        private readonly ProvisioningServiceRetryAfter _serviceRetryAfter;

        /// <param name="createHandler">Creates the handler that sends requests with the given boot credentials.</param>
        /// <param name="retryPolicy">The policy that decides whether, and when, to retry a transient failure.</param>
        /// <param name="requestTimeout">How long to wait for each HTTPS request.</param>
        /// <param name="serviceRetryAfter">Tracks the Retry-After the service asks for.</param>
        public HttpProvisioner(
            Func<AuthenticationCredentials, HttpMessageHandler> createHandler,
            IRetryPolicy retryPolicy,
            TimeSpan requestTimeout,
            ProvisioningServiceRetryAfter serviceRetryAfter)
        {
            _createHandler = createHandler;
            _retryPolicy = retryPolicy;
            _requestTimeout = requestTimeout;
            _serviceRetryAfter = serviceRetryAfter;
        }

        public async Task<DeviceRegistrationResult> RegisterAsync(
            ProvisioningRun run,
            TpmAuthenticationProvider authentication,
            CancellationToken cancellationToken)
        {
            string registrationId = authentication.GetRegistrationId();
            uint attempt = 0;

            while (true)
            {
                try
                {
                    AuthenticationCredentials credentials = await authentication.GetPrimaryBootAuthenticationCredentialsAsync(cancellationToken).ConfigureAwait(false);
                    using var provisioningHttpClient = new ProvisioningHttpClient(
                        _createHandler(credentials),
                        run.Settings.GlobalEndpointAddress,
                        run.Settings.IdScope,
                        registrationId,
                        ProvisioningDefaults.ApiVersion,
                        ProvisioningDefaults.UserAgent,
                        _requestTimeout,
                        ProvisioningDefaults.OperationPollingInterval,
                        _serviceRetryAfter.Capture,
                        authentication);

                    return await provisioningHttpClient
                        .RegisterAsync(run.RequestPayload.Payload, run.RequestPayload.ClientCertificateSigningRequest, cancellationToken)
                        .ConfigureAwait(false);
                }
                catch (DeviceException e) when (e.Retryability == ErrorRetryability.Retryable && !cancellationToken.IsCancellationRequested)
                {
                    if (attempt < uint.MaxValue)
                    {
                        attempt++;
                    }

                    RetryGuidance guidance = _retryPolicy.GetRetryGuidance(attempt, e, ConnectionEndpoint.DeviceProvisioningService, out TimeSpan retryDelay);
                    if (guidance == RetryGuidance.AbandonRetry)
                    {
                        throw;
                    }

                    retryDelay = _serviceRetryAfter.Floor(retryDelay);
                    Trace.TraceWarning("Registering with Device Provisioning Service over HTTPS failed on attempt {0}; retrying in {1}. {2}", attempt, retryDelay, e);

                    if (retryDelay > TimeSpan.Zero)
                    {
                        await Task.Delay(retryDelay, cancellationToken).ConfigureAwait(false);
                    }
                }
            }
        }
    }
}
