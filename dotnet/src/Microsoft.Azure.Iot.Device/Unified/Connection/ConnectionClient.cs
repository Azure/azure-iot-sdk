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
using Microsoft.Azure.Iot.Device.Unified.Models;

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

        private MQTTv5.Connection.ConnectionClient _mqttv5ConnectionClient;

        // The current (or most recent) connection to an MQTTv3 IoT hub. Never null, so that the twin client holds reported
        // patches until the device presence flow completes on the first connection.
        private volatile ClassicConnection _classicConnection = new(Guid.NewGuid());

        Guid? IConnectionClient.ConnectionNonce => CurrentConnectionContext?.ConnectionProfile switch
        {
            IotHubConnectionType.Mqttv5 => _mqttv5ConnectionClient.CurrentConnectionNonce,
            IotHubConnectionType.Mqttv3 => _classicConnection.Nonce,
            _ => null,
        };

        event Action? IConnectionClient.ConnectionLost
        {
            add => HubConnectionLost += value;
            remove => HubConnectionLost -= value;
        }

        private event Action? ClassicDevicePresenceEstablished;

        event Action? IConnectionClient.ClassicDevicePresenceEstablished
        {
            add => ClassicDevicePresenceEstablished += value;
            remove => ClassicDevicePresenceEstablished -= value;
        }

        bool IConnectionClient.IsClassicDevicePresenceEstablished =>
            CurrentConnectionContext?.ConnectionProfile == IotHubConnectionType.Mqttv3
                && _classicConnection.IsPresenceEstablished;

        // The nested MQTTv5 client announces the twin versions that the device holds in each birth, so it keeps track of them
        // even while this client is connected to an MQTTv3 IoT hub, in case the device is later re-provisioned to an MQTTv5 one.
        private MQTTv5.Connection.IConnectionClient Mqttv5TwinVersions => _mqttv5ConnectionClient;

        BirthAckVersions? IConnectionClient.LatestBirthAck => Mqttv5TwinVersions.LatestBirthAck;

        void IConnectionClient.ObserveDesiredPropertiesVersion(ulong version) => Mqttv5TwinVersions.ObserveDesiredPropertiesVersion(version);

        void IConnectionClient.ObserveReportedPropertiesVersion(ulong version) => Mqttv5TwinVersions.ObserveReportedPropertiesVersion(version);

        void IConnectionClient.ReplaceDesiredPropertiesVersion(ulong version) => Mqttv5TwinVersions.ReplaceDesiredPropertiesVersion(version);

        void IConnectionClient.ReplaceReportedPropertiesVersion(ulong version) => Mqttv5TwinVersions.ReplaceReportedPropertiesVersion(version);

        /// <inheritdoc/>
        public bool IsTwinEnabled { get; }

        /// <inheritdoc/>
        public bool AreDirectMethodsEnabled { get; }

        /// <inheritdoc/>
        public bool AreCloudToDeviceMessagesEnabled { get; }

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

            // The nested gen 2 client only lends this client its connect packet logic and its device presence flow, both of
            // which it runs on the connection that this client owns. It never establishes a connection of its own, so it must
            // not be handed the application-supplied MQTT client. Doing so would attach a second MqttConnectionManager to that
            // same client, and that manager's "connecting" handler would rewrite every CONNECT this client sends (including the
            // one sent to DPS) with gen 2 hub credentials and MQTT 5.
            _mqttv5ConnectionClient = new(new MQTTv5.Models.ConnectionClientOptions()
            {
                ConnectionRetryPolicy = options.ConnectionRetryPolicy,
                ConnectionAttemptTimeout = options.ConnectionAttemptTimeout,
                EnableMqttLogging = options.EnableMqttLogging,
                ReceiveDesiredPropertyUpdates = options.EnableTwin,
                ReceiveReportedPropertiesUponConnect = options.EnableTwin,
                // MqttClient = options.MqttClient, //TODO See above for why this isn't passed to the MQTTv5 client, but this design feels a bit off, but needs testing of websocket + proxy support when using unified client to connect to MQTTv5 hub
            });

            // Snapshot the opt-outs so that later changes to the options object cannot change which topics the
            // presence flow subscribes to out from under feature clients that already checked them.
            IsTwinEnabled = options.EnableTwin;
            AreDirectMethodsEnabled = options.EnableDirectMethods;
            AreCloudToDeviceMessagesEnabled = options.EnableCloudToDeviceMessages;

            ManagedMqttConnection.PublishReceivedAsync += HandleReceivedCertificateSigningPublish;
            _mqttv5ConnectionClient.DevicePresenceFlowCompletedAsync += HandleMQTTv5ClientConnectionReady;
            ManagedMqttConnection.DisconnectedAsync += HandleCertificateSigningDisconnectedAsync;
            HubConnectionLost += () => _classicConnection.MarkLost();
        }

        private async Task HandleMQTTv5ClientConnectionReady(DevicePresenceFlowCompletedArgs args)
        {
            // When the underlying MQTTv5 connection client has re-established its presence, then it is ready to use (analogous to an MQTTv3 client that has finished re-subscribing to twin/direct methods/telemetry topics)
            await RaiseDevicePresenceFlowCompletedAsync(new DevicePresenceFlowCompletedArgs() { IsSuccess = true });
        }

        internal override MqttConnect IotHubMqttConnectOverride(MqttConnect connect)
        {
            Debug.Assert(CurrentConnectionContext != null);
            if (CurrentConnectionContext.ConnectionProfile == IotHubConnectionType.Mqttv5)
            {
                return _mqttv5ConnectionClient.IotHubMqttConnectOverride(connect);
            }

            connect.WebsocketUri = BuildWebSocketUri(connect.HostName, connect.WebsocketPort, DefaultWebSocketPath);
            connect.ProtocolVersion = CurrentConnectionContext.ConnectionProfile == IotHubConnectionType.Mqttv5 ? MqttProtocolVersion.V500 : MqttProtocolVersion.V311;

            connect.Username = $"{connect.HostName}/{connect.ClientId}/?api-version={ClassicHubApiVersion}&DeviceClientType={Uri.EscapeDataString(GetUserAgentString())}";

            // MQTTv3 flow does not need to insert anything unique per connect attempt
            return connect;
        }

        internal override void ResetBirthAckTimeoutSchedule()
        {
            // The nested gen 2 client runs the presence flow for MQTTv5 hubs, so it owns the schedule.
            _mqttv5ConnectionClient.ResetBirthAckTimeoutSchedule();
        }

        public override async Task HandleConnectedToHubAsync(MqttClientConnectedEventArgs args)
        {
            // Note that this callback handler is only set after provisioning has completed. It is only to respond to IoT Hub connection events, not DPS connection events.

            // This callback should only be reached after provisioning, so their should always be a connection context to use
            Debug.Assert(CurrentConnectionContext != null);

            if (CurrentConnectionContext.ConnectionProfile == IotHubConnectionType.Mqttv5)
            {
                // A gen 2 hub connection re-announces this device's birth instead of re-subscribing to the classic topics.
                // The gen 2 client owns that flow, but it must run on this client's connection. It signals that the device
                // is ready by raising its own device presence flow completed event, which this client relays.
                await _mqttv5ConnectionClient.AnnounceDevicePresenceAsync(ManagedMqttConnection, CurrentConnectionContext.DeviceId, args, this);
                return;
            }

            // Upon an MQTT connection being established, re-subscribe to the enabled twin/cloud-to-device/direct methods
            // topics unless the resumed session is known to still hold them.
            ClassicConnection classicConnection = new(Guid.NewGuid());
            _classicConnection = classicConnection;
            if (TryElideDevicePresenceSubscribe(args.ConnectAck))
            {
                await CompleteClassicDevicePresenceFlowAsync(classicConnection);

                return;
            }

            // A resumed session whose subscriptions this process has not observed (for example one left behind by a previous
            // process that had other features enabled) may still hold subscriptions for features that are now disabled.
            // Nothing would handle or acknowledge their messages, so those subscriptions are removed.
            MqttSubscribe mqttSubscribe = new();
            MqttUnsubscribe mqttUnsubscribe = new();
            if (AreCloudToDeviceMessagesEnabled)
            {
                mqttSubscribe.TopicFilters.Add(new(Telemetry.TelemetryClient.DeviceBoundMessagesTopicFormat + "#", MqttQualityOfServiceLevel.AtLeastOnce));
            }
            else
            {
                mqttUnsubscribe.TopicFilters.Add(Telemetry.TelemetryClient.DeviceBoundMessagesTopicFormat + "#");
            }

            if (IsTwinEnabled)
            {
                mqttSubscribe.TopicFilters.Add(new(Twin.TwinClient.ClassicTwinResponseTopic + "#", MqttQualityOfServiceLevel.AtMostOnce));
                mqttSubscribe.TopicFilters.Add(new(Twin.TwinClient.ClassicTwinDesiredPropertiesPatchTopic + "#", MqttQualityOfServiceLevel.AtMostOnce));
            }
            else
            {
                mqttUnsubscribe.TopicFilters.Add(Twin.TwinClient.ClassicTwinResponseTopic + "#");
                mqttUnsubscribe.TopicFilters.Add(Twin.TwinClient.ClassicTwinDesiredPropertiesPatchTopic + "#");
            }

            if (AreDirectMethodsEnabled)
            {
                mqttSubscribe.TopicFilters.Add(new(DirectMethods.DirectMethodClient.ClassicDirectMethodsRequestTopic + "#", MqttQualityOfServiceLevel.AtMostOnce));
            }
            else
            {
                mqttUnsubscribe.TopicFilters.Add(DirectMethods.DirectMethodClient.ClassicDirectMethodsRequestTopic + "#");
            }

            if (args.ConnectAck.IsSessionPresent && mqttUnsubscribe.TopicFilters.Count > 0
                && !await TryUnsubscribeDisabledFeaturesAsync(mqttUnsubscribe))
            {
                return;
            }

            if (mqttSubscribe.TopicFilters.Count == 0)
            {
                // A SUBSCRIBE with no topic filters is a protocol error, and there is nothing to wait for.
                MarkDevicePresenceSubscribed();
                await CompleteClassicDevicePresenceFlowAsync(classicConnection);

                return;
            }

            string deviceBoundMessagesTopic = string.Empty;
            var suback = await ManagedMqttConnection.BuildAndSubscribeAsync(ConnectionEndpoint.IotHub, () =>
            {
                string deviceId = GetDeviceIdForTopics();
                deviceBoundMessagesTopic = Telemetry.TelemetryClient.DeviceBoundMessagesTopicFormat
                    .Replace(IConnectionClient.DeviceIdPlaceholder, deviceId, StringComparison.Ordinal) + "#";
                return mqttSubscribe.WithTopicFilters(mqttSubscribe.TopicFilters
                    .Select(topicFilter => topicFilter.Topic.Contains(IConnectionClient.DeviceIdPlaceholder, StringComparison.Ordinal)
                        ? new MqttTopicFilter(
                            topicFilter.Topic.Replace(IConnectionClient.DeviceIdPlaceholder, deviceId, StringComparison.Ordinal),
                            topicFilter.QualityOfServiceLevel)
                        : topicFilter)
                    .ToList());
            }, CurrentReprovisioningCancellationToken);

            bool anySubscribeFailed = false;
            foreach (var topicSuback in suback.Items)
            {
                if (topicSuback.TopicFilter.Topic.Equals(deviceBoundMessagesTopic, StringComparison.Ordinal))
                {
                    anySubscribeFailed |= (topicSuback.ReasonCode != MqttClientSubscribeReasonCode.GrantedQoS1);
                }
                else
                { 
                    anySubscribeFailed |= (topicSuback.ReasonCode != MqttClientSubscribeReasonCode.GrantedQoS0);
                }
            }

            if (anySubscribeFailed)
            {
                // Signal to the underlying MQTT connection manager that, even though we are manually disconnecting, we still want to reconnect.
                await ManagedMqttConnection.DisconnectAsync(true, new MqttDisconnect() { Reason = MqttDisconnectReasonCode.NormalDisconnection });

                Trace.TraceError("Device failed to subscribe to one or more necessary MQTT topics upon connecting to IoT Hub. Disconnecting the MQTT client and trying again.");
            }
            else
            {
                MarkDevicePresenceSubscribed();
                await CompleteClassicDevicePresenceFlowAsync(classicConnection);
            }
        }

        private async Task CompleteClassicDevicePresenceFlowAsync(ClassicConnection classicConnection)
        {
            // Raised before the presence flow completes, so that the twin client has started fetching the twin by the time a
            // connect call returns to the application.
            if (classicConnection.TryMarkPresenceEstablished())
            {
                Action? handlers = ClassicDevicePresenceEstablished;
                foreach (Action handler in handlers?.GetInvocationList().Cast<Action>() ?? Enumerable.Empty<Action>())
                {
                    try
                    {
                        handler();
                    }
                    catch (Exception e)
                    {
                        Trace.TraceWarning("A device presence established handler threw and was ignored. {0}", e);
                    }
                }
            }

            await RaiseDevicePresenceFlowCompletedAsync(new DevicePresenceFlowCompletedArgs() { IsSuccess = true });
        }

        // A connection to an MQTTv3 IoT hub, which this client identifies with a locally generated nonce.
        private sealed class ClassicConnection(Guid nonce)
        {
            private const int Pending = 0;
            private const int Established = 1;
            private const int Lost = 2;

            private int _state = Pending;

            public Guid Nonce { get; } = nonce;

            public bool IsPresenceEstablished => Volatile.Read(ref _state) == Established;

            // Returns false if the connection was already lost, so that a presence flow that completes late never marks it.
            public bool TryMarkPresenceEstablished() => Interlocked.CompareExchange(ref _state, Established, Pending) == Pending;

            public void MarkLost() => Interlocked.Exchange(ref _state, Lost);
        }

        /// <summary>
        /// Remove a resumed session's subscriptions for disabled features.
        /// </summary>
        /// <returns>
        /// True if the presence flow can continue; false if it was abandoned because the connection was lost, or if the
        /// connection was torn down so that it is retried.
        /// </returns>
        private async Task<bool> TryUnsubscribeDisabledFeaturesAsync(MqttUnsubscribe mqttUnsubscribe)
        {
            // Abandon the UNSUBSCRIBE once this connection is lost, so that a failure on it can never tear down a newer one.
            using CancellationTokenSource presenceCancellation = CancellationTokenSource.CreateLinkedTokenSource(
                ManagedMqttConnection.ConnectionLifetimeCancellationToken,
                CurrentReprovisioningCancellationToken);

            Task HandleConnectionLostAsync(MqttClientDisconnectedEventArgs _)
            {
                try
                {
                    presenceCancellation.Cancel();
                }
                catch (ObjectDisposedException)
                {
                    // A disconnect dispatch may have captured this handler before the UNSUBSCRIBE completed.
                }

                return Task.CompletedTask;
            }

            ManagedMqttConnection.DisconnectedAsync += HandleConnectionLostAsync;
            try
            {
                var unsuback = await ManagedMqttConnection.BuildAndUnsubscribeAsync(ConnectionEndpoint.IotHub, () =>
                {
                    string deviceId = GetDeviceIdForTopics();
                    return mqttUnsubscribe.WithTopicFilters(mqttUnsubscribe.TopicFilters
                        .Select(topicFilter => topicFilter.Replace(IConnectionClient.DeviceIdPlaceholder, deviceId, StringComparison.Ordinal))
                        .ToList());
                }, throwOnUnsubackErrorCode: false, presenceCancellation.Token);

                presenceCancellation.Token.ThrowIfCancellationRequested();

                if (unsuback.Items.Any(item => item.ReasonCode != MqttClientUnsubscribeReasonCode.Success
                    && item.ReasonCode != MqttClientUnsubscribeReasonCode.NoSubscriptionExisted))
                {
                    Trace.TraceError("Device failed to unsubscribe from the MQTT topics of disabled features upon resuming a session with IoT Hub. Disconnecting the MQTT client and trying again.");

                    // Signal to the underlying MQTT connection manager that, even though we are manually disconnecting, we still want to reconnect.
                    await ManagedMqttConnection.DisconnectAsync(true, new MqttDisconnect() { Reason = MqttDisconnectReasonCode.NormalDisconnection }, presenceCancellation.Token);
                    return false;
                }

                return true;
            }
            catch (OperationCanceledException) when (presenceCancellation.IsCancellationRequested)
            {
                // The connection layer owns recovery; an abandoned presence flow must not restart it.
                return false;
            }
            catch (Exception e) when (!presenceCancellation.IsCancellationRequested)
            {
                // Without this, nothing would reconnect or complete the presence flow while the connection stays up.
                Trace.TraceError("Exception thrown while unsubscribing from the MQTT topics of disabled features upon resuming a session with IoT Hub. Disconnecting the MQTT client and trying again. {0}", e);
                try
                {
                    await ManagedMqttConnection.DisconnectAsync(true, new MqttDisconnect() { Reason = MqttDisconnectReasonCode.NormalDisconnection }, presenceCancellation.Token);
                }
                catch (OperationCanceledException) when (presenceCancellation.IsCancellationRequested)
                {
                    // The connection was lost or closed meanwhile, so the connection layer owns recovery.
                }

                return false;
            }
            finally
            {
                ManagedMqttConnection.DisconnectedAsync -= HandleConnectionLostAsync;
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

            if (CurrentConnectionContext.ConnectionProfile == IotHubConnectionType.Mqttv5)
            {
                return await _mqttv5ConnectionClient.SendCertificateSigningRequestAsync(request, cancellationToken);
            }
            else
            {
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

        private Task HandleReceivedCertificateSigningPublish(MqttPublishReceivedEventArgs args)
        {
            if (args.Publish.Topic.StartsWith(CertificateSigningResponseTopic))
            {
                CertificateSigningOperation? pendingCertificateSigningOperation = null;
                string? requestId = null;
                string? status = null;

                try
                {
                    string[] topicTokens = args.Publish.Topic.Split("/");
                    if (topicTokens.Length != 5 || !topicTokens[4].StartsWith(RequestId, StringComparison.Ordinal))
                    {
                        Trace.TraceWarning("Received a certificate signing response with an invalid topic: {0}", args.Publish.Topic);
                        return Task.CompletedTask;
                    }

                    status = topicTokens[3];
                    requestId = topicTokens[4][RequestId.Length..].Split('&')[0];

                    if (!_pendingCertificateSigningOperations.TryGetValue(requestId, out pendingCertificateSigningOperation))
                    {
                        return Task.CompletedTask;
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
                            return Task.CompletedTask;
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

            return Task.CompletedTask;
        }

        /// <summary>
        /// Releases the unmanaged resources used by this client and optionally disposes of the managed resources.
        /// </summary>
        /// <param name="disposing">true to release both managed and unmanaged resources; false to releases only unmanaged resources.</param>
        public override void Dispose(bool disposing)
        {
            CancelPendingCertificateSigningOperations();
            ManagedMqttConnection.PublishReceivedAsync -= HandleReceivedCertificateSigningPublish;
            _mqttv5ConnectionClient.DevicePresenceFlowCompletedAsync -= HandleMQTTv5ClientConnectionReady;
            _mqttv5ConnectionClient.Dispose(disposing);
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
            _mqttv5ConnectionClient.DevicePresenceFlowCompletedAsync -= HandleMQTTv5ClientConnectionReady;
            _mqttv5ConnectionClient.Dispose();
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
            return connectionProfile == ConnectionProfile.MqttV5 || connectionProfile == ConnectionProfile.Classic;
        }
    }
}
