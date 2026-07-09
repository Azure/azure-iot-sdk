using Google.Protobuf;
using Microsoft.Azure.Devices.Client.CertificateManagement;
using Microsoft.Azure.Devices.Client.Mqtt;
using Microsoft.Azure.Devices.Client.Serialization;
using Microsoft.Azure.Devices.Client.Twin.LegacyTwinObjects;
using System.Collections.Concurrent;
using System.Collections.Specialized;
using System.Diagnostics;
using System.Globalization;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;
using System.Web;

namespace Microsoft.Azure.Devices.Client.Twin
{
    public class TwinClient : IDisposable
    {
        private ConnectionClient _connection;

        // Response topics to subscribe to
        internal const string ClassicTwinResponseTopic = "$iothub/twin/res/";
        internal const string ClassicTwinDesiredPropertiesPatchTopic = "$iothub/twin/PATCH/properties/desired/";
        private readonly Regex _twinResponseTopicRegex = new(ClassicTwinResponseTopicPattern, RegexOptions.Compiled);
        private const string ClassicTwinResponseTopicPattern = @"\$iothub/twin/res/(\d+)/(\?.+)";

        // Topics to publish to
        private const string ClassicTwinGetTopicFormat = "$iothub/twin/GET/?$rid={0}";
        private const string ClassicTwinReportedPropertiesPatchTopicFormat = "$iothub/twin/PATCH/properties/reported/?$rid={0}";

        private const string TwinDesiredPropertiesPatchTopic = "$iothub/twin/PATCH/properties/desired/";

        private const string AzureEventGridOutgoingTwinPublishTopicFormat = "ih/{deviceId}/srv/twin";
        private const string AzureEventGridIncomingTwinPublishTopicFormat = "ih/{deviceId}/dev/twin";

        private const string RequestIdTopicKey = "$rid";
        internal const string VersionKey = "$version";

        private readonly ConcurrentDictionary<Guid, PendingGetTwinRequest> _pendingGetTwinOperations = new();
        private readonly ConcurrentDictionary<Guid, PendingReportedPropertiesUpdateRequest> _pendingReportedPropertyUpdateOperations = new();

        /// <summary>
        /// An event that executes whenever this client receives a desired properties update from IoT hub.
        /// </summary>
        public event Action<DesiredPatchReceivedEventArgs>? DesiredPatchReceived;

        /// <summary>
        /// An event that executes whenever this client receives a Twin push message from IoT hub.
        /// </summary>
        /// <remarks>
        /// This feature is only supported by IoT hubs using Azure Event Grid. Older IoT hubs will never encounter this event.
        /// </remarks>
        public event Action<TwinPushReceivedEventArgs>? TwinPushReceived;

        public TwinClient(ConnectionClient connection)
        {
            _connection = connection;
            _connection.ApplicationMessageReceivedAsync += HandleReceivedAzureEventGridMqttPublish;
            _connection.ApplicationMessageReceivedAsync += HandleReceivedClassicMqttPublish;
        }

        /// <summary>
        /// Get the full twin, or some conditional set of the twin properties.
        /// </summary>
        /// <param name="getReported">Request the service to include the reported properties in the returned twin.</param>
        /// <param name="getDesired">Request the service to include the desired properties in the returned twin.</param>
        /// <param name="ifNotMatchReported">
        /// If provided, and the version matches or exceeds the version of the reported properties held by the service, then the 
        /// service will not include the reported properties in the returned twin.
        /// </param>
        /// <param name="ifNotMatchDesired">
        /// If provided, and the version matches or exceeds the version of the desired properties held by the service, then the 
        /// service will not include the desired properties in the returned twin.
        /// </param>
        /// <param name="cancellationToken">The cancellation token.</param>
        /// <returns>The returned twin.</returns>
        /// <remarks>
        /// Only IoT hubs that use Azure Event Grid will actually respond to the getReported/getDesired/ifNotMatch flags as this feature is unsupported in older IoT hubs. 
        /// However, this SDK will parse the twin that the service returns to filter out unrequested sections to mimic the behavior of Azure Event Grid IoT hubs.
        /// </remarks>
        public async Task<TwinGetResponseWrapper> GetTwinAsync(bool getReported = true, bool getDesired = true, ulong ifNotMatchReported = 0, ulong ifNotMatchDesired = 0,  CancellationToken cancellationToken = default)
        {
            Guid requestId = Guid.NewGuid();

            // Note the request as "in progress" before actually sending it so that no matter how quickly the service
            // responds, this layer can correlate the request.
            var pendingGetTwinRequest = new PendingGetTwinRequest()
            {
                GetDesired = getDesired,
                GetReported = getReported,
                IfNotMatchDesired = ifNotMatchDesired,
                IfNotMatchReported = ifNotMatchReported,
            };
            _pendingGetTwinOperations[requestId] = pendingGetTwinRequest;

            MqttPublish publish;
            if (_connection.CurrentConnectionContext!.IsAzureEventGrid)
            {
                publish = new MqttPublish()
                {
                    Topic = string.Format(AzureEventGridOutgoingTwinPublishTopicFormat, _connection.CurrentConnectionContext.DeviceId),
                    QualityOfServiceLevel = MqttQualityOfServiceLevel.AtMostOnce,
                    CorrelationData = requestId.ToByteArray(),
                    PayloadAsByteArray = new TwinGet()
                    {
                        Sections = Sections.Both,
                        IfNotMatchDesired = ifNotMatchDesired,
                        IfNotMatchReported = ifNotMatchReported,
                    }.ToByteArray(),
                };

                publish.UserProperties.Add(new("type", Encoding.UTF8.GetBytes("get:1")));
            }
            else
            {
                publish = new MqttPublish()
                {
                    Topic = string.Format(ClassicTwinGetTopicFormat, requestId.ToString()),
                    QualityOfServiceLevel = MqttQualityOfServiceLevel.AtLeastOnce,
                };
            }

            MqttPublishAck puback = await _connection.PublishAsync(publish, cancellationToken);

            if (puback.ReasonCode != Mqtt.MqttClientPublishReasonCode.Success)
            {
                throw new Exception("TODO");
            }

            // Wait until IoT hub sends a message to this client with the response to this patch twin request.
            var getTwinResponse = await pendingGetTwinRequest.TwinResponseTask.Task.WaitAsync(cancellationToken).ConfigureAwait(false);

            _pendingGetTwinOperations.Remove(requestId, out _); //TODO in finally block. Happy path has already removed it at this point, though.

            return getTwinResponse;
        }

