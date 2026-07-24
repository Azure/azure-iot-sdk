using Google.Protobuf;
using Microsoft.Azure.Devices.Client.Mqtt;
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
    /// <summary>
    /// The feature client for interacting with a device's twin properties.
    /// </summary>
    public class TwinClient : IDisposable
    {
        private IConnectionClient _connection;

        // Response topics to subscribe to
        internal const string ClassicTwinResponseTopic = "$iothub/twin/res/";
        internal const string ClassicTwinDesiredPropertiesPatchTopic = "$iothub/twin/PATCH/properties/desired/";
        private readonly Regex _twinResponseTopicRegex = new(ClassicTwinResponseTopicPattern, RegexOptions.Compiled);
        private const string ClassicTwinResponseTopicPattern = @"\$iothub/twin/res/(\d+)/(\?.+)";

        // Topics to publish to
        private const string ClassicTwinGetTopicFormat = "$iothub/twin/GET/?$rid={0}";
        private const string ClassicTwinReportedPropertiesPatchTopicFormat = "$iothub/twin/PATCH/properties/reported/?$rid={0}";

        private const string TwinDesiredPropertiesPatchTopic = "$iothub/twin/PATCH/properties/desired/";

        private const string AzureEventGridOutgoingTwinPublishTopicFormat = "ih/{0}/srv/twin";
        private const string AzureEventGridIncomingTwinPublishTopicFormat = "ih/{0}/dev/twin";

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

        /// <summary>
        /// Construct a new <see cref="TwinClient"/> instance.
        /// </summary>
        /// <param name="connection">The connection client this feature client will use.</param>
        /// <remarks>
        /// The provided connection client does not need to be connected before this constructor is called. However, the provided connection client must be connected prior
        /// to using this feature client to send/receive any twin messages.
        /// </remarks>
        /// <example>
        /// The recommended order to instantiate feature clients and the connection client is as follows:
        /// <code>
        /// // Construct all the clients your device will use
        /// ConnectionClient connectionClient = new();
        /// TwinClient twinClient = new(connectionClient);
        /// 
        /// //Set all handlers 
        /// twinClient.DesiredPatchReceived += SomeDesiredPatchHandlingMethod;
        /// 
        /// // Open the connection (and start receiving desired patches)
        /// await connectionClient.ProvisionAndConnectAsync();
        /// </code>
        /// </example>
        public TwinClient(IConnectionClient connection)
        {
            _connection = connection;
            _connection.ApplicationMessageReceivedAsync += HandleReceivedAzureEventGridHubMqttPublish;
            _connection.ApplicationMessageReceivedAsync += HandleReceivedClassicHubMqttPublish;
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
        /// <exception cref="PublishRejectedException">Thrown if this get twin request is rejected by IoT Hub for any reason.</exception>
        public async Task<Twin> GetTwinAsync(bool getReported = true, bool getDesired = true, ulong ifNotMatchReported = 0, ulong ifNotMatchDesired = 0,  CancellationToken cancellationToken = default)
        {
            if (_connection.GetCurrentConnectionContext() == null)
            {
                throw new NotSupportedException("Must be connected before calling this method.");
            }

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
            if (_connection.GetCurrentConnectionContext()!.IsAzureEventGrid)
            {
                publish = new MqttPublish()
                {
                    Topic = string.Format(AzureEventGridOutgoingTwinPublishTopicFormat, _connection.GetCurrentConnectionContext().DeviceId),
                    QualityOfServiceLevel = MqttQualityOfServiceLevel.AtMostOnce,
                    CorrelationData = requestId.ToByteArray(bigEndian: true),
                    Payload = new TwinGet()
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

            Trace.TraceInformation("Publishing 'GetTwin' request on topic " + publish.Topic);
            MqttPublishAck puback = await _connection.PublishAsync(publish, cancellationToken);

            PublishRejectedException.ThrowIfUnsuccessfulPuback(puback, "Failed to request the twin because the MQTT broker rejected the request.");

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
        /// <exception cref="PublishRejectedException">Thrown if this reported property update message is rejected by IoT Hub for any reason.</exception>
        public async Task<ReportedPatchResponse> UpdateReportedPropertiesAsync(ReportedPatchRequest patch, CancellationToken cancellationToken = default)
        {
            if (_connection.GetCurrentConnectionContext() == null)
            {
                throw new NotSupportedException("Must be connected before calling this method.");
            }

            Guid requestId = Guid.NewGuid();
            
            // Note the request as "in progress" before actually sending it so that no matter how quickly the service
            // responds, this layer can correlate the request.
            var pendingReportedPropertiesUpdateRequest = new PendingReportedPropertiesUpdateRequest();
            _pendingReportedPropertyUpdateOperations[requestId] = pendingReportedPropertiesUpdateRequest;

            MqttPublish publish;
            if (_connection.GetCurrentConnectionContext()!.IsAzureEventGrid)
            {
                publish = new MqttPublish()
                {
                    Topic = string.Format(AzureEventGridOutgoingTwinPublishTopicFormat, _connection.GetCurrentConnectionContext().DeviceId),
                    QualityOfServiceLevel = MqttQualityOfServiceLevel.AtMostOnce,
                    CorrelationData = requestId.ToByteArray(bigEndian: true),
                    Payload = new ReportedPatch()
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
                string payload = JsonSerializer.Serialize(patch.ReportedProperties);

                publish = new MqttPublish()
                {
                    Topic = topic,
                    QualityOfServiceLevel = MqttQualityOfServiceLevel.AtLeastOnce,
                    Payload = JsonSerializer.SerializeToUtf8Bytes(patch.ReportedProperties), 
                };
            }

            Trace.TraceInformation("Publishing 'PatchReported' request on topic " + publish.Topic);
            MqttPublishAck puback = await _connection.PublishAsync(publish, cancellationToken);

            PublishRejectedException.ThrowIfUnsuccessfulPuback(puback, "Failed to update the reported properties because the MQTT broker rejected the request.");

            // Wait until IoT hub sends a message to this client with the response to this patch twin request.
            var updateReportedPropertiesResponse = await pendingReportedPropertiesUpdateRequest.ReportedPropertyUpdateResponse.Task.WaitAsync(cancellationToken).ConfigureAwait(false);

            return updateReportedPropertiesResponse;
        }

        private async Task HandleReceivedAzureEventGridHubMqttPublish(MqttPublishReceivedEventArgs args)
        { 
            if (!_connection.GetCurrentConnectionContext()!.IsAzureEventGrid)
            {
                // The other handler covers this scenario
                return;
            }

            if (!args.Publish.Topic.Equals(string.Format(AzureEventGridIncomingTwinPublishTopicFormat, _connection.GetCurrentConnectionContext().DeviceId)))
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
                    Desired = JsonObject.Parse(twinGetResponse.DesiredPayload.Span)!.AsObject(),
                    Reported = JsonObject.Parse(twinGetResponse.ReportedPayload.Span)!.AsObject(),
                    DesiredVersion = twinGetResponse.DesiredVersion,
                    ReportedVersion = twinGetResponse.ReportedVersion,
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

        private async Task HandleReceivedClassicHubMqttPublish(MqttPublishReceivedEventArgs args)
        {
            if (_connection.GetCurrentConnectionContext()!.IsAzureEventGrid)
            {
                // The other handler covers this scenario
                return;
            }

            // Note that all twin response messages are QoS 0, so no need to ack the MQTT message here
            if (args.Publish.Topic.StartsWith(ClassicTwinResponseTopic, StringComparison.InvariantCulture))
            {
                if (ParseResponseTopic(args.Publish.Topic, out string receivedRequestId, out int status, out ulong version))
                {
                    byte[] payloadBytes = args.Publish.Payload ?? Array.Empty<byte>();

                    Guid requestIdGuid = new Guid(receivedRequestId);

                    Trace.TraceInformation("Received twin response message on topic " + args.Publish.Topic);
                    if (_pendingGetTwinOperations.TryRemove(requestIdGuid, out PendingGetTwinRequest? getTwinOperation))
                    {
                        var clientTwinProperties = JsonNode.Parse(payloadBytes)!.AsObject();

                        var desiredVersion = clientTwinProperties["desired"]![VersionKey];
                        ulong desiredPropertiesVersion = (ulong)desiredVersion!.AsValue();

                        // Remove the "$version" entry so that the twin object more closely mimics how it would in AEG scenario
                        clientTwinProperties["desired"]!.AsObject().Remove(VersionKey);

                        var reportedVersion = clientTwinProperties["reported"]![VersionKey];
                        ulong reportedPropertiesVersion = (ulong)reportedVersion!.AsValue();

                        // Remove the "$version" entry so that the twin object more closely mimics how it would in AEG scenario
                        clientTwinProperties["reported"]!.AsObject().Remove(VersionKey);

                        var twinGetResponse = new Twin()
                        {
                            DesiredVersion = desiredPropertiesVersion,
                            ReportedVersion = reportedPropertiesVersion,
                        };

                        // These user-supplied configurations are handled by the service if it is an AEG broker, but classic hub does not actually support them. The below
                        // will intentionally remove the desired/reported properties in such a way to mimic that service behavior when connected to a classic hub.
                        if (getTwinOperation.GetDesired && (getTwinOperation.IfNotMatchDesired < desiredPropertiesVersion))
                        {
                            twinGetResponse.Desired = clientTwinProperties["desired"]!.AsObject();
                        }

                        if (getTwinOperation.GetReported && (getTwinOperation.IfNotMatchReported < reportedPropertiesVersion))
                        {
                            twinGetResponse.Reported = clientTwinProperties["reported"]!.AsObject();
                        }

                        getTwinOperation.TwinResponseTask.TrySetResult(twinGetResponse);
                    }
                    else if (_pendingReportedPropertyUpdateOperations.TryRemove(requestIdGuid, out PendingReportedPropertiesUpdateRequest? pendingReportedPropertiesUpdateRequest))
                    {
                        pendingReportedPropertiesUpdateRequest.ReportedPropertyUpdateResponse.TrySetResult(new ReportedPatchResponse()
                        {
                            Result = Result.Ok, // TODO mapping possible classic integer error codes to this new error enum
                            Version = version,
                        });
                    }
                }
            }
            else if (args.Publish.Topic.StartsWith(TwinDesiredPropertiesPatchTopic, StringComparison.InvariantCulture))
            {
                // Note that all desired property update messages are QoS 0, so no need to ack the MQTT message here
                if (DesiredPatchReceived != null)
                {
                    var desiredPropertiesWithVersion = JsonNode.Parse(args.Publish.Payload)!.AsObject();
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

        private bool ParseResponseTopic(string topicName, out string rid, out int status, out ulong version)
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
                _ = ulong.TryParse(queryStringKeyValuePairs.Get(VersionKey), out version);
            }

            return true;
        }

        public void Dispose()
        {
            _connection.ApplicationMessageReceivedAsync -= HandleReceivedAzureEventGridHubMqttPublish;
            _connection.ApplicationMessageReceivedAsync -= HandleReceivedClassicHubMqttPublish;
        }
    }
}
