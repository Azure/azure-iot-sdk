// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Google.Protobuf;
using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Models.CertificateManagement;
using Microsoft.Azure.Iot.Device.Models.Twin;
using Microsoft.Azure.Iot.Device.Mqtt;
using System.Diagnostics;
using System.Reflection;
using System.Text;
using Microsoft.Azure.Iot.Device.Provisioning.Models;
using ConnectionClientOptions = Microsoft.Azure.Iot.Device.MQTTv5.Models.ConnectionClientOptions;

namespace Microsoft.Azure.Iot.Device.MQTTv5.Connection
{
    public class ConnectionClient : AbstractConnectionClient, IConnectionClient
    {
        private static TimeSpan birthAckReceivedDefensiveTimeout = TimeSpan.FromMinutes(5); // As prescribed by the service team

        private readonly bool _receiveDesiredPropertyUpdates;
        private readonly bool _receiveReportedPropertiesUponConnect;
        private readonly uint _sessionExpiryInterval;

        internal Guid? CurrentConnectionNonce { get; private set; }

        Guid? IConnectionClient.ConnectionNonce => CurrentConnectionNonce;

        private readonly ulong _initialDesiredPropertiesVersion;
        private readonly ulong _initialReportedPropertiesVersion;

        // The latest twin section versions that the device holds, which are announced in each birth.
        private ulong _desiredPropertiesVersion;
        private ulong _reportedPropertiesVersion;

        // The attempt (starting at 1) of the defensive timeout for the next twin push that a birth ack implies IoT hub will send.
        // It escalates each time the twin push doesn't arrive, and resets once one does.
        private int _twinPushTimeoutAttempt = 1;

        /// <summary>
        /// How long to wait for the twin push that a birth ack implies IoT hub will send, given the attempt (starting at 1),
        /// before reconnecting so that IoT hub sends it again.
        /// </summary>
        internal Func<int, TimeSpan> TwinPushTimeoutSchedule { get; set; } = DefensiveTimeoutSchedule.GetTimeout;

        ulong IConnectionClient.InitialDesiredPropertiesVersion => _initialDesiredPropertiesVersion;

        ulong IConnectionClient.InitialReportedPropertiesVersion => _initialReportedPropertiesVersion;

        void IConnectionClient.ObserveDesiredPropertiesVersion(ulong version) => AdvanceTo(ref _desiredPropertiesVersion, version);

        void IConnectionClient.ObserveReportedPropertiesVersion(ulong version) => AdvanceTo(ref _reportedPropertiesVersion, version);

        void IConnectionClient.ReplaceDesiredPropertiesVersion(ulong version) => Interlocked.Exchange(ref _desiredPropertiesVersion, version);

        void IConnectionClient.ReplaceReportedPropertiesVersion(ulong version) => Interlocked.Exchange(ref _reportedPropertiesVersion, version);

        private static void AdvanceTo(ref ulong location, ulong version)
        {
            ulong current = Interlocked.Read(ref location);
            while (version > current)
            {
                ulong original = Interlocked.CompareExchange(ref location, version, current);
                if (original == current)
                {
                    return;
                }

                current = original;
            }
        }

        public ConnectionClient(ConnectionClientOptions? options = null, ConnectionContext? connectionContext = null)
            : base((options ?? new ConnectionClientOptions()).ToCommonOptions(), connectionContext)
        {
            options ??= new ConnectionClientOptions();
            _receiveDesiredPropertyUpdates = options.ReceiveDesiredPropertyUpdates;
            _receiveReportedPropertiesUponConnect = options.ReceiveReportedPropertiesUponConnect;
            _sessionExpiryInterval = options.SessionExpiryInterval;
            _initialDesiredPropertiesVersion = options.InitialDesiredPropertiesVersion;
            _initialReportedPropertiesVersion = options.InitialReportedPropertiesVersion;
            _desiredPropertiesVersion = options.InitialDesiredPropertiesVersion;
            _reportedPropertiesVersion = options.InitialReportedPropertiesVersion;
        }

        public override Task HandleConnectedToHubAsync(MqttClientConnectedEventArgs args)
        {
            Debug.Assert(CurrentConnectionContext != null);

            return AnnounceDevicePresenceAsync(ManagedMqttConnection, CurrentConnectionContext.DeviceId, args, this);
        }

