// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Exceptions;
using Microsoft.Azure.Iot.Device.MQTTv5.Connection;
using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Models.CertificateManagement;
using Microsoft.Azure.Iot.Device.Mqtt;
using System.Collections.Concurrent;
using System.Diagnostics;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using Microsoft.Azure.Iot.Device.Provisioning.Models;

namespace Microsoft.Azure.Iot.Device.Unified.Connection
{
    public class ConnectionClient : AbstractConnectionClient, IConnectionClient
    {
        private const string CertificateSigningRequestTopic = "$iothub/credentials/POST/issueCertificate/?$rid=";
        private const string CertificateSigningResponseTopicFilter = "$iothub/credentials/res/#";
        private const string CertificateSigningResponseTopic = "$iothub/credentials/res/";
        internal const string ClassicHubApiVersion = "2025-08-01-preview";

        private const string RequestId = "?$rid=";

        private readonly ConcurrentDictionary<string, CertificateSigningOperation> _pendingCertificateSigningOperations = new();

        /// <summary>
        /// Construct a new <see cref="ConnectionClient"/>
        /// </summary>
        /// <param name="options">
        /// The optional configurations that this client will use
        /// </param>
        /// <param name="connectionContext">
        /// An optional IoT hub assignment the application restored (for example, one persisted to disk across a device
        /// reboot). When supplied, <see cref="AbstractConnectionClient.ProvisionAndConnectAsync"/> attempts to connect
        /// directly to this IoT hub before provisioning for a new assignment.
        /// </param>
        public ConnectionClient(ConnectionClientOptions? options = null, ConnectionContext? connectionContext = null) : base(options, connectionContext)
        {
            options ??= new ConnectionClientOptions();

            ManagedMqttConnection.PublishReceivedAsync += HandleReceivedCertificateSigningPublish;
            ManagedMqttConnection.DisconnectedAsync += HandleCertificateSigningDisconnectedAsync;
        }

        private async Task HandleMQTTv5ClientConnectionReady(DevicePresenceFlowCompletedArgs args)
        {
            // When the underlying MQTTv5 connection client has re-established its presence, then it is ready to use (analogous to an MQTTv3 client that has finished re-subscribing to twin/direct methods/telemetry topics)
            await RaiseDevicePresenceFlowCompletedAsync(new DevicePresenceFlowCompletedArgs() { IsSuccess = true });
        }

        internal override MqttConnect IotHubMqttConnectOverride(MqttConnect connect)
        {
            Debug.Assert(CurrentConnectionContext != null);

            connect.WebsocketUri = $"wss://{connect.HostName}/$iothub/websocket";
            connect.ProtocolVersion = CurrentConnectionContext.ConnectionProfile == IotHubConnectionType.Mqttv5 ? MqttProtocolVersion.V500 : MqttProtocolVersion.V311;
            connect.Username = $"{connect.HostName}/{connect.ClientId}/?api-version={ClassicHubApiVersion}&DeviceClientType={Uri.EscapeDataString(GetUserAgentString())}";

            // MQTTv3 flow does not need to insert anything unique per connect attempt
            return connect;
        }

