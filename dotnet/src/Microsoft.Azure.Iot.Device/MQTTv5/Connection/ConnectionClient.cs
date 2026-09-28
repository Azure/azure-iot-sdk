// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Google.Protobuf;
using Microsoft.Azure.Iot.Device.MQTTv5.Twin;
using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Models.CertificateManagement;
using Microsoft.Azure.Iot.Device.Models.Twin;
using Microsoft.Azure.Iot.Device.Mqtt;
using System.Diagnostics;
using System.Reflection;
using System.Text;

namespace Microsoft.Azure.Iot.Device.MQTTv5.Connection
{
    public class ConnectionClient : AbstractConnectionClient, IConnectionClient
    {
        private static TimeSpan birthAckReceivedDefensiveTimeout = TimeSpan.FromSeconds(60); //TODO value is magic number

        private readonly TwinPushOptions _twinPushOptions;

        private Guid? CurrentConnectionNonce { get; set; }

        public ConnectionClient(ConnectionClientOptions? options = null, TwinPushOptions? twinPushOptions = null) : base(options)
        {
            _twinPushOptions = twinPushOptions ?? new TwinPushOptions();
        }

        public override Task HandleConnectedToHubAsync(MqttClientConnectedEventArgs args)
        {
            Debug.Assert(CurrentConnectionContext != null);

            return AnnounceDevicePresenceAsync(ManagedMqttConnection, CurrentConnectionContext.DeviceId, args);
        }

