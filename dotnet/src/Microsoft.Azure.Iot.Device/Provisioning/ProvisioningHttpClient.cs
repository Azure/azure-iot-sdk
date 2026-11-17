// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Authentication;
using Microsoft.Azure.Iot.Device.Exceptions;
using Microsoft.Azure.Iot.Device.Provisioning.Models;
using System.Diagnostics;
using System.Net;
using System.Net.Http.Headers;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Text.Json.Serialization;

namespace Microsoft.Azure.Iot.Device.Provisioning
{
    /// <summary>
    /// Runs a single registration with Device Provisioning Service over its HTTPS REST API: one register request, then
    /// polling the operation it started until the service reports a terminal state.
    /// </summary>
    internal sealed class ProvisioningHttpClient : IDisposable
    {
        private const string JsonMediaType = "application/json";

        private readonly HttpClient _httpClient;
        private readonly string _registrationsUri;
        private readonly string _registrationId;
        private readonly string _apiVersion;
        private readonly string _userAgent;
        private readonly TimeSpan _requestTimeout;
        private readonly TimeSpan _minimumPollingInterval;
        private readonly Action<TimeSpan?> _onRetryAfter;
        private readonly TpmAuthenticationProvider _tpm;

        // Set once this registration has answered Device Provisioning Service's TPM challenge, after which every request
        // carries a SAS token signed by the TPM.
        private bool _tpmChallengeAnswered;
        private string? _tpmKeyName;

        /// <param name="handler">The handler that sends requests. It is disposed with this client.</param>
        /// <param name="globalEndpointAddress">The Device Provisioning Service host name.</param>
        /// <param name="idScope">The ID scope of the Device Provisioning Service instance.</param>
        /// <param name="registrationId">The registration ID of this device.</param>
        /// <param name="apiVersion">The Device Provisioning Service API version.</param>
        /// <param name="userAgent">The User-Agent to send.</param>
        /// <param name="requestTimeout">How long to wait for each HTTP request. <see cref="Timeout.InfiniteTimeSpan"/> or a non-positive value never times out.</param>
        /// <param name="minimumPollingInterval">The shortest time to wait between operation status polls.</param>
        /// <param name="onRetryAfter">Invoked with the Retry-After of every response (null when a response has none).</param>
        /// <param name="tpm">
        /// The TPM to attest with. The register request carries its endorsement and storage root keys and is first sent
        /// unauthenticated; Device Provisioning Service answers it with a key for the TPM to import, and the request is sent
        /// again with a SAS token the TPM signs with that key.
        /// </param>
        public ProvisioningHttpClient(
            HttpMessageHandler handler,
            string globalEndpointAddress,
            string idScope,
            string registrationId,
            string apiVersion,
            string userAgent,
            TimeSpan requestTimeout,
            TimeSpan minimumPollingInterval,
            Action<TimeSpan?> onRetryAfter,
            TpmAuthenticationProvider tpm)
        {
            // Each request has its own timeout, applied below, so that a timeout can be told apart from cancellation.
            _httpClient = new HttpClient(handler, disposeHandler: true) { Timeout = Timeout.InfiniteTimeSpan };
            _registrationsUri = $"https://{globalEndpointAddress}/{Uri.EscapeDataString(idScope)}/registrations/{Uri.EscapeDataString(registrationId)}";
            _registrationId = registrationId;
            _apiVersion = apiVersion;
            _userAgent = userAgent;
            _requestTimeout = requestTimeout;
            _minimumPollingInterval = minimumPollingInterval;
            _onRetryAfter = onRetryAfter;
            _tpm = tpm;
        }

        /// <summary>
        /// Register this device and wait for Device Provisioning Service to finish the registration.
        /// </summary>
        /// <returns>The terminal registration result.</returns>
        /// <exception cref="DeviceException">
        /// <see cref="ErrorRetryability.IdentityTerminal"/> if the service rejected the credentials, or
        /// <see cref="ErrorRetryability.Retryable"/> if the request failed in a way that may succeed when tried again.
        /// </exception>
        public async Task<DeviceRegistrationResult> RegisterAsync(JsonNode? payload, string? certificateSigningRequest, CancellationToken cancellationToken)
        {
            var request = new HttpRegistrationRequest
            {
                RegistrationId = _registrationId,
                Payload = payload,
                ClientCertificateSigningRequest = certificateSigningRequest,
                Tpm = new HttpTpmAttestation
                {
                    EndorsementKey = Convert.ToBase64String(await _tpm.GetEndorsementKeyAsync(cancellationToken).ConfigureAwait(false)),
                    StorageRootKey = Convert.ToBase64String(await _tpm.GetStorageRootKeyAsync(cancellationToken).ConfigureAwait(false)),
                },
            };

            string body = JsonSerializer.Serialize(request, JsonSerializationSettings.Options);
            RegistrationOperationStatus operation = await SendAsync(HttpMethod.Put, $"{_registrationsUri}/register", body, cancellationToken).ConfigureAwait(false);

            while (true)
            {
                if (operation.Status != ProvisioningRegistrationStatus.Assigning)
                {
                    // The service may finish the registration without handing back an operation to poll.
                    DeviceRegistrationResult result = operation.RegistrationState
                        ?? throw new Exception($"Device Provisioning Service reported registration status '{operation.Status}' without a registration state.");

                    if (operation.Status == ProvisioningRegistrationStatus.Assigned)
                    {
                        await ImportIotHubKeyAsync(result, cancellationToken).ConfigureAwait(false);
                    }

                    return result;
                }

                if (string.IsNullOrEmpty(operation.OperationId))
                {
                    throw new Exception("Device Provisioning Service returned a registration operation with no operation Id.");
                }

                // Never poll faster than the default cadence. Without a Retry-After from the service, add jitter so that
                // many devices that registered at the same time do not poll in lockstep.
                TimeSpan pollingDelay = operation.RetryAfter is { } serviceRetryAfter
                    ? (serviceRetryAfter < _minimumPollingInterval ? _minimumPollingInterval : serviceRetryAfter)
                    : RetryJitter.GenerateDelayWithJitterForRetry(_minimumPollingInterval);

                await Task.Delay(pollingDelay, cancellationToken).ConfigureAwait(false);

                operation = await SendAsync(
                    HttpMethod.Get,
                    $"{_registrationsUri}/operations/{Uri.EscapeDataString(operation.OperationId)}",
                    null,
                    cancellationToken).ConfigureAwait(false);
            }
        }

