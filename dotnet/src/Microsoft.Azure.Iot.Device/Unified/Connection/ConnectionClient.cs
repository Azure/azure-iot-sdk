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

        // IoT hub error codes that a resubmitted certificate signing request recovers from.
        private const int ConflictingRequestErrorCode = 409005;
        private const int NoRequestToReplaceErrorCode = 412001;

        private readonly object _certificateSigningLock = new();

        // Keyed by each request's current request ID. Guarded by _certificateSigningLock.
        private readonly Dictionary<string, PendingCertificateSigning> _pendingCertificateSignings = new(StringComparer.Ordinal);

        // The current (or most recent) connection to IoT hub. Never null, so that the twin client holds reported patches until
        // the device presence flow completes on the first connection.
        private volatile ClassicConnection _classicConnection = new(Guid.NewGuid());

        Guid? IConnectionClient.ConnectionNonce => CurrentConnectionContext != null ? _classicConnection.Nonce : null;

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
            CurrentConnectionContext != null && _classicConnection.IsPresenceEstablished;

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

            // Snapshot the opt-outs so that later changes to the options object cannot change which topics the
            // presence flow subscribes to out from under feature clients that already checked them.
            IsTwinEnabled = options.EnableTwin;
            AreDirectMethodsEnabled = options.EnableDirectMethods;
            AreCloudToDeviceMessagesEnabled = options.EnableCloudToDeviceMessages;

            ManagedMqttConnection.PublishReceivedAsync += HandleReceivedCertificateSigningPublish;
            ClassicDevicePresenceEstablished += HandleCertificateSigningPresenceEstablished;
            DeliberatelyDisconnected += CancelPendingCertificateSigningOperations;
            UnrecoverablyFaulted += CancelPendingCertificateSigningOperations;
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

            connect.WebsocketUri = BuildWebSocketUri(connect.HostName, connect.WebsocketPort, DefaultWebSocketPath);
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

        // A connection to IoT hub, which this client identifies with a locally generated nonce.
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
        /// <remarks>
        /// <para>
        /// If the connection to IoT hub is lost before IoT hub delivers the issued certificate, the operation stays pending
        /// and this client resends the request (with the same request ID) once it reconnects to the same IoT hub as the
        /// same device. If IoT hub rejects that resubmission because the earlier request is still active (409005), this
        /// client resubmits it with a new request ID that replaces the earlier one; if IoT hub then reports that there is
        /// nothing to replace (412001), it sends the request once more with a new request ID and without replacing
        /// anything. The request ID reported by <see cref="CertificateSigningRequestFailedException.RequestId"/> may
        /// therefore be one this client generated.
        /// </para>
        /// <para>
        /// Pending certificate signing operations are canceled if the application disconnects or disposes this client, or if
        /// this client stops reconnecting because of a fault it cannot recover from. They fail if this client reconnects
        /// to a different IoT hub or as a different device. Canceling the token removes only this request; it does not
        /// cancel certificate issuance on the service.
        /// </para>
        /// </remarks>
        public async Task<CertificateSigningOperation> SendCertificateSigningRequestAsync(IotHubCertificateSigningRequest request, CancellationToken cancellationToken = default)
        {
            ObjectDisposedException.ThrowIf(_isDisposed, this);

            // The connection context survives a disconnect, so the endpoint is what says whether this client is connected.
            ConnectionContext? connectionContext = CurrentConnectionContext;
            if (connectionContext == null || CurrentEndpoint != ConnectionEndpoint.IotHub)
            {
                throw new NotSupportedException("Must be connected before calling this method.");
            }

            cancellationToken.ThrowIfCancellationRequested();

            CertificateSigningOperation operation = new();
            PendingCertificateSigning pending = new(operation, request, connectionContext, cancellationToken);
            Guid claimedNonce;

            lock (_certificateSigningLock)
            {
                if (!_pendingCertificateSignings.TryAdd(pending.RequestId, pending))
                {
                    throw new InvalidOperationException($"A certificate signing request with request ID '{request.RequestId}' is already pending.");
                }

                // Claim the current connection so that a reconnection racing this call resends the request only if it is
                // a different connection from the one this call sends it on.
                claimedNonce = _classicConnection.Nonce;
                pending.SentConnectionNonce = claimedNonce;
            }

            CancellationTokenRegistration cancellationRegistration = default;
            try
            {
                if (cancellationToken.CanBeCanceled)
                {
                    cancellationRegistration = cancellationToken.Register(() =>
                    {
                        if (TryRemovePendingCertificateSigning(pending))
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

                // Puback is checked for non-success cases under this layer, so no need to check it here as well
                await PublishPendingCertificateSigningAsync(pending, request.RequestId, claimedNonce);
            }
            catch (StaleCertificateSigningSendException)
            {
                // A reconnection already took over sending this request.
            }
            catch (MqttClientNotConnectedException) when (!IsConnectionAbandoned)
            {
                // The connection was lost before IoT hub acknowledged the request. It stays pending and is resent once this
                // client reconnects.
                ReleaseCertificateSigningConnectionClaim(pending, claimedNonce);
            }
            catch
            {
                TryRemovePendingCertificateSigning(pending);
                cancellationRegistration.Dispose();
                throw;
            }

            return operation;
        }

        /// <summary>
        /// Publish a pending request as the given request ID, on the connection it was claimed for.
        /// </summary>
        /// <remarks>
        /// Sends may be queued (for example, behind the SUBACK, or on the thread pool), so whether the send is still wanted is
        /// only decided once it is admitted for IoT hub, when the connection can no longer change underneath it. A send whose
        /// request was canceled, completed, or resubmitted under another request ID, or that another send took over, is
        /// discarded with <see cref="StaleCertificateSigningSendException"/> (or the request's own cancellation). A send whose
        /// claimed connection was replaced, or that would go out as a different identity, throws
        /// <see cref="MqttClientNotConnectedException"/>, so that the caller releases its claim and the request is resent
        /// (or failed) by the current connection.
        /// </remarks>
        private Task<MqttPublishAck> PublishPendingCertificateSigningAsync(PendingCertificateSigning pending, string requestId, Guid claimedNonce)
        {
            return ManagedMqttConnection.BuildAndPublishAsync(
                ConnectionEndpoint.IotHub,
                () =>
                {
                    pending.CancellationToken.ThrowIfCancellationRequested();
                    lock (_certificateSigningLock)
                    {
                        if (!_pendingCertificateSignings.TryGetValue(requestId, out PendingCertificateSigning? current)
                            || current != pending
                            || pending.SentConnectionNonce != claimedNonce)
                        {
                            throw new StaleCertificateSigningSendException();
                        }

                        ConnectionContext? connectionContext = CurrentConnectionContext;
                        if (_classicConnection.Nonce != claimedNonce || connectionContext == null || !pending.IsForIdentity(connectionContext))
                        {
                            throw new MqttClientNotConnectedException("The connection this certificate signing request was to be sent on was replaced.");
                        }

                        return BuildCertificateSigningPublish(pending);
                    }
                },
                pending.CancellationToken);
        }

        private static MqttPublish BuildCertificateSigningPublish(PendingCertificateSigning pending)
        {
            IotHubCertificateSigningRequest request = new(pending.DeviceId, pending.CertificateSigningRequestData, pending.RequestId, pending.Replace);
            return new MqttPublish()
            {
                Topic = CertificateSigningRequestTopic + request.RequestId,
                Payload = JsonSerializer.SerializeToUtf8Bytes(request),
            };
        }

        private bool TryRemovePendingCertificateSigning(PendingCertificateSigning pending)
        {
            lock (_certificateSigningLock)
            {
                return _pendingCertificateSignings.TryGetValue(pending.RequestId, out PendingCertificateSigning? current)
                    && current == pending
                    && _pendingCertificateSignings.Remove(pending.RequestId);
            }
        }

        /// <summary>
        /// Forget that a pending request was sent on the given connection, because sending it there failed, so that it is
        /// resent on the next connection (or on the current one, if its device presence flow already completed).
        /// </summary>
        private void ReleaseCertificateSigningConnectionClaim(PendingCertificateSigning pending, Guid claimedNonce)
        {
            lock (_certificateSigningLock)
            {
                if (pending.SentConnectionNonce == claimedNonce)
                {
                    pending.SentConnectionNonce = null;
                }
            }

            // The current connection's resend may already have run and skipped this request while it was claimed.
            ClassicConnection classicConnection = _classicConnection;
            if (CurrentConnectionContext != null && classicConnection.IsPresenceEstablished)
            {
                _ = Task.Run(() => ResendPendingCertificateSigningsAsync(classicConnection.Nonce));
            }
        }

        private void HandleCertificateSigningPresenceEstablished()
        {
            Guid nonce = _classicConnection.Nonce;
            _ = Task.Run(() => ResendPendingCertificateSigningsAsync(nonce));
        }

        /// <summary>
        /// Resend each pending certificate signing request that was not already sent on the given connection.
        /// </summary>
        private async Task ResendPendingCertificateSigningsAsync(Guid nonce)
        {
            List<(PendingCertificateSigning Pending, string RequestId)> toSend = new();
            List<PendingCertificateSigning> toFail = new();
            ConnectionContext? connectionContext = CurrentConnectionContext;

            lock (_certificateSigningLock)
            {
                // A newer connection resends the requests itself.
                if (_classicConnection.Nonce != nonce || _pendingCertificateSignings.Count == 0)
                {
                    return;
                }

                foreach (PendingCertificateSigning pending in _pendingCertificateSignings.Values.ToList())
                {
                    if (pending.SentConnectionNonce == nonce)
                    {
                        continue;
                    }

                    if (connectionContext == null || !pending.IsForIdentity(connectionContext))
                    {
                        _pendingCertificateSignings.Remove(pending.RequestId);
                        toFail.Add(pending);
                        continue;
                    }

                    pending.SentConnectionNonce = nonce;
                    pending.IsCurrentRequestResent = true;
                    toSend.Add((pending, pending.RequestId));
                }
            }

            foreach (PendingCertificateSigning pending in toFail)
            {
                pending.Operation.SetFailed(new DeviceException(
                    "The certificate signing request was abandoned because this client reconnected to a different IoT hub or as a different device before the request completed.")
                {
                    Retryability = ErrorRetryability.Terminal,
                    IsContained = true,
                });
            }

            if (toSend.Count == 0)
            {
                return;
            }

            try
            {
                await ManagedMqttConnection.SubscribeAsync(ConnectionEndpoint.IotHub, new(CertificateSigningResponseTopicFilter, MqttQualityOfServiceLevel.AtLeastOnce));
            }
            catch (Exception e) when (e is MqttClientNotConnectedException || e is OperationCanceledException)
            {
                // This connection was lost (or closed) meanwhile. The next connection resends the requests.
                return;
            }
            catch (Exception e)
            {
                foreach ((PendingCertificateSigning pending, _) in toSend)
                {
                    if (TryRemovePendingCertificateSigning(pending))
                    {
                        pending.Operation.SetFailed(e);
                    }
                }

                return;
            }

            await Task.WhenAll(toSend.Select(item => SendPendingCertificateSigningAsync(item.Pending, item.RequestId, nonce)));
        }

        /// <summary>
        /// Send a request that is already pending. If the connection is lost first, the request is resent on the next one.
        /// </summary>
        private async Task SendPendingCertificateSigningAsync(PendingCertificateSigning pending, string requestId, Guid claimedNonce)
        {
            try
            {
                await PublishPendingCertificateSigningAsync(pending, requestId, claimedNonce);
            }
            catch (MqttClientNotConnectedException)
            {
                ReleaseCertificateSigningConnectionClaim(pending, claimedNonce);
            }
            catch (OperationCanceledException)
            {
                // The application canceled this request (which already removed it), this client is shutting down, or the
                // send became stale before it was admitted.
            }
            catch (Exception e)
            {
                if (TryRemovePendingCertificateSigning(pending))
                {
                    pending.Operation.SetFailed(e);
                }
            }
        }

        private void CancelPendingCertificateSigningOperations()
        {
            List<PendingCertificateSigning> pendingOperations;
            lock (_certificateSigningLock)
            {
                pendingOperations = _pendingCertificateSignings.Values.ToList();
                _pendingCertificateSignings.Clear();
            }

            foreach (PendingCertificateSigning pending in pendingOperations)
            {
                pending.Operation.SetCanceled(CancellationToken.None);
            }
        }

        private Task HandleReceivedCertificateSigningPublish(MqttPublishReceivedEventArgs args)
        {
            if (args.Publish.Topic.StartsWith(CertificateSigningResponseTopic))
            {
                PendingCertificateSigning? pending = null;
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

                    lock (_certificateSigningLock)
                    {
                        if (!_pendingCertificateSignings.TryGetValue(requestId, out pending))
                        {
                            return Task.CompletedTask;
                        }
                    }

                    if (status.Equals("202"))
                    {
                        CertificateSigningRequestAccepted accepted = JsonSerializer.Deserialize<CertificateSigningRequestAccepted>(args.Publish.Payload)
                            ?? throw new JsonException("The certificate signing acceptance response was null.");
                        if (accepted.CorrelationId == null)
                        {
                            throw new JsonException("The certificate signing acceptance response had a null correlation ID.");
                        }

                        pending.Operation.SetAccepted(accepted);
                    }
                    else if (status.Equals("200"))
                    {
                        CertificateSigningResponse response = JsonSerializer.Deserialize<CertificateSigningResponse>(args.Publish.Payload)
                            ?? throw new JsonException("The certificate signing completion response was null.");
                        if (response.Certificates == null || response.CorrelationId == null)
                        {
                            throw new JsonException("The certificate signing completion response had null certificates or correlation ID.");
                        }

                        if (!TryRemovePendingCertificateSigning(pending))
                        {
                            return Task.CompletedTask;
                        }

                        // The authentication provider receives these on the next connection attempt to IoT hub.
                        if (CurrentConnectionContext != null)
                        {
                            CurrentConnectionContext.IssuedClientCertificates = response.Certificates;
                        }

                        pending.Operation.SetCompleted(response);
                    }
                    else
                    {
                        CertificateSigningRequestErrorResponse error = JsonSerializer.Deserialize<CertificateSigningRequestErrorResponse>(args.Publish.Payload)
                            ?? throw new JsonException("The certificate signing error response was null.");
                        HandleCertificateSigningErrorResponse(pending, requestId, error);
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
                    if (pending != null && TryRemovePendingCertificateSigning(pending))
                    {
                        pending.Operation.SetFailed(failure);
                    }

                    Trace.TraceError("Failed to handle a certificate signing response. {0}", failure);
                }
            }

            return Task.CompletedTask;
        }

        /// <summary>
        /// Fail a pending request that IoT hub rejected, unless the rejection is one this client recovers from by
        /// resubmitting the request.
        /// </summary>
        private void HandleCertificateSigningErrorResponse(PendingCertificateSigning pending, string requestId, CertificateSigningRequestErrorResponse error)
        {
            string? recoveryRequestId = null;
            Guid claimedNonce = default;

            lock (_certificateSigningLock)
            {
                if (!_pendingCertificateSignings.TryGetValue(requestId, out PendingCertificateSigning? current) || current != pending)
                {
                    return;
                }

                _pendingCertificateSignings.Remove(requestId);

                bool isRecoverable = false;
                if (error.ErrorCode == ConflictingRequestErrorCode && pending.IsCurrentRequestResent)
                {
                    // The request this client resent after a connection loss is still active on IoT hub (for example
                    // because IoT hub accepted it before the connection was lost), so replace it with a new request.
                    pending.Replace = requestId;
                    pending.IsReplaceRecovery = true;
                    isRecoverable = true;
                }
                else if (error.ErrorCode == NoRequestToReplaceErrorCode && pending.IsReplaceRecovery)
                {
                    // The request being replaced ended meanwhile, so send the request again without replacing anything.
                    pending.Replace = null;
                    pending.IsReplaceRecovery = false;
                    isRecoverable = true;
                }

                if (isRecoverable)
                {
                    pending.RequestId = Guid.NewGuid().ToString();
                    pending.IsCurrentRequestResent = false;
                    _pendingCertificateSignings.Add(pending.RequestId, pending);
                    claimedNonce = _classicConnection.Nonce;
                    pending.SentConnectionNonce = claimedNonce;
                    recoveryRequestId = pending.RequestId;
                }
            }

            if (recoveryRequestId == null)
            {
                pending.Operation.SetFailed(new CertificateSigningRequestFailedException()
                {
                    Error = error,
                    RequestId = requestId,
                });

                return;
            }

            // Not awaited, because waiting for the PUBACK here would block the delivery of received publishes.
            _ = Task.Run(() => SendPendingCertificateSigningAsync(pending, recoveryRequestId, claimedNonce));
        }

        // Discards a queued send of a certificate signing request that is no longer wanted. An OperationCanceledException, so
        // that the connection layer passes it through as-is.
        private sealed class StaleCertificateSigningSendException : OperationCanceledException
        {
            public StaleCertificateSigningSendException()
                : base("The certificate signing request send is stale.")
            {
            }
        }

        // A certificate signing request that IoT hub has not finished, and what is needed to resend it.
        private sealed class PendingCertificateSigning
        {
            public PendingCertificateSigning(
                CertificateSigningOperation operation,
                IotHubCertificateSigningRequest request,
                ConnectionContext connectionContext,
                CancellationToken cancellationToken)
            {
                Operation = operation;
                DeviceId = request.DeviceId;
                CertificateSigningRequestData = request.CertificateSigningRequestData;
                RequestId = request.RequestId;
                Replace = request.Replace;
                HubDeviceId = connectionContext.DeviceId;
                IotHubHostName = connectionContext.IotHubHostName;
                CancellationToken = cancellationToken;
            }

            public CertificateSigningOperation Operation { get; }

            public string DeviceId { get; }

            public string CertificateSigningRequestData { get; }

            public CancellationToken CancellationToken { get; }

            // The identity this client connected with when the request was first sent.
            public string HubDeviceId { get; }

            public string IotHubHostName { get; }

            // The remaining members are guarded by _certificateSigningLock.

            // The request ID that IoT hub knows this request by, which changes when this client resubmits it to recover.
            public string RequestId { get; set; }

            public string? Replace { get; set; }

            // The connection the request was most recently sent on, or null if it still has to be sent.
            public Guid? SentConnectionNonce { get; set; }

            // Whether this client resent the current request ID after a connection loss.
            public bool IsCurrentRequestResent { get; set; }

            // Whether the current request ID replaces an earlier request that this client resent.
            public bool IsReplaceRecovery { get; set; }

            public bool IsForIdentity(ConnectionContext connectionContext) =>
                string.Equals(HubDeviceId, connectionContext.DeviceId, StringComparison.Ordinal)
                && string.Equals(IotHubHostName, connectionContext.IotHubHostName, StringComparison.OrdinalIgnoreCase);
        }

        /// <summary>
        /// Releases the unmanaged resources used by this client and optionally disposes of the managed resources.
        /// </summary>
        /// <param name="disposing">true to release both managed and unmanaged resources; false to releases only unmanaged resources.</param>
        public override void Dispose(bool disposing)
        {
            CancelPendingCertificateSigningOperations();
            UnsubscribeCertificateSigningHandlers();
            base.Dispose(disposing);
        }

        /// <summary>
        /// Releases the unmanaged resources and disposes of the managed resources used by this client 
        /// </summary>
        public override void Dispose()
        {
            CancelPendingCertificateSigningOperations();
            UnsubscribeCertificateSigningHandlers();
            base.Dispose();
        }

        private void UnsubscribeCertificateSigningHandlers()
        {
            ManagedMqttConnection.PublishReceivedAsync -= HandleReceivedCertificateSigningPublish;
            ClassicDevicePresenceEstablished -= HandleCertificateSigningPresenceEstablished;
            DeliberatelyDisconnected -= CancelPendingCertificateSigningOperations;
            UnrecoverablyFaulted -= CancelPendingCertificateSigningOperations;
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
