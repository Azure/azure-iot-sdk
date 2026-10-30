// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Google.Protobuf;
using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Models.Twin;
using Microsoft.Azure.Iot.Device.MQTTv5.Connection;
using Microsoft.Azure.Iot.Device.Mqtt;
using System.Diagnostics;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace Microsoft.Azure.Iot.Device.Twin
{
    // The twin transport of IoT hubs that use MQTTv5. The device announces the twin versions it holds in each connection's
    // birth message, IoT hub acknowledges them (revealing its own versions) and pushes the twin sections that changed, and
    // tags connection scoped messages with the connection nonce.
    internal sealed class Mqttv5TwinTransport : ITwinTransport
    {
        private const string ProtobufContentType = "application/protobuf";

        // The connection client substitutes this device's current ID for the placeholder when it sends each request, so a
        // pending request still goes to the right topic if the device ID changes (for example, after re-provisioning).
        private const string OutgoingTwinTopic = "ih/" + IConnectionClient.DeviceIdPlaceholder + "/srv/twin";
        private const string IncomingTwinTopic = "ih/" + IConnectionClient.DeviceIdPlaceholder + "/dev/twin";
        private const string IncomingPresenceTopic = "ih/" + IConnectionClient.DeviceIdPlaceholder + "/dev/presence";

        private readonly IConnectionClient _connection;

        // Whether this transport passes the connection's events to the handler. A transport that is composed with another one
        // leaves that to its owner.
        private readonly bool _handlesConnectionEvents;

        private ITwinTransportHandler? _handler;

        public Mqttv5TwinTransport(IConnectionClient connection, bool handlesConnectionEvents)
        {
            _connection = connection;
            _handlesConnectionEvents = handlesConnectionEvents;
        }

        public bool AnnouncesTwinVersions => true;

        // IoT hub pushes the twin sections that changed instead.
        public TwinSnapshot? ConnectTwinSnapshot => null;

        public Guid? ConnectionNonce => _connection.ConnectionNonce;

        public bool IsConnected => _connection.GetCurrentConnectionContext() != null && _connection.IsConnectionTargetSet;

        public bool IsReady => IsConnected;

        public BirthAckVersions? LatestBirthAck => _connection.LatestBirthAck;

        public bool IsBirthAckTimedOut(Guid connectionNonce) => _connection.IsBirthAckTimedOut(connectionNonce);

        public ulong InitialDesiredPropertiesVersion => _connection.InitialDesiredPropertiesVersion;

        public ulong InitialReportedPropertiesVersion => _connection.InitialReportedPropertiesVersion;

        public void ObserveDesiredPropertiesVersion(ulong version) => _connection.ObserveDesiredPropertiesVersion(version);

        public void ObserveReportedPropertiesVersion(ulong version) => _connection.ObserveReportedPropertiesVersion(version);

        public void ReplaceDesiredPropertiesVersion(ulong version) => _connection.ReplaceDesiredPropertiesVersion(version);

        public void ReplaceReportedPropertiesVersion(ulong version) => _connection.ReplaceReportedPropertiesVersion(version);

        public void Start(ITwinTransportHandler handler)
        {
            _handler = handler;
            _connection.PublishReceivedAsync += HandleReceivedMqttPublishAsync;
            if (_handlesConnectionEvents)
            {
                _connection.ConnectionLost += HandleConnectionLost;
                _connection.TwinIdentityChanged += handler.HandleTwinIdentityChanged;
            }
        }

        public void Detach()
        {
            _connection.PublishReceivedAsync -= HandleReceivedMqttPublishAsync;
            if (_handlesConnectionEvents && _handler != null)
            {
                _connection.ConnectionLost -= HandleConnectionLost;
                _connection.TwinIdentityChanged -= _handler.HandleTwinIdentityChanged;
            }
        }

        private void HandleConnectionLost()
        {
            _handler?.HandleConnectionLost(_connection.ConnectionNonce);
        }

        public async Task PublishGetAsync(Guid getId, bool getDesired, bool getReported, ulong ifNotMatchDesired, ulong ifNotMatchReported, CancellationToken cancellationToken)
        {
            MqttPublish publish = new()
            {
                Topic = OutgoingTwinTopic,
                QualityOfServiceLevel = MqttQualityOfServiceLevel.AtMostOnce,
                CorrelationData = getId.ToByteArray(bigEndian: true),
                Payload = new TwinGet()
                {
                    Sections = getReported
                        ? getDesired ? Sections.Both : Sections.Reported
                        : Sections.Desired,
                    IfNotMatchDesired = ifNotMatchDesired,
                    IfNotMatchReported = ifNotMatchReported,
                }.ToByteArray(),
                ContentType = ProtobufContentType,
            };

            publish.UserProperties.Add(new("type", Encoding.UTF8.GetBytes("get:1")));

            Trace.TraceInformation("Publishing 'GetTwin' request on topic " + publish.Topic);

            // Puback is checked for non-success cases under this layer, so no need to check it here as well
            await _connection.PublishAsync(publish, throwOnErrorReasonCode: true, cancellationToken).ConfigureAwait(false);
        }

        public async Task PublishReportedPatchAsync(Guid patchId, ulong ifMatch, byte[] payload, CancellationToken cancellationToken)
        {
            MqttPublish publish = new()
            {
                Topic = OutgoingTwinTopic,
                QualityOfServiceLevel = MqttQualityOfServiceLevel.AtMostOnce,
                CorrelationData = patchId.ToByteArray(bigEndian: true),
                Payload = new ReportedPatch()
                {
                    IfMatch = ifMatch, // If this value is 0, the if-match field will not be included in the serialized payload and the request is treated as unconditional
                    Payload = ByteString.CopyFrom(payload),
                }.ToByteArray(),
                ContentType = ProtobufContentType,
            };

            publish.UserProperties.Add(new("type", Encoding.UTF8.GetBytes("reported-patch:1")));

            Trace.TraceInformation("Publishing 'PatchReported' request on topic " + publish.Topic);

            // Puback is checked for non-success cases under this layer, so no need to check it here as well
            await _connection.PublishAsync(publish, throwOnErrorReasonCode: true, cancellationToken).ConfigureAwait(false);
        }

        private async Task HandleReceivedMqttPublishAsync(MqttPublishReceivedEventArgs args)
        {
            ITwinTransportHandler? handler = _handler;
            string topic = args.Publish.Topic;
            bool isTwinTopic = topic.EndsWith("/dev/twin", StringComparison.Ordinal);
            bool isPresenceTopic = topic.EndsWith("/dev/presence", StringComparison.Ordinal);
            if (handler == null || !topic.StartsWith("ih/", StringComparison.Ordinal) || !(isTwinTopic || isPresenceTopic))
            {
                // The publish is not relevant to this transport, so ignore it. This check needs to happen prior to checking the deviceId within the topic b/c deviceId is
                // not available until after provisioning finishes and this transport may be set up prior to provisioning. This allows this transport to ignore DPS
                // publishes without needing to know the deviceId.
                return;
            }

            ConnectionContext? connectionContext = _connection.GetCurrentConnectionContext();
            if (connectionContext == null)
            {
                Trace.TraceWarning("Cannot handle a received MQTT message while disconnected");
                return;
            }

            string expectedTopic = isTwinTopic ? IncomingTwinTopic : IncomingPresenceTopic;
            if (!topic.Equals(expectedTopic.Replace(IConnectionClient.DeviceIdPlaceholder, connectionContext.DeviceId, StringComparison.Ordinal), StringComparison.Ordinal))
            {
                // This message wasn't for this device so ignore it
                return;
            }

            MqttPublish publish = args.Publish;
            if (isPresenceTopic)
            {
                await HandleReceivedPresencePublishAsync(handler, publish).ConfigureAwait(false);
                return;
            }

            if (!publish.UserProperties.TryGetType(out string? type, out int? typeVersion))
            {
                Trace.TraceWarning("Received a twin message, but it is either missing the message type or the message type is malformed. Ignoring it.");
                return;
            }

            if (typeVersion != 1)
            {
                // The service may increment the protocol version of these messages over time. For now, there is only the one version, though
                Trace.TraceWarning("Received a twin message whose type version ({0}) is not supported by this client (supported version: {1}). You may need to upgrade this library's version to handle this kind of message. Ignoring it.", typeVersion, 1);
                return;
            }

            switch (type)
            {
                case "get-response":
                    await HandleReceivedGetResponseAsync(handler, publish).ConfigureAwait(false);
                    break;

                case "reported-patch-response":
                    await HandleReceivedReportedPatchResponseAsync(handler, publish).ConfigureAwait(false);
                    break;

                case "desired-patch":
                    await HandleReceivedDesiredPatchAsync(handler, publish, type).ConfigureAwait(false);
                    break;

                case "twin-push":
                    await HandleReceivedTwinPushAsync(handler, publish, type).ConfigureAwait(false);
                    break;
            }
        }

        private async Task HandleReceivedPresencePublishAsync(ITwinTransportHandler handler, MqttPublish publish)
        {
            // The connection client validates and acts on the birth ack's other contents. The engine only adopts its twin versions.
            if (!publish.UserProperties.TryGetType(out string? type, out int? typeVersion) || !type.Equals("birth-ack", StringComparison.Ordinal) || typeVersion != 1)
            {
                return;
            }

            if (!IsFromCurrentConnection(publish, type, out Guid? connectionNonce))
            {
                return;
            }

            if (connectionNonce != null && _connection.IsBirthAckTimedOut(connectionNonce.Value))
            {
                Trace.TraceWarning("Ignoring a birth ack that arrived after the connection client stopped waiting for it.");
                return;
            }

            BirthAck birthAck;
            try
            {
                birthAck = BirthAck.Parser.ParseFrom(publish.PayloadAsReadOnlySequence);
            }
            catch (InvalidProtocolBufferException e)
            {
                Trace.TraceWarning("Received a malformed birth ack. Ignoring its twin versions: {0}", e);
                return;
            }

            await handler.HandleBirthAckAsync(connectionNonce, birthAck.DesiredVersion, birthAck.ReportedVersion).ConfigureAwait(false);
        }

        private async Task HandleReceivedGetResponseAsync(ITwinTransportHandler handler, MqttPublish publish)
        {
            if (!GuidExtensions.TryParseBytes(publish.CorrelationData, out Guid? getId))
            {
                Trace.TraceWarning("Received a get twin response with missing or malformed correlation data. Ignoring it.");
                return;
            }

            TwinGetResponse twinGetResponse;
            try
            {
                twinGetResponse = TwinGetResponse.Parser.ParseFrom(publish.PayloadAsReadOnlySequence);
            }
            catch (InvalidProtocolBufferException e)
            {
                Trace.TraceWarning("Received a malformed get twin response: {0}", e);
                return;
            }

            TwinResponse response = new()
            {
                Status = TwinResponseStatus.Success,
                ParseSnapshot = () => new TwinSnapshot(
                    twinGetResponse.HasDesiredPayload ? ParseSectionPayload(twinGetResponse.DesiredPayload) : null,
                    twinGetResponse.DesiredVersion,
                    twinGetResponse.HasReportedPayload ? ParseSectionPayload(twinGetResponse.ReportedPayload) : null,
                    twinGetResponse.ReportedVersion),
            };

            await handler.HandleResponseAsync(_connection.ConnectionNonce, getId.Value, response).ConfigureAwait(false);
        }

        private async Task HandleReceivedReportedPatchResponseAsync(ITwinTransportHandler handler, MqttPublish publish)
        {
            if (!GuidExtensions.TryParseBytes(publish.CorrelationData, out Guid? patchId))
            {
                Trace.TraceWarning("Dropping a reported properties patch response without a valid correlation ID.");
                return;
            }

            ReportedPatchResponse reportedPatchResponse;
            try
            {
                reportedPatchResponse = ReportedPatchResponse.Parser.ParseFrom(publish.PayloadAsReadOnlySequence);
            }
            catch (InvalidProtocolBufferException e)
            {
                Trace.TraceWarning("Received a malformed reported properties patch response: {0}", e);
                return;
            }

            TwinResponse response = new()
            {
                Status = TwinResponseStatus.Success,
                PatchResponse = reportedPatchResponse,
            };

            await handler.HandleResponseAsync(_connection.ConnectionNonce, patchId.Value, response).ConfigureAwait(false);
        }

        private async Task HandleReceivedDesiredPatchAsync(ITwinTransportHandler handler, MqttPublish publish, string type)
        {
            if (!IsFromCurrentConnection(publish, type, out Guid? connectionNonce))
            {
                return;
            }

            ReceivedDesiredPatch patch;
            try
            {
                DesiredPatch desiredPatch = DesiredPatch.Parser.ParseFrom(publish.PayloadAsReadOnlySequence);
                patch = new ReceivedDesiredPatch(desiredPatch.Version, desiredPatch.HasPayload ? ParseSectionPayload(desiredPatch.Payload) : null);
            }
            catch (Exception e) when (e is InvalidProtocolBufferException or JsonException or InvalidOperationException)
            {
                Trace.TraceWarning("Dropping a malformed desired properties patch: {0}", e);
                return;
            }

            await handler.HandleDesiredPatchAsync(connectionNonce, patch).ConfigureAwait(false);
        }

        private async Task HandleReceivedTwinPushAsync(ITwinTransportHandler handler, MqttPublish publish, string type)
        {
            if (!IsFromCurrentConnection(publish, type, out Guid? connectionNonce))
            {
                return;
            }

            TwinPushSection? desired = null;
            TwinPushSection? reported = null;
            try
            {
                TwinPush twinPush = TwinPush.Parser.ParseFrom(publish.PayloadAsReadOnlySequence);
                if (twinPush.Desired != null)
                {
                    desired = new()
                    {
                        Properties = ParseSectionPayload(twinPush.Desired.Payload),
                        PropertiesVersion = twinPush.Desired.Version,
                    };
                }

                if (twinPush.Reported != null)
                {
                    reported = new()
                    {
                        Properties = ParseSectionPayload(twinPush.Reported.Payload),
                        PropertiesVersion = twinPush.Reported.Version,
                    };
                }
            }
            catch (Exception e) when (e is InvalidProtocolBufferException or JsonException or InvalidOperationException)
            {
                Trace.TraceWarning("Dropping a malformed twin push: {0}", e);
                return;
            }

            await handler.HandleTwinPushAsync(connectionNonce, desired, reported).ConfigureAwait(false);
        }

        // Returns false if the publish is a connection scoped message (a twin push, desired patch or birth ack) for a different
        // connection than the current one.
        private bool IsFromCurrentConnection(MqttPublish publish, string messageType, out Guid? connectionNonce)
        {
            connectionNonce = _connection.ConnectionNonce;
            if (connectionNonce == null)
            {
                // The connection client does not expose its connection nonce, so this cannot be checked.
                return true;
            }

            if (GuidExtensions.TryParseBytes(publish.CorrelationData, out Guid? receivedNonce) && receivedNonce.Value == connectionNonce.Value)
            {
                return true;
            }

            Trace.TraceInformation("Dropping a received '{0}' twin message because it was not sent for this device's current connection.", messageType);
            return false;
        }

        private static JsonObject ParseSectionPayload(ByteString payload)
        {
            return payload.IsEmpty ? new JsonObject() : JsonObject.Parse(payload.Span)!.AsObject();
        }
    }
}