        /// <summary>
        /// Update this device's reported properties.
        /// </summary>
        /// <param name="patch">The patch of the reported properties to send to IoT hub</param>
        /// <param name="cancellationToken">The cancellation token.</param>
        /// <returns>Whether IoT hub accepted this patch.</returns>
        public async Task<ReportedPatchResponse> UpdateReportedPropertiesAsync(ReportedPatchRequest patch, CancellationToken cancellationToken = default)
        {
            Guid requestId = Guid.NewGuid();
            
            // Note the request as "in progress" before actually sending it so that no matter how quickly the service
            // responds, this layer can correlate the request.
            var pendingReportedPropertiesUpdateRequest = new PendingReportedPropertiesUpdateRequest();
            _pendingReportedPropertyUpdateOperations[requestId] = pendingReportedPropertiesUpdateRequest;

            MqttPublish publish;
            if (_connection.CurrentConnectionContext!.IsAzureEventGrid)
            {
                publish = new MqttPublish()
                {
                    Topic = string.Format(AzureEventGridOutgoingTwinPublishTopicFormat, _connection.CurrentConnectionContext.DeviceId),
                    QualityOfServiceLevel = MqttQualityOfServiceLevel.AtMostOnce,
                    CorrelationData = requestId.ToByteArray(),
                    PayloadAsByteArray = new ReportedPatch()
                    {
                        IfMatch = patch.IfMatch,
                        Payload = ByteString.CopyFromUtf8(JsonSerializer.Serialize(patch.ReportedProperties))
                    }.ToByteArray(),
                };

                publish.UserProperties.Add(new("type", Encoding.UTF8.GetBytes("reported-patch:1")));
            }
            else
            {
                string topic = string.Format(CultureInfo.InvariantCulture, ClassicTwinReportedPropertiesPatchTopicFormat, requestId);

                publish = new MqttPublish()
                {
                    Topic = string.Format(ClassicTwinGetTopicFormat, requestId),
                    QualityOfServiceLevel = MqttQualityOfServiceLevel.AtLeastOnce,
                    PayloadAsByteArray = new ReportedPatch()
                    {
                        IfMatch = patch.IfMatch,
                        Payload = ByteString.CopyFromUtf8(JsonSerializer.Serialize(patch.ReportedProperties))
                    }.ToByteArray(), // TODO no idea if this AEG payload works for the classic reported properties patch payload
                };
            }

            MqttPublishAck puback = await _connection.PublishAsync(publish, cancellationToken);

            if (puback.ReasonCode != Mqtt.MqttClientPublishReasonCode.Success)
            {
                throw new Exception("TODO");
            }

            // Wait until IoT hub sends a message to this client with the response to this patch twin request.
            var updateReportedPropertiesResponse = await pendingReportedPropertiesUpdateRequest.ReportedPropertyUpdateResponse.Task.WaitAsync(cancellationToken).ConfigureAwait(false);

            return updateReportedPropertiesResponse;
        }


