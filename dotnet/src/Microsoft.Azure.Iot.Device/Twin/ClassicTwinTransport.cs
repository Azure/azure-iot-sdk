// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Models.Twin;
using Microsoft.Azure.Iot.Device.MQTTv5.Connection;
using Microsoft.Azure.Iot.Device.Mqtt;
using Microsoft.Azure.Iot.Device.Unified.Models;
using System.Collections.Specialized;
using System.Diagnostics;
using System.Globalization;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;
using System.Web;
using IUnifiedConnectionClient = Microsoft.Azure.Iot.Device.Unified.Connection.IConnectionClient;

namespace Microsoft.Azure.Iot.Device.Twin
{
    // The twin transport of IoT hubs that use MQTTv3. IoT hub neither acknowledges this device's presence nor pushes the twin,
    // and its twin requests can't be conditional, so the connection client gets the full twin as part of each connection's
    // device presence flow instead, which this transport offers to the engine in place of a birth ack and twin push (or the
    // engine gets the full twin itself, if the connection client couldn't).
    internal sealed class ClassicTwinTransport : ITwinTransport
    {
        internal const string ResponseTopic = "$iothub/twin/res/";
        internal const string DesiredPropertiesPatchTopic = "$iothub/twin/PATCH/properties/desired/";
        internal const string VersionKey = "$version";
        private const string TopicPrefix = "$iothub/twin/";
        private const string ResponseTopicPattern = @"\$iothub/twin/res/(\d+)/(\?.+)";
        private static readonly Regex s_responseTopicRegex = new(ResponseTopicPattern, RegexOptions.Compiled);

        // Each request carries its ID as the request ID ($rid), which IoT hub echoes in the topic of the response.
        internal const string GetTopicFormat = "$iothub/twin/GET/?$rid={0}";
        private const string ReportedPropertiesPatchTopicFormat = "$iothub/twin/PATCH/properties/reported/?$rid={0}";

        private const string RequestIdTopicKey = "$rid";

        private readonly IUnifiedConnectionClient _connection;

        private ITwinTransportHandler? _handler;

        // IoT hubs that use MQTTv3 don't tag messages with a connection nonce, so this transport makes up its own for each
        // connection, which tells the engine's state of one connection from that of the next.
        private Guid _connectionNonce = Guid.NewGuid();

        public ClassicTwinTransport(IUnifiedConnectionClient connection)
        {
            _connection = connection;
        }

        public bool AnnouncesTwinVersions => false;

        public TwinSnapshot? ConnectTwinSnapshot
        {
            get
            {
                Unified.Connection.ConnectTwinSnapshot? snapshot = _connection.LatestConnectTwinSnapshot;
                if (snapshot == null || snapshot.ConnectionId != _connection.HubConnectionId)
                {
                    // The connection client didn't get the twin on the current connection.
                    return null;
                }

                return new TwinSnapshot(snapshot.Desired, snapshot.DesiredVersion, snapshot.Reported, snapshot.ReportedVersion);
            }
        }

        public Guid? ConnectionNonce => _connectionNonce;

        public bool IsConnected => _connection.GetCurrentConnectionContext() != null && _connection.IsConnectionTargetSet;

        public bool IsReady => _connection.IsConnectionReady;

        public BirthAckVersions? LatestBirthAck => null;

        public bool IsBirthAckTimedOut(Guid connectionNonce) => false;

        public ulong InitialDesiredPropertiesVersion => _connection.InitialDesiredPropertiesVersion;

        public ulong InitialReportedPropertiesVersion => _connection.InitialReportedPropertiesVersion;

        public void ObserveDesiredPropertiesVersion(ulong version) => _connection.ObserveDesiredPropertiesVersion(version);

        public void ObserveReportedPropertiesVersion(ulong version) => _connection.ObserveReportedPropertiesVersion(version);

        public void ReplaceDesiredPropertiesVersion(ulong version) => _connection.ReplaceDesiredPropertiesVersion(version);

        public void ReplaceReportedPropertiesVersion(ulong version) => _connection.ReplaceReportedPropertiesVersion(version);