        private async Task<RegistrationOperationStatus> SendAsync(HttpMethod method, string uri, string? body, CancellationToken cancellationToken)
        {
            while (true)
            {
                RegistrationOperationStatus? operation = await SendOnceAsync(method, uri, body, cancellationToken).ConfigureAwait(false);
                if (operation != null)
                {
                    return operation;
                }

                // The TPM challenge was answered, so send the request again, this time authenticated.
            }
        }

        /// <returns>The operation status, or null if the response was a TPM challenge that has now been answered.</returns>
        private async Task<RegistrationOperationStatus?> SendOnceAsync(HttpMethod method, string uri, string? body, CancellationToken cancellationToken)
        {
            using var request = new HttpRequestMessage(method, $"{uri}?api-version={Uri.EscapeDataString(_apiVersion)}");
            request.Headers.Accept.Add(new MediaTypeWithQualityHeaderValue(JsonMediaType));
            request.Headers.TryAddWithoutValidation("User-Agent", _userAgent);

            // Until the TPM challenge is answered there is no key to sign with, so the request goes unauthenticated.
            // Sign a fresh token for each request so that one is never presented after it expires.
            if (_tpmChallengeAnswered)
            {
                string sasToken = await _tpm.BuildProvisioningSasTokenAsync(_tpmKeyName, cancellationToken).ConfigureAwait(false);
                request.Headers.TryAddWithoutValidation("Authorization", sasToken);
            }

            if (body != null)
            {
                request.Content = new StringContent(body, Encoding.UTF8, JsonMediaType);
            }

            using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
            if (_requestTimeout > TimeSpan.Zero)
            {
                timeout.CancelAfter(_requestTimeout);
            }

            HttpResponseMessage response;
            string responseBody;
            try
            {
                Trace.TraceInformation("Sending {0} to Device Provisioning Service at {1}", method, uri);
                response = await _httpClient.SendAsync(request, timeout.Token).ConfigureAwait(false);
                responseBody = await response.Content.ReadAsStringAsync(timeout.Token).ConfigureAwait(false);
            }
            catch (OperationCanceledException e) when (!cancellationToken.IsCancellationRequested)
            {
                throw new DeviceException($"Timed out after {_requestTimeout} waiting for Device Provisioning Service to respond to {method} {uri}.", e)
                {
                    Retryability = ErrorRetryability.Retryable,
                    IsContained = false,
                };
            }
            catch (HttpRequestException e)
            {
                throw new DeviceException($"Failed to send {method} {uri} to Device Provisioning Service: {e.Message}", e)
                {
                    Retryability = ErrorRetryability.Retryable,
                    IsContained = false,
                };
            }

            using (response)
            {
                // Remember what the service asked for on every response -- including an error -- as a floor on the
                // re-provisioning backoff, the same as the MQTT transport does.
                TimeSpan? retryAfter = GetRetryAfter(response.Headers.RetryAfter);
                _onRetryAfter(retryAfter);

                if (!response.IsSuccessStatusCode)
                {
                    string message = $"Device Provisioning Service rejected {method} {uri} with status code {(int)response.StatusCode} ({response.StatusCode}). Response body: '{responseBody}'.";

                    if (response.StatusCode == HttpStatusCode.Unauthorized)
                    {
                        if (!_tpmChallengeAnswered && await TryAnswerTpmChallengeAsync(responseBody, cancellationToken).ConfigureAwait(false))
                        {
                            return null;
                        }

                        throw new DeviceException(message)
                        {
                            Retryability = ErrorRetryability.IdentityTerminal,
                            IsContained = false,
                        };
                    }

                    if (IsTransient(response.StatusCode))
                    {
                        throw new DeviceException(message)
                        {
                            Retryability = ErrorRetryability.Retryable,
                            IsContained = false,
                        };
                    }

                    throw new Exception(message);
                }

                RegistrationOperationStatus? operation;
                try
                {
                    operation = JsonSerializer.Deserialize<RegistrationOperationStatus>(responseBody, JsonSerializationSettings.Options);
                }
                catch (JsonException e)
                {
                    throw new Exception($"Device Provisioning Service sent a response that is not a registration operation. Response body: '{responseBody}'.", e);
                }

                if (operation == null)
                {
                    throw new Exception($"Device Provisioning Service sent an empty response to {method} {uri}.");
                }

                operation.RetryAfter = retryAfter ?? operation.RetryAfter;
                return operation;
            }
        }