        private async Task HandleReceivedAzureEventGridMqttPublish(MqttPublishReceivedEventArgs args)
        { 
            if (!_connection.CurrentConnectionContext!.IsAzureEventGrid)
            {
                // The other handler covers this scenario
                return;
            }

            if (!args.Publish.Topic.Equals(string.Format(AzureEventGridIncomingTwinPublishTopicFormat, _connection.CurrentConnectionContext.DeviceId)))
            {
                // This message wasn't a twin message, so ignore it
                return;
            }

            if (!args.Publish.UserProperties.TryGetType(out string? type, out int? typeVersion))
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


            if (type.Equals("get-response")
                && GuidExtensions.TryParseBytes(args.Publish.CorrelationData, out Guid? getResponseCorrelationData)
                && _pendingGetTwinOperations.TryRemove(getResponseCorrelationData.Value, out PendingGetTwinRequest? pendingGetTwinRequest))
            {
                TwinGetResponse twinGetResponse = TwinGetResponse.Parser.ParseFrom(args.Publish.PayloadAsReadOnlySequence);

                pendingGetTwinRequest.TwinResponseTask.TrySetResult(new()
                {
                    DesiredProperties = JsonObject.Parse(twinGetResponse.DesiredPayload.Span)!.AsObject(),
                    ReportedProperties = JsonObject.Parse(twinGetResponse.ReportedPayload.Span)!.AsObject(),
                    DesiredPropertiesVersion = twinGetResponse.DesiredVersion,
                    ReportedPropertiesVersion = twinGetResponse.ReportedVersion,
                });
            }
            else if (type.Equals("reported-patch-response")
                && GuidExtensions.TryParseBytes(args.Publish.CorrelationData, out Guid? patchResponseCorrelationData)
                && _pendingReportedPropertyUpdateOperations.TryRemove(patchResponseCorrelationData.Value, out PendingReportedPropertiesUpdateRequest? pendingReportedPropertiesUpdateRequest))
            {
                ReportedPatchResponse reportedPatchResponse = ReportedPatchResponse.Parser.ParseFrom(args.Publish.PayloadAsReadOnlySequence);
                pendingReportedPropertiesUpdateRequest.ReportedPropertyUpdateResponse.TrySetResult(reportedPatchResponse);
            }
            else if (type.Equals("desired-patch") && typeVersion == 1)
            {
                DesiredPatch receivedDesiredPatch = DesiredPatch.Parser.ParseFrom(args.Publish.PayloadAsReadOnlySequence);

                var desiredPatchArgs = new DesiredPatchReceivedEventArgs()
                {
                    DesiredProperties = JsonObject.Parse(receivedDesiredPatch.Payload.Span)!.AsObject(),
                    DesiredPropertiesVersion = receivedDesiredPatch.Version,
                };

                DesiredPatchReceived?.Invoke(desiredPatchArgs);
            }
            else if (type.Equals("twin-push"))
            {
                TwinPush receivedTwinPush = TwinPush.Parser.ParseFrom(args.Publish.PayloadAsReadOnlySequence);
                var twinPushArgs = new TwinPushReceivedEventArgs();
                if (receivedTwinPush.Desired != null)
                {
                    twinPushArgs.Desired = new()
                    {
                        Properties = JsonObject.Parse(receivedTwinPush.Desired.Payload.Span)!.AsObject(),
                        PropertiesVersion = receivedTwinPush.Desired.Version
                    };
                }

                if (receivedTwinPush.Reported != null)
                {
                    twinPushArgs.Desired = new()
                    {
                        Properties = JsonObject.Parse(receivedTwinPush.Reported.Payload.Span)!.AsObject(),
                        PropertiesVersion = receivedTwinPush.Reported.Version
                    };
                }
                TwinPushReceived?.Invoke(twinPushArgs);
            }
        }

