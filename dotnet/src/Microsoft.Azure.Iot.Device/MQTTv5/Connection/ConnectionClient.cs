// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Google.Protobuf;
using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Models.CertificateManagement;
using Microsoft.Azure.Iot.Device.Models.Twin;
using Microsoft.Azure.Iot.Device.Mqtt;
using System.Diagnostics;
using System.Diagnostics.CodeAnalysis;
using System.Reflection;
using System.Text;
using Microsoft.Azure.Iot.Device.Provisioning.Models;
using ConnectionClientOptions = Microsoft.Azure.Iot.Device.MQTTv5.Models.ConnectionClientOptions;

namespace Microsoft.Azure.Iot.Device.MQTTv5.Connection
{
    public class ConnectionClient : AbstractConnectionClient, IConnectionClient
    {
        private const string ProtobufContentType = "application/protobuf";

        // Base birth-ack timeouts for consecutive timeouts 0, 1, 2 and 3+, per the device presence spec. Each is extended
        // by a uniform random jitter of up to s_birthAckTimeoutMaxJitter so that devices on a degraded backend don't
        // reconnect in lockstep.
        private static readonly TimeSpan[] s_birthAckTimeoutBases =
        {
            TimeSpan.FromMinutes(5),
            TimeSpan.FromMinutes(6),
            TimeSpan.FromMinutes(8),
            TimeSpan.FromMinutes(10),
        };

        private static readonly TimeSpan s_birthAckTimeoutMaxJitter = TimeSpan.FromMinutes(5);

        private readonly bool _receiveDesiredPropertyUpdates;
        private readonly bool _receiveReportedPropertiesUponConnect;
        private readonly uint _sessionExpiryInterval;

        private Guid? CurrentConnectionNonce { get; set; }

        // How many presence flows in a row have timed out waiting for their birth-ack. Selects the next birth-ack
        // timeout, and resets once a birth-ack arrives.
        private int _consecutiveBirthAckTimeouts;

        // Guards _consecutiveBirthAckTimeouts together with CurrentConnectionNonce, so that only the current connection
        // attempt's presence flow can accept a birth-ack and reset the streak.
        private readonly object _birthAckLock = new();

        /// <summary>
        /// Picks the birth-ack timeout from the number of consecutive birth-ack timeouts so far. Replaceable by tests.
        /// </summary>
        internal Func<int, TimeSpan> BirthAckTimeoutProvider { get; set; } =
            consecutiveTimeouts => GetBirthAckTimeout(consecutiveTimeouts, Random.Shared.NextDouble());

        /// <summary>
        /// The birth-ack timeout schedule from the device presence spec: 5, 6, 8 and then 10 minutes, each plus up to
        /// 5 minutes of jitter.
        /// </summary>
        /// <param name="consecutiveTimeouts">How many birth-ack timeouts in a row preceded this attempt.</param>
        /// <param name="jitterFraction">A value in [0, 1) that selects how much of the maximum jitter to add.</param>
        internal static TimeSpan GetBirthAckTimeout(int consecutiveTimeouts, double jitterFraction)
        {
            TimeSpan baseTimeout = s_birthAckTimeoutBases[Math.Clamp(consecutiveTimeouts, 0, s_birthAckTimeoutBases.Length - 1)];
            return baseTimeout + s_birthAckTimeoutMaxJitter * Math.Clamp(jitterFraction, 0, 1);
        }

        private const string BirthAckType = "birth-ack";
        private const string SupportedBirthAckType = BirthAckType + ":1";

        // True for any version of the birth-ack, including ones this SDK cannot process.
        private static bool IsAnyBirthAckType(string messageType)
        {
            return messageType.StartsWith(BirthAckType, StringComparison.Ordinal)
                && (messageType.Length == BirthAckType.Length || messageType[BirthAckType.Length] == ':');
        }

        // Rejects an undecodable payload rather than completing the flow without it: the birth-ack carries per-feature
        // recovery state that must not be silently dropped. The birth-ack timeout then reconnects and births again.
        private static bool TryParseBirthAck(byte[] payload, [NotNullWhen(true)] out BirthAck? birthAck)
        {
            try
            {
                birthAck = BirthAck.Parser.ParseFrom(payload);
                return true;
            }
            catch (InvalidProtocolBufferException e)
            {
                Trace.TraceWarning("Ignoring birth ack with a malformed payload. {0}", e.Message);
                birthAck = null;
                return false;
            }
        }