        /// <summary>
        /// Run the gen 2 device presence flow (subscribe to the device topic if needed, announce this device's birth
        /// and wait for the birth ack) on the provided connection, then raise <see cref="AbstractConnectionClient.DevicePresenceFlowCompletedAsync"/>.
        /// </summary>
        /// <remarks>
        /// The connection and device id are supplied by the caller rather than read from this client's own state so
        /// that the unified connection client can run this same flow on the connection that it owns when it is
        /// connected to a gen 2 hub. Likewise, <paramref name="subscriptionSourceOwner"/> is the client whose registered
        /// <see cref="IDevicePresenceSubscriptionSource"/>s are re-subscribed to alongside the devicebound topic.
        /// </remarks>
        internal async Task AnnounceDevicePresenceAsync(MqttConnectionManager connection, string deviceId, MqttClientConnectedEventArgs args, AbstractConnectionClient subscriptionSourceOwner)
        {
            // The unified client's nested MQTTv5 client runs this flow on its owner's connection and recovery lifetime.
            using CancellationTokenSource presenceCancellation = CancellationTokenSource.CreateLinkedTokenSource(
                connection.ConnectionLifetimeCancellationToken,
                subscriptionSourceOwner.CurrentReprovisioningCancellationToken);

            Task HandleConnectionLostAsync(MqttClientDisconnectedEventArgs _)
            {
                try
                {
                    presenceCancellation.Cancel();
                }
                catch (ObjectDisposedException)
                {
                    // A disconnect dispatch may have captured this handler before the announcement completed.
                }

                return Task.CompletedTask;
            }

            connection.DisconnectedAsync += HandleConnectionLostAsync;
            try
            {
                await AnnounceDevicePresenceCoreAsync(connection, deviceId, args, subscriptionSourceOwner, presenceCancellation.Token);
            }
            catch (OperationCanceledException) when (presenceCancellation.IsCancellationRequested)
            {
                // The connection layer owns recovery; an abandoned announcement must not restart it.
            }
            finally
            {
                connection.DisconnectedAsync -= HandleConnectionLostAsync;
            }
        }