        private async Task HandleReceivedClassicMqttPublish(MqttPublishReceivedEventArgs args)
        {
            if (_connection.CurrentConnectionContext!.IsAzureEventGrid)
            {
                // The other handler covers this scenario
                return;
            }

            // Note that all twin response messages are QoS 0, so no need to ack the MQTT message here
            if (args.Publish.Topic.StartsWith(ClassicTwinResponseTopic, StringComparison.InvariantCulture))
            {
                if (ParseResponseTopic(args.Publish.Topic, out string receivedRequestId, out int status, out long version))
                {
                    byte[] payloadBytes = args.Publish.PayloadAsByteArray ?? Array.Empty<byte>();

                    Guid requestIdGuid = new Guid(receivedRequestId);
                    if (_pendingGetTwinOperations.TryRemove(requestIdGuid, out PendingGetTwinRequest? getTwinOperation))
                    {
                        var clientTwinProperties = JsonNode.Parse(payloadBytes)!.AsObject();

                        var desiredVersion = clientTwinProperties["desired"]![VersionKey];
                        ulong desiredPropertiesVersion = (ulong) desiredVersion!.AsValue();

                        // Remove the "$version" entry so that the twin object more closely mimics how it would in AEG scenario
                        clientTwinProperties["desired"]!.AsObject().Remove(VersionKey);

                        var reportedVersion = clientTwinProperties["reported"]![VersionKey];
                        ulong reportedPropertiesVersion = (ulong)reportedVersion!.AsValue();

                        // Remove the "$version" entry so that the twin object more closely mimics how it would in AEG scenario
                        clientTwinProperties["reported"]!.AsObject().Remove(VersionKey);

                        var twinGetResponse = new TwinGetResponseWrapper()
                        {
                            DesiredPropertiesVersion = desiredPropertiesVersion,
                            ReportedPropertiesVersion = reportedPropertiesVersion,
                        };

                        // These user-supplied configurations are handled by the service if it is an AEG broker, but classic hub does not actually support them. The below
                        // will intentionally remove the desired/reported properties in such a way to mimic that service behavior when connected to a classic hub.
                        if (getTwinOperation.GetDesired && (getTwinOperation.IfNotMatchDesired < desiredPropertiesVersion))
                        {
                            twinGetResponse.DesiredProperties = clientTwinProperties["desired"]!.AsObject();
                        }

                        if (getTwinOperation.GetReported && (getTwinOperation.IfNotMatchReported < reportedPropertiesVersion))
                        {
                            twinGetResponse.ReportedProperties = clientTwinProperties["reported"]!.AsObject();
                        }

                        getTwinOperation.TwinResponseTask.TrySetResult(twinGetResponse);
                    }
                    else if (_pendingReportedPropertyUpdateOperations.TryRemove(requestIdGuid, out PendingReportedPropertiesUpdateRequest? pendingReportedPropertiesUpdateRequest))
                    {
                        ReportedPropertyUpdateResponse? response = JsonSerializer.Deserialize<ReportedPropertyUpdateResponse>(payloadBytes, JsonSerializationSettings.Options);

                        pendingReportedPropertiesUpdateRequest.ReportedPropertyUpdateResponse.TrySetResult(new ReportedPatchResponse()
                        {
                            Result = Result.Ok, // TODO mapping possible classic integer error codes to this new error enum
                            Version = response!.Version
                        });
                    }
                }
            }
            else if (args.Publish.Topic.StartsWith(TwinDesiredPropertiesPatchTopic, StringComparison.InvariantCulture))
            {
                // Note that all desired property update messages are QoS 0, so no need to ack the MQTT message here
                if (DesiredPatchReceived != null)
                {
                    var desiredPropertiesWithVersion = JsonNode.Parse(args.Publish.PayloadAsByteArray)!.AsObject();
                    ulong desiredPropertiesVersion = (ulong)desiredPropertiesWithVersion[VersionKey]!;
                    desiredPropertiesWithVersion.Remove(VersionKey);

                    var desiredPropertyPatch = new DesiredPatchReceivedEventArgs()
                    {
                        DesiredProperties = desiredPropertiesWithVersion,
                        DesiredPropertiesVersion = desiredPropertiesVersion
                    };
                    DesiredPatchReceived.Invoke(desiredPropertyPatch);
                }
            }
        }

        private bool ParseResponseTopic(string topicName, out string rid, out int status, out long version)
        {
            rid = "";
            status = 500;
            version = 0;

            // The topic here looks like
            // "$iothub/twin/res/204/?$rid=efc34c73-79ce-4054-9985-0cdf40a3c794&$version=2"
            // The regex matching splits it up into
            // "$iothub/twin" "res/204" "$rid=efc34c73-79ce-4054-9985-0cdf40a3c794&$version=2"
            // Then the third group is parsed for key value pairs such as "$version=2"
            Match match = _twinResponseTopicRegex.Match(topicName);
            if (!match.Success)
            {
                return false;
            }

            // match.Groups[1] evaluates to the key-value pair that looks like "res/204"
            status = Convert.ToInt32(match.Groups[1].Value, CultureInfo.InvariantCulture);

            // match.Groups[1] evaluates to the query string key-value pair parameters
            NameValueCollection queryStringKeyValuePairs = HttpUtility.ParseQueryString(match.Groups[2].Value);
            rid = queryStringKeyValuePairs.Get(RequestIdTopicKey)!;

            if (status == 204)
            {
                // This query string key-value pair is only expected in a successful patch twin response message.
                // Get twin requests will contain the twin version in the payload instead.
                _ = long.TryParse(queryStringKeyValuePairs.Get(VersionKey), out version);
            }

            return true;
        }

        public void Dispose()
        {
            _connection.ApplicationMessageReceivedAsync -= HandleReceivedAzureEventGridMqttPublish;
            _connection.ApplicationMessageReceivedAsync -= HandleReceivedClassicMqttPublish;
        }
    }
}