        public ConnectionClient(ConnectionClientOptions? options = null, ConnectionContext? connectionContext = null)
            : base((options ?? new ConnectionClientOptions()).ToCommonOptions(), connectionContext)
        {
            options ??= new ConnectionClientOptions();
            _receiveDesiredPropertyUpdates = options.ReceiveDesiredPropertyUpdates;
            _receiveReportedPropertiesUponConnect = options.ReceiveReportedPropertiesUponConnect;
            _sessionExpiryInterval = options.SessionExpiryInterval;
        }

        public override Task HandleConnectedToHubAsync(MqttClientConnectedEventArgs args)
        {
            Debug.Assert(CurrentConnectionContext != null);

            return AnnounceDevicePresenceAsync(ManagedMqttConnection, CurrentConnectionContext.DeviceId, args, this);
        }

        internal override void ResetBirthAckTimeoutSchedule()
        {
            lock (_birthAckLock)
            {
                _consecutiveBirthAckTimeouts = 0;
            }
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

            // Read this connection's per-attempt state before the first await. The connection layer raises ConnectedAsync
            // (and the handlers run synchronously up to here) before it can start another CONNECT attempt, whose
            // ConnectingAsync would replace the nonce and the subscribed snapshot.
            Debug.Assert(CurrentConnectionNonce != null);
            Guid connectNonce = CurrentConnectionNonce!.Value;
            bool elideSubscribe = subscriptionSourceOwner.TryElideDevicePresenceSubscribe(connack);

            // Subscribe-elide: skip the SUBSCRIBE only when the resumed session is known to still hold the subscriptions.
            if (!elideSubscribe)
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
                            featureSubscriptions = subscriptionSourceOwner.CollectDevicePresenceSubscriptions(connack.IsSessionPresent);
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
                        await connection.DisconnectAsync(true, new MqttDisconnect() { Reason = MqttDisconnectReasonCode.NormalDisconnection }, cancellationToken);
                        return;
                    }

                    if (presenceSuback.ReasonCode != MqttClientSubscribeReasonCode.GrantedQoS1)
                    {
                        Trace.TraceWarning("Received SUBACK on device presence SUBSCRIBE with unsuccessful result code: {0}.", presenceSuback.ReasonCode);
                        await connection.DisconnectAsync(true, new MqttDisconnect() { Reason = MqttDisconnectReasonCode.NormalDisconnection }, cancellationToken);
                        return;
                    }