        private async Task AnnounceDevicePresenceCoreAsync(MqttConnectionManager connection, string deviceId, MqttClientConnectedEventArgs args, AbstractConnectionClient subscriptionSourceOwner, CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            var connack = args.ConnectAck;

            Debug.Assert(CurrentConnectionNonce != null);
            Guid connectNonce = CurrentConnectionNonce!.Value;

            // Only send the subscribe if it hasn't been sent in this MQTT session yet
            if (!connack.IsSessionPresent) //TODO subscribe elide logic
            {
                try
                {
                    // Feature clients' subscriptions (such as custom topics) were lost with the session, so they are
                    // re-established in the same SUBSCRIBE as the devicebound topic. They are collected only once the
                    // SUBSCRIBE is about to be sent, so that it reflects every subscribe/unsubscribe made before then.
                    IReadOnlyList<DevicePresenceSubscriptionRequest> featureSubscriptions = Array.Empty<DevicePresenceSubscriptionRequest>();
                    int sentTopicFilterCount = 0;
                    var suback = await connection.BuildAndSubscribeAsync(
                        ConnectionEndpoint.IotHub,
                        () =>
                        {
                            featureSubscriptions = subscriptionSourceOwner.CollectDevicePresenceSubscriptions();
                            MqttSubscribe subscribe = new($"ih/{deviceId}/dev/#", MqttQualityOfServiceLevel.AtLeastOnce);
                            foreach (DevicePresenceSubscriptionRequest request in featureSubscriptions)
                            {
                                foreach (MqttTopicFilter topicFilter in request.TopicFilters)
                                {
                                    subscribe.TopicFilters.Add(new MqttTopicFilter(
                                        topicFilter.Topic.Replace(IConnectionClient.DeviceIdPlaceholder, deviceId, StringComparison.Ordinal),
                                        topicFilter.QualityOfServiceLevel));
                                }
                            }

                            sentTopicFilterCount = subscribe.TopicFilters.Count;
                            return subscribe;
                        },
                        // A feature client's topic being rejected must not fail the presence flow, so judge each item below
                        throwOnSubackErrorCode: false,
                        cancellationToken);

                    cancellationToken.ThrowIfCancellationRequested();

                    MqttSubscribeAckItem? presenceSuback = suback.Items.FirstOrDefault();
                    if (presenceSuback == null || suback.Items.Count != sentTopicFilterCount)
                    {
                        Trace.TraceWarning("Received malformed SUBACK on devicebound SUBSCRIBE. Attempting connection again...");
                        await connection.DisconnectAsync(true, new MqttDisconnect() { Reason = MqttDisconnectReasonCode.NormalDisconnection, SessionExpiryInterval = 0 }, cancellationToken);
                        return;
                    }

                    if (presenceSuback.ReasonCode != MqttClientSubscribeReasonCode.GrantedQoS1)
                    {
                        Trace.TraceWarning("Received SUBACK on device presence SUBSCRIBE with unsuccessful result code: {0}.", presenceSuback.ReasonCode);
                        await connection.DisconnectAsync(true, new MqttDisconnect() { Reason = MqttDisconnectReasonCode.NormalDisconnection, SessionExpiryInterval = 0 }, cancellationToken);
                        return;
                    }

                    ReportDevicePresenceSubscriptionResults(featureSubscriptions, suback.Items.Skip(1).ToList());
                }
                catch (Exception e) when (!cancellationToken.IsCancellationRequested)
                {
                    Trace.TraceWarning("Exception thrown while subscribing to devicebound topic. Attempting connection again...", e);
                    await connection.DisconnectAsync(true, new MqttDisconnect() { Reason = MqttDisconnectReasonCode.NormalDisconnection, SessionExpiryInterval = 0 }, cancellationToken);
                    return;
                }
            }

            // The latest versions of the twin's desired and reported properties that the device holds. IoT hub pushes a section upon
            // connecting (if requested) only if its version differs from the one announced here.
            ulong deviceDesiredPropertyVersion = Interlocked.Read(ref _desiredPropertiesVersion);
            ulong deviceReportedPropertyVersion = Interlocked.Read(ref _reportedPropertiesVersion);

            Birth birth = new()
            {
                SessionPresent = connack.IsSessionPresent,
                ReportedVersion = deviceReportedPropertyVersion, // Service allows for device to "resume" its previously known state if device boots up and has twin in durable storage somewhere.
                DesiredVersion = deviceDesiredPropertyVersion,

                PushDesired = _receiveDesiredPropertyUpdates, // If false, users will only get desired properties via a GetTwin call. Akin to subscribing to desired properties in v1 land
                PushReported = _receiveReportedPropertiesUponConnect, // If true, all current reported properties will be pushed to this device to "re-hydrate" its reported properties state. Not analogous to anything in v1 AFAIK
            };

            MqttPublish birthMessage = new MqttPublish()
            {
                Topic = string.Format("ih/{0}/srv/presence", deviceId),
                CorrelationData = connectNonce.ToByteArray(bigEndian: true),
                Payload = birth.ToByteArray(),
                QualityOfServiceLevel = MqttQualityOfServiceLevel.AtMostOnce, // QoS 0 because we don't care about the MQTT-level ack for this message.  The service will send a fully-fledged MQTT publish as the ack and we will listen for that below
            };

            birthMessage.UserProperties.Add(new("type", Encoding.UTF8.GetBytes("birth:1")));

            TaskCompletionSource<BirthAck> birthAckReceivedTaskCompletionSource = new();
            Func<MqttPublishReceivedEventArgs, Task> HandleReceivedBirthAck = async (args) =>
            {
                // Birth ack messages are QoS 0, so no need to ack
                MqttPublish publish = args.Publish;
                if (publish.Topic.Equals(string.Format("ih/{0}/dev/presence", deviceId)))
                {
                    if (publish.UserProperties.TryGetType(out string? messageType, out int? version))
                    {
                        if (messageType.Equals("birth-ack"))
                        {
                            if (GuidExtensions.TryParseBytes(publish.CorrelationData, out Guid? receivedGuid))
                            {
                                if (receivedGuid.Equals(connectNonce))
                                {
                                    // The birth message flow is only complete once Hub sends a birth message ack with connection epoch equal to the latest connection epoch we have attempted
                                    birthAckReceivedTaskCompletionSource.TrySetResult(BirthAck.Parser.ParseFrom(args.Publish.Payload));
                                }
                                else
                                {
                                    Trace.TraceWarning("Received birth ack, but for an unexpected connection nonce. Expected {0}, but was {1}", connectNonce.ToString(), receivedGuid.ToString());
                                }
                            }
                            else
                            {
                                Trace.TraceWarning("Received birth ack, with a malformed connection nonce");
                            }
                        }
                    }
                }
            };

            // IoT hub may send the twin push that this birth triggers before the birth ack, so watch for it from before sending the birth.
            TwinPushWatch? twinPushWatch = birth.PushDesired || birth.PushReported
                ? new TwinPushWatch(connection, deviceId, connectNonce)
                : null;

            connection.PublishReceivedAsync += HandleReceivedBirthAck;

            try
            {
                MqttPublishAck birthMessagePuback;
                try
                {
                    birthMessagePuback = await connection.PublishAsync(ConnectionEndpoint.IotHub, birthMessage, cancellationToken);
                }
                catch (Exception e) when (!cancellationToken.IsCancellationRequested)
                {
                    Trace.TraceWarning("Exception thrown while publishing birth message", e);
                    await connection.DisconnectAsync(true, new MqttDisconnect() { Reason = MqttDisconnectReasonCode.NormalDisconnection, SessionExpiryInterval = 0 }, cancellationToken);
                    return;
                }

                cancellationToken.ThrowIfCancellationRequested();

                if (birthMessagePuback.ReasonCode != MqttPublishAckReasonCode.Success)
                {
                    Trace.TraceWarning("Received unsuccessful PUBACK when publishing birth message with reason code: {0}.", birthMessagePuback.ReasonCode);
                    await connection.DisconnectAsync(true, new MqttDisconnect() { Reason = MqttDisconnectReasonCode.NormalDisconnection, SessionExpiryInterval = 0 }, cancellationToken);
                    return;
                }

                BirthAck birthAck;
                try
                {
                    birthAck = await birthAckReceivedTaskCompletionSource.Task.WaitAsync(birthAckReceivedDefensiveTimeout, cancellationToken);
                }
                catch (TimeoutException)
                {
                    Trace.TraceWarning("Timed out waiting for birth ack message");
                    await connection.DisconnectAsync(true, new MqttDisconnect() { Reason = MqttDisconnectReasonCode.NormalDisconnection, SessionExpiryInterval = 0 }, cancellationToken);
                    return;
                }

                if (twinPushWatch != null && IsTwinPushExpected(birth, birthAck))
                {
                    _ = WaitForExpectedTwinPushAsync(connection, subscriptionSourceOwner, connectNonce, twinPushWatch);
                    twinPushWatch = null;
                }
            }
            finally
            {
                connection.PublishReceivedAsync -= HandleReceivedBirthAck;
                twinPushWatch?.Stop();
            }

            cancellationToken.ThrowIfCancellationRequested();
            await RaiseDevicePresenceFlowCompletedAsync(new());
        }