        public override async Task HandleConnectedToHubAsync(MqttClientConnectedEventArgs args)
        {
            // Note that this callback handler is only set after provisioning has completed. It is only to respond to IoT Hub connection events, not DPS connection events.

            // This callback should only be reached after provisioning, so their should always be a connection context to use
            Debug.Assert(CurrentConnectionContext != null);

            // Upon an MQTT connection being established, immediately re-subscribe to all twin/telemetry/direct methods topics if there is no session present.
            if (args.ConnectAck.IsSessionPresent)
            {
                await RaiseDevicePresenceFlowCompletedAsync(new DevicePresenceFlowCompletedArgs() { IsSuccess = true });

                return;
            }

            MqttSubscribe mqttSubscribe = new();
            var expectedQos = MqttQualityOfServiceLevel.AtMostOnce;
            mqttSubscribe.TopicFilters.Add(new(Telemetry.TelemetryClient.DeviceBoundMessagesTopicFormat + "#", expectedQos));
            mqttSubscribe.TopicFilters.Add(new(Twin.TwinClient.ClassicTwinResponseTopic + "#", expectedQos));
            mqttSubscribe.TopicFilters.Add(new(Twin.TwinClient.ClassicTwinDesiredPropertiesPatchTopic + "#", expectedQos));
            mqttSubscribe.TopicFilters.Add(new(DirectMethods.DirectMethodClient.ClassicDirectMethodsRequestTopic + "#", expectedQos));
            var suback = await ManagedMqttConnection.BuildAndSubscribeAsync(ConnectionEndpoint.IotHub, () =>
            {
                string deviceId = GetDeviceIdForTopics();
                return mqttSubscribe.WithTopicFilters(mqttSubscribe.TopicFilters
                    .Select(topicFilter => topicFilter.Topic.Contains(IConnectionClient.DeviceIdPlaceholder, StringComparison.Ordinal)
                        ? new MqttTopicFilter(
                            topicFilter.Topic.Replace(IConnectionClient.DeviceIdPlaceholder, deviceId, StringComparison.Ordinal),
                            topicFilter.QualityOfServiceLevel)
                        : topicFilter)
                    .ToList());
            });

            bool anySubscribeFailed = false;
            foreach (var topicSuback in suback.Items)
            {
                anySubscribeFailed |= (topicSuback.ReasonCode != MqttClientSubscribeReasonCode.GrantedQoS0);
            }

            if (anySubscribeFailed)
            {
                // Signal to the underlying MQTT connection manager that, even though we are manually disconnecting, we still want to reconnect.
                await ManagedMqttConnection.DisconnectAsync(true, new MqttDisconnect() { Reason = MqttDisconnectReasonCode.NormalDisconnection, SessionExpiryInterval = 0 });

                Trace.TraceError("Device failed to subscribe to one or more necessary MQTT topics upon connecting to IoT Hub. Disconnecting the MQTT client and trying again.");
            }
            else
            {
                await RaiseDevicePresenceFlowCompletedAsync(new DevicePresenceFlowCompletedArgs() { IsSuccess = true });
            }
        }

        /// <summary>
        /// Send a certificate signing request to IoT hub
        /// </summary>
        /// <param name="request">The certificates to have IoT hub sign.</param>
        /// <param name="cancellationToken">Cancels sending the request and any unfinished tasks of the pending certificate signing operation.</param>
        /// <returns>A set of tasks. One that completes when IoT hub accepts the request (and starts signing), one that completes when IoT hub completes the signing, and one that completes if any step in the process fails.</returns>
        /// <remarks>Any MQTT disconnection cancels all pending certificate signing operations. Canceling the token removes only this request; it does not cancel certificate issuance on the service.</remarks>
        public async Task<CertificateSigningOperation> SendCertificateSigningRequestAsync(IotHubCertificateSigningRequest request, CancellationToken cancellationToken = default)
        {
            ObjectDisposedException.ThrowIf(_isDisposed, this);

            // The connection context survives a disconnect, so the endpoint is what says whether this client is connected.
            if (CurrentConnectionContext == null || CurrentEndpoint != ConnectionEndpoint.IotHub)
            {
                throw new NotSupportedException("Must be connected before calling this method.");
            }

            cancellationToken.ThrowIfCancellationRequested();

            CertificateSigningOperation operation = new();

            if (!_pendingCertificateSigningOperations.TryAdd(request.RequestId, operation))
            {
                throw new InvalidOperationException($"A certificate signing request with request ID '{request.RequestId}' is already pending.");
            }

            CancellationTokenRegistration cancellationRegistration = default;
            try
            {
                if (cancellationToken.CanBeCanceled)
                {
                    cancellationRegistration = cancellationToken.Register(() =>
                    {
                        if (_pendingCertificateSigningOperations.TryRemove(new KeyValuePair<string, CertificateSigningOperation>(request.RequestId, operation)))
                        {
                            operation.SetCanceled(cancellationToken);
                        }
                    });
                    _ = operation.Completed.ContinueWith(
                        _ => cancellationRegistration.Dispose(),
                        CancellationToken.None,
                        TaskContinuationOptions.ExecuteSynchronously,
                        TaskScheduler.Default);
                }

                await ManagedMqttConnection.SubscribeAsync(ConnectionEndpoint.IotHub, new(CertificateSigningResponseTopicFilter, MqttQualityOfServiceLevel.AtLeastOnce), cancellationToken); // TODO QoS correct?

                MqttPublish certificateSigningRequestPublish = new()
                {
                    Topic = CertificateSigningRequestTopic + request.RequestId,
                    Payload = JsonSerializer.SerializeToUtf8Bytes(request),
                };

                // Puback is checked for non-success cases under this layer, so no need to check it here as well
                await ManagedMqttConnection.PublishAsync(ConnectionEndpoint.IotHub, certificateSigningRequestPublish, cancellationToken: cancellationToken);
            }
            catch
            {
                _pendingCertificateSigningOperations.TryRemove(new KeyValuePair<string, CertificateSigningOperation>(request.RequestId, operation));
                cancellationRegistration.Dispose();
                throw;
            }

            return operation;
        }