        /// <summary>
        /// Run the gen 2 device presence flow (subscribe to the device topic if needed, announce this device's birth
        /// and wait for the birth ack) on the provided connection, then raise <see cref="AbstractConnectionClient.DevicePresenceFlowCompletedAsync"/>.
        /// </summary>
        /// <remarks>
        /// The connection and device id are supplied by the caller rather than read from this client's own state so
        /// that the unified connection client can run this same flow on the connection that it owns when it is
        /// connected to a gen 2 hub.
        /// </remarks>
        internal async Task AnnounceDevicePresenceAsync(MqttConnectionManager connection, string deviceId, MqttClientConnectedEventArgs args)
        {
            //TODO do we need any sort of cancellation handling here if the connect call's cancellation token triggers?

            //TODO we definitely need some cancellation token to pass in to all these publishes/subscribes. Doesn't hub doc have some recommended defensive timeout here?

            var connack = args.ConnectAck;

            Debug.Assert(CurrentConnectionNonce != null);
            Guid connectNonce = CurrentConnectionNonce!.Value;

            // Only send the subscribe if it hasn't been sent in this MQTT session yet
            if (!connack.IsSessionPresent) //TODO subscribe elide logic
            {
                try
                {
                    var suback = await connection.SubscribeAsync(new(string.Format("ih/{0}/dev/#", deviceId), MqttQualityOfServiceLevel.AtLeastOnce));
                    var subackFirstItem = suback.Items.FirstOrDefault();
                    if (subackFirstItem == null)
                    {
                        Trace.TraceWarning("Received malformed SUBACK. Attempting connection again...");
                        await connection.DisconnectAsync(true, new MqttDisconnect() { Reason = MqttDisconnectReasonCode.NormalDisconnection, SessionExpiryInterval = 0 });
                        return;
                    }

                    if (subackFirstItem == null)
                    {
                        Trace.TraceWarning($"Received malformed SUBACK on devicebound SUBSCRIBE.");
                        await connection.DisconnectAsync(true, new MqttDisconnect() { Reason = MqttDisconnectReasonCode.NormalDisconnection, SessionExpiryInterval = 0 });
                        return;
                    }

                    if (subackFirstItem.ReasonCode != MqttClientSubscribeReasonCode.GrantedQoS1)
                    {
                        Trace.TraceWarning("Received SUBACK on devicebound SUBSCRIBE with unsuccessful result code: {0}.", subackFirstItem.ReasonCode);
                        await connection.DisconnectAsync(true, new MqttDisconnect() { Reason = MqttDisconnectReasonCode.NormalDisconnection, SessionExpiryInterval = 0 });
                        return;
                    }
                }
                catch (Exception e)
                {
                    Trace.TraceWarning("Exception thrown while subscribing to devicebound topic. Attempting connection again...", e);
                    await connection.DisconnectAsync(true, new MqttDisconnect() { Reason = MqttDisconnectReasonCode.NormalDisconnection, SessionExpiryInterval = 0 });
                    return;
                }
            }

            // The device's last known version of the twin's reported and desired properties. Currently, there is no support for persisting this state on disk, so assume the device has never seen the twin.
            uint deviceDesiredPropertyVersion = 0;
            uint deviceReportedPropertyVersion = 0;

            bool pushDesired = _twinPushOptions.ReceiveDesiredPropertyUpdates;
            bool pushReported = _twinPushOptions.ReceiveReportedPropertiesUponConnect;

            Birth birth = new()
            {
                SessionPresent = connack.IsSessionPresent,
                ReportedVersion = deviceReportedPropertyVersion, // Service allows for device to "resume" its previously known state if device boots up and has twin in durable storage somewhere. Not something we supported in v1, though AFAIK
                DesiredVersion = deviceDesiredPropertyVersion,

                PushDesired = pushDesired, // If false, users will only get desired properties via a GetTwin call. Akin to subscribing to desired properties in v1 land
                PushReported = pushReported, // If true, all current reported properties will be pushed to this device to "re-hydrate" its reported properties state. Not analogous to anything in v1 AFAIK
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

            TaskCompletionSource<TwinPush> twinPushReceivedTaskCompletionSource = new();

            connection.PublishReceivedAsync += HandleReceivedBirthAck;

            MqttPublishAck birthMessagePuback;
            try
            {
                birthMessagePuback = await connection.PublishAsync(birthMessage);
            }
            catch (Exception e)
            {
                Trace.TraceWarning("Exception thrown while publishing birth message", e);
                connection.PublishReceivedAsync -= HandleReceivedBirthAck;
                await connection.DisconnectAsync(true, new MqttDisconnect() { Reason = MqttDisconnectReasonCode.NormalDisconnection, SessionExpiryInterval = 0 });
                return;
            }

            if (birthMessagePuback.ReasonCode != MqttPublishAckReasonCode.Success)
            {
                await connection.DisconnectAsync(true, new MqttDisconnect() { Reason = MqttDisconnectReasonCode.NormalDisconnection, SessionExpiryInterval = 0 });
                Trace.TraceWarning("Received unsuccessful PUBACK when publishing birth message with reason code: {0}.", birthMessagePuback.ReasonCode);
                return;
            }

            BirthAck birthAck;
            try
            {
                birthAck = await birthAckReceivedTaskCompletionSource.Task.WaitAsync(birthAckReceivedDefensiveTimeout);
            }
            catch (TimeoutException)
            {
                // Did not receive mqtt birth ack message in timely manner (and user has not canceled this function yet)
                await connection.DisconnectAsync(true, new MqttDisconnect() { Reason = MqttDisconnectReasonCode.NormalDisconnection, SessionExpiryInterval = 0 });
                Trace.TraceWarning("Timed out waiting for birth ack message");
                return;
            }

            // Birth ack was received, so stop listening for birth acks.
            connection.PublishReceivedAsync -= HandleReceivedBirthAck;

            await RaiseDevicePresenceFlowCompletedAsync(new());
        }

        public override MqttConnect MqttConnectOverride(MqttConnect connect)
        {
            CurrentConnectionNonce = Guid.NewGuid(); //Note that this nonce must be unique per connection attempt, not per successfuly connection

            string hexEncodedConnectNonce = Convert.ToHexString(CurrentConnectionNonce.Value.ToByteArray(bigEndian: true));

            // Should look something like "correlationId=4f3c2a1b9d8e47f0a1b2c3d4e5f60718&clientVersion=csharp%2F1.42.0"
            // TODO do we want to also include previous user agent details like OS, architecture, etc? Service currently discards those
            string username = $"correlationId={Uri.EscapeDataString(hexEncodedConnectNonce)}&clientVersion={Uri.EscapeDataString($"csharp/{GetPackageVersion()}")}";

            connect.WebsocketUri = $"wss://{connect.HostName}/$iothub/websocket";
            connect.Username = username;
            connect.Password = Array.Empty<byte>();
            connect.ProtocolVersion = MqttProtocolVersion.V500;

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
    }
}