        // Whether IoT hub sends a twin push upon admitting this birth. It does if a section the birth asked to be pushed has a
        // different version than the one the device holds.
        private static bool IsTwinPushExpected(Birth birth, BirthAck birthAck)
        {
            // IoT hub's versions are never 0, so a 0 means the birth ack didn't include that section's version.
            return (birth.PushDesired && birthAck.DesiredVersion != 0 && birthAck.DesiredVersion != birth.DesiredVersion)
                || (birth.PushReported && birthAck.ReportedVersion != 0 && birthAck.ReportedVersion != birth.ReportedVersion);
        }

        // Wait for the twin push that IoT hub should send for the given connection's birth. If it doesn't arrive in time, IoT hub
        // most likely failed to set up this connection for twin traffic (including future desired property patches), and only a
        // fresh birth makes it do so, so reconnect.
        private async Task WaitForExpectedTwinPushAsync(MqttConnectionManager connection, AbstractConnectionClient subscriptionSourceOwner, Guid connectNonce, TwinPushWatch twinPushWatch)
        {
            CancellationTokenSource? connectionLost = null;

            Task HandleConnectionLostAsync(MqttClientDisconnectedEventArgs _)
            {
                try
                {
                    connectionLost?.Cancel();
                }
                catch (ObjectDisposedException)
                {
                    // A disconnect dispatch may have captured this handler after the wait completed.
                }

                return Task.CompletedTask;
            }

            int attempt = Volatile.Read(ref _twinPushTimeoutAttempt);
            try
            {
                connectionLost = CancellationTokenSource.CreateLinkedTokenSource(
                    connection.ConnectionLifetimeCancellationToken,
                    subscriptionSourceOwner.CurrentReprovisioningCancellationToken);
                connection.DisconnectedAsync += HandleConnectionLostAsync;

                try
                {
                    await twinPushWatch.Received.WaitAsync(TwinPushTimeoutSchedule(attempt), connectionLost.Token);
                    Volatile.Write(ref _twinPushTimeoutAttempt, 1);
                }
                catch (TimeoutException)
                {
                    if (CurrentConnectionNonce != connectNonce)
                    {
                        // The device already reconnected, which made IoT hub decide whether to push the twin again.
                        return;
                    }

                    Volatile.Write(ref _twinPushTimeoutAttempt, attempt + 1);
                    Trace.TraceWarning("Timed out waiting for the twin push that IoT hub should send upon connecting. Reconnecting so that IoT hub sends it again.");

                    // Don't let the disconnect this causes cancel itself.
                    connection.DisconnectedAsync -= HandleConnectionLostAsync;
                    await connection.DisconnectAsync(true, new MqttDisconnect() { Reason = MqttDisconnectReasonCode.NormalDisconnection, SessionExpiryInterval = 0 }, connectionLost.Token);
                }
            }
            catch (Exception e) when (e is OperationCanceledException || e is ObjectDisposedException)
            {
                // The connection was lost or closed, so the next connection's birth decides whether IoT hub pushes the twin.
            }
            catch (Exception e)
            {
                Trace.TraceWarning("Failed to reconnect after timing out waiting for a twin push: {0}", e);
            }
            finally
            {
                connection.DisconnectedAsync -= HandleConnectionLostAsync;
                connectionLost?.Dispose();
                twinPushWatch.Stop();
            }
        }