        // Called when the connection is lost. Returns the nonce of the connection that was lost.
        public Guid RenewConnectionNonce()
        {
            Guid lostConnectionNonce = _connectionNonce;
            _connectionNonce = Guid.NewGuid();
            return lostConnectionNonce;
        }

        public void Start(ITwinTransportHandler handler)
        {
            _handler = handler;
            _connection.PublishReceivedAsync += HandleReceivedMqttPublishAsync;
        }

        public void Detach()
        {
            _connection.PublishReceivedAsync -= HandleReceivedMqttPublishAsync;
        }

        // IoT hubs that use MQTTv3 always return the full twin.
        public async Task PublishGetAsync(Guid getId, bool getDesired, bool getReported, ulong ifNotMatchDesired, ulong ifNotMatchReported, CancellationToken cancellationToken)
        {
            MqttPublish publish = new()
            {
                Topic = string.Format(CultureInfo.InvariantCulture, GetTopicFormat, getId),
                QualityOfServiceLevel = MqttQualityOfServiceLevel.AtLeastOnce,
            };

            Trace.TraceInformation("Publishing 'GetTwin' request on topic " + publish.Topic);

            // Puback is checked for non-success cases under this layer, so no need to check it here as well
            await _connection.PublishAsync(publish, throwOnErrorReasonCode: true, cancellationToken).ConfigureAwait(false);
        }

        // IoT hubs that use MQTTv3 always apply a reported properties patch unconditionally.
        public async Task PublishReportedPatchAsync(Guid patchId, ulong ifMatch, byte[] payload, CancellationToken cancellationToken)
        {
            MqttPublish publish = new()
            {
                Topic = string.Format(CultureInfo.InvariantCulture, ReportedPropertiesPatchTopicFormat, patchId),
                QualityOfServiceLevel = MqttQualityOfServiceLevel.AtLeastOnce,
                Payload = payload,
            };

            Trace.TraceInformation("Publishing 'PatchReported' request on topic " + publish.Topic);
            await _connection.PublishAsync(publish, throwOnErrorReasonCode: true, cancellationToken).ConfigureAwait(false);
        }

        private async Task HandleReceivedMqttPublishAsync(MqttPublishReceivedEventArgs args)
        {
            ITwinTransportHandler? handler = _handler;
            string topic = args.Publish.Topic;
            if (handler == null || !topic.StartsWith(TopicPrefix, StringComparison.Ordinal))
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

            if (connectionContext.ConnectionProfile == IotHubConnectionType.Mqttv5)
            {
                // This transport only handles the twin messages of IoT hubs that use MQTTv3.
                return;
            }

            Guid connectionNonce = _connectionNonce;

            // Note that all twin response and desired property update messages are QoS 0, so no need to ack the MQTT message here
            try
            {
                if (topic.StartsWith(ResponseTopic, StringComparison.Ordinal))
                {
                    if (!TryParseResponseTopic(topic, out Guid requestId, out int status, out ulong version))
                    {
                        Trace.TraceWarning("Dropping a twin response with an unexpected topic: " + topic);
                        return;
                    }

                    Trace.TraceInformation("Received twin response message on topic " + topic);
                    byte[] payload = args.Publish.Payload;

                    // A response doesn't say which kind of request it is for, so it is offered as either.
                    TwinResponse response = new()
                    {
                        Status = status == 429 || status >= 500
                            ? TwinResponseStatus.Transient
                            : IsSuccessStatus(status) ? TwinResponseStatus.Success : TwinResponseStatus.Rejected,
                        StatusCode = status,
                        ParseSnapshot = () => ParseGetTwinResponsePayload(payload),
                        PatchResponse = new ReportedPatchResponse()
                        {
                            Result = MapReportedPatchStatus(status),
                            Version = version,
                        },
                    };

                    await handler.HandleResponseAsync(connectionNonce, requestId, response).ConfigureAwait(false);
                }
                else if (topic.StartsWith(DesiredPropertiesPatchTopic, StringComparison.Ordinal))
                {
                    byte[] payload = args.Publish.Payload;
                    JsonObject desiredProperties = payload.Length == 0 ? new JsonObject() : JsonNode.Parse(payload)!.AsObject();
                    ulong version = TakeVersion(desiredProperties);
                    if (version == 0)
                    {
                        version = ParseVersionFromTopic(topic);
                    }

                    if (version == 0)
                    {
                        Trace.TraceWarning("Dropping a desired property patch without a version.");
                        return;
                    }

                    await handler.HandleDesiredPatchAsync(connectionNonce, new ReceivedDesiredPatch(version, desiredProperties)).ConfigureAwait(false);
                }
            }
            catch (Exception e) when (e is JsonException or InvalidOperationException or FormatException)
            {
                Trace.TraceWarning("Dropping a malformed twin message received on topic {0}: {1}", topic, e);
            }
        }