        private Task HandleCertificateSigningDisconnectedAsync(MqttClientDisconnectedEventArgs args)
        {
            CancelPendingCertificateSigningOperations();

            return Task.CompletedTask;
        }

        private void CancelPendingCertificateSigningOperations()
        {
            foreach (var pendingOperation in _pendingCertificateSigningOperations.ToArray())
            {
                if (_pendingCertificateSigningOperations.TryRemove(pendingOperation))
                {
                    pendingOperation.Value.SetCanceled(CancellationToken.None);
                }
            }
        }

        private async Task HandleReceivedCertificateSigningPublish(MqttPublishReceivedEventArgs args)
        {
            if (args.Publish.Topic.StartsWith(CertificateSigningResponseTopic))
            {
                CertificateSigningOperation? pendingCertificateSigningOperation = null;
                string? requestId = null;
                string? status = null;

                try
                {
                    try
                    {
                        string[] topicTokens = args.Publish.Topic.Split("/");
                        if (topicTokens.Length != 5 || !topicTokens[4].StartsWith(RequestId, StringComparison.Ordinal))
                        {
                            Trace.TraceWarning("Received a certificate signing response with an invalid topic: {0}", args.Publish.Topic);
                            return;
                        }

                        status = topicTokens[3];
                        requestId = topicTokens[4][RequestId.Length..].Split('&')[0];

                        if (!_pendingCertificateSigningOperations.TryGetValue(requestId, out pendingCertificateSigningOperation))
                        {
                            return;
                        }

                        if (status.Equals("202"))
                        {
                            CertificateSigningRequestAccepted accepted = JsonSerializer.Deserialize<CertificateSigningRequestAccepted>(args.Publish.Payload)
                                ?? throw new JsonException("The certificate signing acceptance response was null.");
                            if (accepted.CorrelationId == null)
                            {
                                throw new JsonException("The certificate signing acceptance response had a null correlation ID.");
                            }

                            pendingCertificateSigningOperation.SetAccepted(accepted);
                        }
                        else if (status.Equals("200"))
                        {
                            CertificateSigningResponse response = JsonSerializer.Deserialize<CertificateSigningResponse>(args.Publish.Payload)
                                ?? throw new JsonException("The certificate signing completion response was null.");
                            if (response.Certificates == null || response.CorrelationId == null)
                            {
                                throw new JsonException("The certificate signing completion response had null certificates or correlation ID.");
                            }

                            if (!_pendingCertificateSigningOperations.TryRemove(new KeyValuePair<string, CertificateSigningOperation>(requestId, pendingCertificateSigningOperation)))
                            {
                                return;
                            }

                            // The authentication provider receives these on the next connection attempt to IoT hub.
                            if (CurrentConnectionContext != null)
                            {
                                CurrentConnectionContext.IssuedClientCertificates = response.Certificates;
                            }

                            pendingCertificateSigningOperation.SetCompleted(response);
                        }
                        else
                        {
                            CertificateSigningRequestErrorResponse error = JsonSerializer.Deserialize<CertificateSigningRequestErrorResponse>(args.Publish.Payload)
                                ?? throw new JsonException("The certificate signing error response was null.");
                            _pendingCertificateSigningOperations.TryRemove(new KeyValuePair<string, CertificateSigningOperation>(requestId, pendingCertificateSigningOperation));
                            pendingCertificateSigningOperation.SetFailed(new CertificateSigningRequestFailedException()
                            {
                                Error = error,
                                RequestId = requestId,
                            });
                        }
                    }
                    finally
                    {
                        await args.AcknowledgeAsync(CancellationToken.None);
                    }
                }
                catch (Exception ex)
                {
                    DeviceException failure = new(
                        $"Failed to handle certificate signing response with status '{status}' for request '{requestId}'. Response payload: {Encoding.UTF8.GetString(args.Publish.Payload)}",
                        ex)
                    {
                        Retryability = ErrorRetryability.Terminal,
                        IsContained = true,
                    };
                    if (pendingCertificateSigningOperation != null && requestId != null)
                    {
                        _pendingCertificateSigningOperations.TryRemove(new KeyValuePair<string, CertificateSigningOperation>(requestId, pendingCertificateSigningOperation));
                        pendingCertificateSigningOperation.SetFailed(failure);
                    }

                    Trace.TraceError("Failed to handle a certificate signing response. {0}", failure);
                }
            }
        }