                    subscriptionSourceOwner.MarkDevicePresenceSubscribed();
                    ReportDevicePresenceSubscriptionResults(featureSubscriptions, suback.Items.Skip(1).ToList());
                }
                catch (Exception e) when (!cancellationToken.IsCancellationRequested)
                {
                    Trace.TraceWarning("Exception thrown while subscribing to devicebound topic. Attempting connection again...", e);
                    await connection.DisconnectAsync(true, new MqttDisconnect() { Reason = MqttDisconnectReasonCode.NormalDisconnection }, cancellationToken);
                    return;
                }
            }

            // The device's last known version of the twin's reported and desired properties. Currently, there is no support for persisting this state on disk, so assume the device has never seen the twin.
            uint deviceDesiredPropertyVersion = 0;
            uint deviceReportedPropertyVersion = 0;

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
                ContentType = ProtobufContentType,
                QualityOfServiceLevel = MqttQualityOfServiceLevel.AtMostOnce, // QoS 0 because we don't care about the MQTT-level ack for this message.  The service will send a fully-fledged MQTT publish as the ack and we will listen for that below
            };

            birthMessage.UserProperties.Add(new("type", Encoding.UTF8.GetBytes("birth:1")));

            TaskCompletionSource<BirthAck> birthAckReceivedTaskCompletionSource = new(TaskCreationOptions.RunContinuationsAsynchronously);
            string birthAckTopic = string.Format("ih/{0}/dev/presence", deviceId);
            Func<MqttPublishReceivedEventArgs, Task> HandleReceivedBirthAck = (args) =>
            {
                // Birth ack messages are QoS 0, so no need to ack
                MqttPublish publish = args.Publish;
                if (!publish.Topic.Equals(birthAckTopic, StringComparison.Ordinal)
                    || !publish.UserProperties.TryGetProperty("type", out string? messageType)
                    || messageType == null)
                {
                    return Task.CompletedTask;
                }

                if (!messageType.Equals(SupportedBirthAckType, StringComparison.Ordinal))
                {
                    // Another schema version may not be readable as version 1, so leave it to the birth-ack timeout.
                    if (IsAnyBirthAckType(messageType))
                    {
                        Trace.TraceWarning("Ignoring birth ack of unsupported type {0}; only {1} is supported.", messageType, SupportedBirthAckType);
                    }
                }
                else
                {
                    if (GuidExtensions.TryParseBytes(publish.CorrelationData, out Guid? receivedGuid))
                    {
                        if (receivedGuid.Equals(connectNonce))
                        {
                            // The birth message flow is only complete once Hub sends a birth message ack with connection epoch equal to the latest connection epoch we have attempted
                            if (TryParseBirthAck(publish.Payload, out BirthAck? birthAck))
                            {
                                // Reset the streak as soon as the ack is accepted, even if the flow is canceled before it
                                // observes the ack. An abandoned handler cannot reset a newer attempt's streak because a
                                // newer CONNECT attempt replaces the nonce under the same lock.
                                lock (_birthAckLock)
                                {
                                    if (CurrentConnectionNonce == connectNonce
                                        && birthAckReceivedTaskCompletionSource.TrySetResult(birthAck))
                                    {
                                        _consecutiveBirthAckTimeouts = 0;
                                    }
                                }
                            }
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

                return Task.CompletedTask;
            };

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
                    await connection.DisconnectAsync(true, new MqttDisconnect() { Reason = MqttDisconnectReasonCode.NormalDisconnection }, cancellationToken);
                    return;
                }

                cancellationToken.ThrowIfCancellationRequested();

                if (birthMessagePuback.ReasonCode != MqttPublishAckReasonCode.Success)
                {
                    Trace.TraceWarning("Received unsuccessful PUBACK when publishing birth message with reason code: {0}.", birthMessagePuback.ReasonCode);
                    await connection.DisconnectAsync(true, new MqttDisconnect() { Reason = MqttDisconnectReasonCode.NormalDisconnection }, cancellationToken);
                    return;
                }

                // Armed only once the birth has been handed to the MQTT client, as the spec requires.
                int consecutiveTimeouts;
                lock (_birthAckLock)
                {
                    consecutiveTimeouts = _consecutiveBirthAckTimeouts;
                }

                TimeSpan birthAckTimeout = BirthAckTimeoutProvider(consecutiveTimeouts);
                bool birthAckTimedOut = false;
                try
                {
                    await birthAckReceivedTaskCompletionSource.Task.WaitAsync(birthAckTimeout, cancellationToken);
                }
                catch (TimeoutException)
                {
                    lock (_birthAckLock)
                    {
                        // Claim the attempt so that a birth-ack racing with the timeout is either accepted (and the
                        // flow completes) or counted as a timeout, never both.
                        birthAckTimedOut = birthAckReceivedTaskCompletionSource.TrySetCanceled();
                        if (birthAckTimedOut)
                        {
                            _consecutiveBirthAckTimeouts++;
                        }
                    }
                }

                if (birthAckTimedOut)
                {
                    // The session may not hold the subscriptions the service expects, so re-subscribe on the next connection.
                    subscriptionSourceOwner.ClearDevicePresenceSubscribed();
                    Trace.TraceWarning(
                        "Timed out after {0} waiting for birth ack message for connection nonce {1} ({2} consecutive birth ack timeouts)",
                        birthAckTimeout,
                        connectNonce,
                        consecutiveTimeouts + 1);
                    await connection.DisconnectAsync(true, new MqttDisconnect() { Reason = MqttDisconnectReasonCode.NormalDisconnection }, cancellationToken);
                    return;
                }
            }
            finally
            {
                connection.PublishReceivedAsync -= HandleReceivedBirthAck;
            }

            cancellationToken.ThrowIfCancellationRequested();
            await RaiseDevicePresenceFlowCompletedAsync(new());
        }

        internal override MqttConnect IotHubMqttConnectOverride(MqttConnect connect)
        {
            lock (_birthAckLock)
            {
                CurrentConnectionNonce = Guid.NewGuid(); //Note that this nonce must be unique per connection attempt, not per successfully connection
            }

            // The service's auth webhook expects exactly 32 lowercase hex characters; anything else is recorded as malformed
            // and dropped, losing the correlation between this attempt and its birth.
            string hexEncodedConnectNonce = Convert.ToHexStringLower(CurrentConnectionNonce.Value.ToByteArray(bigEndian: true));
            Trace.TraceInformation("Connecting to IoT hub with connection nonce {0}", hexEncodedConnectNonce);

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