        private static Result MapReportedPatchStatus(int status)
        {
            return status switch
            {
                >= 200 and < 300 => Result.Ok,
                400 => Result.PayloadInvalid,
                413 => Result.PayloadTooLarge,
                _ => Result.InternalError,
            };
        }

        // The payload looks like {"desired":{...,"$version":n},"reported":{...,"$version":m}}.
        internal static TwinSnapshot ParseGetTwinResponsePayload(byte[] payload)
        {
            JsonObject twin = payload.Length == 0 ? new JsonObject() : JsonNode.Parse(payload)!.AsObject();
            JsonObject desired = DetachSection(twin, "desired", out ulong desiredVersion);
            JsonObject reported = DetachSection(twin, "reported", out ulong reportedVersion);
            return new TwinSnapshot(desired, desiredVersion, reported, reportedVersion);
        }

        private static JsonObject DetachSection(JsonObject twin, string sectionName, out ulong version)
        {
            JsonObject section = twin.TryGetPropertyValue(sectionName, out JsonNode? node) && node is JsonObject sectionObject
                ? sectionObject
                : new JsonObject();
            twin.Remove(sectionName);

            // Remove the "$version" entry so that the twin object more closely mimics how it would in MQTTv5 scenario
            version = TakeVersion(section);
            return section;
        }

        // Removes the "$version" entry from the properties, and returns its value (or 0 if there isn't one).
        private static ulong TakeVersion(JsonObject properties)
        {
            ulong version = 0;
            if (properties.TryGetPropertyValue(VersionKey, out JsonNode? versionNode) && versionNode is JsonValue versionValue)
            {
                version = (ulong)versionValue;
            }

            properties.Remove(VersionKey);
            return version;
        }

        private static ulong ParseVersionFromTopic(string topic)
        {
            int queryStart = topic.IndexOf('?', StringComparison.Ordinal);
            if (queryStart < 0)
            {
                return 0;
            }

            NameValueCollection query = HttpUtility.ParseQueryString(topic.Substring(queryStart));
            return ulong.TryParse(query.Get(VersionKey), NumberStyles.None, CultureInfo.InvariantCulture, out ulong version) ? version : 0;
        }

        internal static bool IsSuccessStatus(int status)
        {
            return status >= 200 && status < 300;
        }

        internal static bool TryParseResponseTopic(string topicName, out Guid requestId, out int status, out ulong version)
        {
            requestId = Guid.Empty;
            status = 0;
            version = 0;

            // The topic here looks like
            // "$iothub/twin/res/204/?$rid=efc34c73-79ce-4054-9985-0cdf40a3c794&$version=2"
            // The regex matching splits it up into the status ("204") and the query string ("?$rid=...&$version=2"), which is
            // then parsed for key value pairs. Get twin responses carry the twin versions in the payload instead.
            Match match = s_responseTopicRegex.Match(topicName);
            if (!match.Success || !int.TryParse(match.Groups[1].Value, NumberStyles.None, CultureInfo.InvariantCulture, out status))
            {
                return false;
            }

            NameValueCollection queryStringKeyValuePairs = HttpUtility.ParseQueryString(match.Groups[2].Value);
            if (!Guid.TryParse(queryStringKeyValuePairs.Get(RequestIdTopicKey), out requestId))
            {
                return false;
            }

            _ = ulong.TryParse(queryStringKeyValuePairs.Get(VersionKey), NumberStyles.None, CultureInfo.InvariantCulture, out version);
            return true;
        }
    }
}