        /// <summary>
        /// Imports the key Device Provisioning Service sent with its 401 response to an unauthenticated TPM register
        /// request, so that the request can be sent again authenticated with it.
        /// </summary>
        /// <returns>False if the response was not a TPM challenge, so the service refused the registration.</returns>
        private async Task<bool> TryAnswerTpmChallengeAsync(string responseBody, CancellationToken cancellationToken)
        {
            HttpTpmChallenge? challenge;
            try
            {
                challenge = JsonSerializer.Deserialize<HttpTpmChallenge>(responseBody, JsonSerializationSettings.Options);
            }
            catch (JsonException)
            {
                return false;
            }

            if (string.IsNullOrEmpty(challenge?.AuthenticationKey))
            {
                return false;
            }

            Trace.TraceInformation("Device Provisioning Service challenged the TPM; importing its key and registering again");
            await _tpm.ImportKeyAsync(DecodeTpmKey(challenge.AuthenticationKey), cancellationToken).ConfigureAwait(false);
            _tpmKeyName = challenge.KeyName;
            _tpmChallengeAnswered = true;
            return true;
        }

        /// <summary>
        /// Imports the key the device authenticates with IoT Hub with, which an assigned TPM registration carries.
        /// </summary>
        private async Task ImportIotHubKeyAsync(DeviceRegistrationResult result, CancellationToken cancellationToken)
        {
            string? authenticationKey = result.Tpm?.AuthenticationKey;
            if (string.IsNullOrEmpty(authenticationKey))
            {
                if (_tpm.PresentsIssuedCertificates && result.IssuedClientCertificateChain is { Count: > 0 })
                {
                    // The device presents its issued certificate to IoT Hub, so it never signs with an IoT Hub key.
                    return;
                }

                throw new Exception("Device Provisioning Service assigned this TPM device without the key it authenticates with IoT Hub with.");
            }

            await _tpm.ImportKeyAsync(DecodeTpmKey(authenticationKey), cancellationToken).ConfigureAwait(false);
        }

        private static byte[] DecodeTpmKey(string authenticationKey)
        {
            try
            {
                return Convert.FromBase64String(authenticationKey);
            }
            catch (FormatException e)
            {
                throw new Exception("Device Provisioning Service sent a TPM authentication key that is not valid Base64.", e);
            }
        }

        private static bool IsTransient(HttpStatusCode statusCode)
        {
            return statusCode == HttpStatusCode.RequestTimeout
                || statusCode == HttpStatusCode.TooManyRequests
                || (int)statusCode >= 500;
        }

        private static TimeSpan? GetRetryAfter(RetryConditionHeaderValue? retryAfter)
        {
            if (retryAfter?.Delta is { } delta)
            {
                return delta;
            }

            if (retryAfter?.Date is { } date)
            {
                TimeSpan untilDate = date - DateTimeOffset.UtcNow;
                return untilDate > TimeSpan.Zero ? untilDate : TimeSpan.Zero;
            }

            return null;
        }

        public void Dispose()
        {
            _httpClient.Dispose();
        }

        /// <summary>
        /// The body of an HTTP register request. Unlike the MQTT transport, which carries the registration ID in the
        /// CONNECT packet, the REST API requires it in the body.
        /// </summary>
        private sealed class HttpRegistrationRequest
        {
            [JsonPropertyName("registrationId")]
            public string RegistrationId { get; set; } = string.Empty;

            [JsonPropertyName("payload")]
            public JsonNode? Payload { get; set; }

            [JsonPropertyName("csr")]
            public string? ClientCertificateSigningRequest { get; set; }

            [JsonPropertyName("tpm")]
            public HttpTpmAttestation? Tpm { get; set; }
        }

        private sealed class HttpTpmAttestation
        {
            [JsonPropertyName("endorsementKey")]
            public string EndorsementKey { get; set; } = string.Empty;

            [JsonPropertyName("storageRootKey")]
            public string StorageRootKey { get; set; } = string.Empty;
        }

        /// <summary>
        /// The body of the 401 response Device Provisioning Service challenges an unauthenticated TPM register request with.
        /// </summary>
        private sealed class HttpTpmChallenge
        {
            [JsonPropertyName("authenticationKey")]
            public string? AuthenticationKey { get; set; }

            [JsonPropertyName("keyName")]
            public string? KeyName { get; set; }
        }
    }
}
