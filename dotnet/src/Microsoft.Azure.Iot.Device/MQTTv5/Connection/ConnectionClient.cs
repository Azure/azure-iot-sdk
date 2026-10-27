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

                try
                {
                    await birthAckReceivedTaskCompletionSource.Task.WaitAsync(birthAckReceivedDefensiveTimeout, cancellationToken);
                }
                catch (TimeoutException)
                {
                    Trace.TraceWarning("Timed out waiting for birth ack message");
                    await connection.DisconnectAsync(true, new MqttDisconnect() { Reason = MqttDisconnectReasonCode.NormalDisconnection, SessionExpiryInterval = 0 }, cancellationToken);
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