        // Watches the given connection for the twin push that IoT hub sends upon admitting the birth with the given connection nonce.
        private sealed class TwinPushWatch
        {
            private readonly MqttConnectionManager _connection;
            private readonly string _topic;
            private readonly Guid _connectNonce;
            private readonly TaskCompletionSource _received = new(TaskCreationOptions.RunContinuationsAsynchronously);

            public TwinPushWatch(MqttConnectionManager connection, string deviceId, Guid connectNonce)
            {
                _connection = connection;
                _topic = $"ih/{deviceId}/dev/twin";
                _connectNonce = connectNonce;
                _connection.PublishReceivedAsync += HandlePublishReceivedAsync;
            }

            public Task Received => _received.Task;

            public void Stop()
            {
                _connection.PublishReceivedAsync -= HandlePublishReceivedAsync;
            }

            private Task HandlePublishReceivedAsync(MqttPublishReceivedEventArgs args)
            {
                MqttPublish publish = args.Publish;
                if (publish.Topic.Equals(_topic, StringComparison.Ordinal)
                    && publish.UserProperties.TryGetType(out string? messageType, out _)
                    && messageType.Equals("twin-push")
                    && GuidExtensions.TryParseBytes(publish.CorrelationData, out Guid? receivedNonce)
                    && receivedNonce.Value == _connectNonce)
                {
                    _received.TrySetResult();
                }

                return Task.CompletedTask;
            }
        }

        internal override MqttConnect IotHubMqttConnectOverride(MqttConnect connect)
        {
            CurrentConnectionNonce = Guid.NewGuid(); //Note that this nonce must be unique per connection attempt, not per successfuly connection

            string hexEncodedConnectNonce = Convert.ToHexString(CurrentConnectionNonce.Value.ToByteArray(bigEndian: true));

            // Should look something like "correlationId=4f3c2a1b9d8e47f0a1b2c3d4e5f60718&clientVersion=csharp%2F1.42.0"
            // TODO do we want to also include previous user agent details like OS, architecture, etc? Service currently discards those
            string username = $"correlationId={Uri.EscapeDataString(hexEncodedConnectNonce)}&clientVersion={Uri.EscapeDataString($"csharp/{GetPackageVersion()}")}";

            connect.WebsocketUri = BuildWebSocketUri(connect.HostName, connect.WebsocketPort, MqttV5HubWebSocketPath);
            connect.Username = username;
            connect.ProtocolVersion = MqttProtocolVersion.V500;
            connect.SessionExpiryInterval = _sessionExpiryInterval;

            return connect;
        }

        /// <summary>
        /// Send a certificate signing request to IoT hub
        /// </summary>
        /// <param name="request">The certificates to have IoT hub sign.</param>
        /// <param name="cancellationToken">The cancellation token</param>
        /// <returns>A set of tasks. One that completes when IoT hub accepts the request (and starts signing), one that completes when IoT hub completes the signing, and one that completes if any step in the process fails.</returns>
        public async Task<CertificateSigningOperation> SendCertificateSigningRequestAsync(IotHubCertificateSigningRequest request, CancellationToken cancellationToken = default)
        {
            ObjectDisposedException.ThrowIf(_isDisposed, this);

            throw new NotImplementedException("Not a supported feature on Gen 2 Hubs yet");
        }

        private static string GetPackageVersion()
        {
            return typeof(ConnectionClient).GetTypeInfo().Assembly.GetName().Version!.ToString(3);
        }

        internal override bool DoesClientSupportHubType(ConnectionProfile connectionProfile)
        {
            return connectionProfile == ConnectionProfile.MqttV5;
        }
    }
}