        /// <summary>
        /// Releases the unmanaged resources used by this client and optionally disposes of the managed resources.
        /// </summary>
        /// <param name="disposing">true to release both managed and unmanaged resources; false to releases only unmanaged resources.</param>
        public override void Dispose(bool disposing)
        {
            CancelPendingCertificateSigningOperations();
            ManagedMqttConnection.PublishReceivedAsync -= HandleReceivedCertificateSigningPublish;
            ManagedMqttConnection.DisconnectedAsync -= HandleCertificateSigningDisconnectedAsync;
            base.Dispose(disposing);
        }

        /// <summary>
        /// Releases the unmanaged resources and disposes of the managed resources used by this client 
        /// </summary>
        public override void Dispose()
        {
            CancelPendingCertificateSigningOperations();
            ManagedMqttConnection.PublishReceivedAsync -= HandleReceivedCertificateSigningPublish;
            ManagedMqttConnection.DisconnectedAsync -= HandleCertificateSigningDisconnectedAsync;
            base.Dispose();
        }

        private static string GetUserAgentString()
        {
            const string name = "Microsoft.Azure.Iot.Device";

            string runtime = RuntimeInformation.FrameworkDescription.Trim();
            string operatingSystem = RuntimeInformation.OSDescription.Trim();
            string processorArchitecture = RuntimeInformation.ProcessArchitecture.ToString().Trim();

            string userAgent = $"{name}/{GetPackageVersion()} ({runtime}; {operatingSystem}; {processorArchitecture})";

            return userAgent;
        }

        private static string GetPackageVersion()
        {
            return typeof(ConnectionClient).GetTypeInfo().Assembly.GetName().Version!.ToString(3);
        }

        internal override bool DoesClientSupportHubType(ConnectionProfile connectionProfile)
        {
            return connectionProfile == ConnectionProfile.Classic;
        }
    }
}
