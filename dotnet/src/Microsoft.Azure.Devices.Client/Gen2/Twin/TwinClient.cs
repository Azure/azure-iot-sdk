using Google.Protobuf;
using Microsoft.Azure.Devices.Client.Exceptions;
using Microsoft.Azure.Devices.Client.Gen2.Connection;
using Microsoft.Azure.Devices.Client.Models;
using Microsoft.Azure.Devices.Client.Models.Twin;
using Microsoft.Azure.Devices.Client.Mqtt;
using System.Collections.Concurrent;
using System.Diagnostics;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace Microsoft.Azure.Devices.Client.Gen2.Twin
{
    /// <summary>
    /// The feature client for interacting with a device's twin properties.
    /// </summary>
    public class TwinClient : IDisposable
    {
        private bool _isDisposed = false;

        private const string ProtobufContentType = "application/protobuf";

        private IConnectionClient _connection;

        private const string AzureEventGridOutgoingTwinPublishTopicFormat = "ih/{0}/srv/twin";
        private const string AzureEventGridIncomingTwinPublishTopicFormat = "ih/{0}/dev/twin";

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
            _connection.PublishReceivedAsync += HandleReceivedMqttPublish;
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
        public async Task<DeviceTwin> GetTwinAsync(bool getReported = true, bool getDesired = true, ulong ifNotMatchReported = 0, ulong ifNotMatchDesired = 0,  CancellationToken cancellationToken = default)
        {
            ObjectDisposedException.ThrowIf(_isDisposed, this);

            var currentConnectionContext = EnsureCorrectConnectionContext();

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

            MqttPublish publish = new MqttPublish()
            {
                Topic = string.Format(AzureEventGridOutgoingTwinPublishTopicFormat, currentConnectionContext.DeviceId),
                QualityOfServiceLevel = MqttQualityOfServiceLevel.AtMostOnce,
                CorrelationData = requestId.ToByteArray(bigEndian: true),
                Payload = new TwinGet()
                {
                    Sections = Sections.Both,
                    IfNotMatchDesired = ifNotMatchDesired,
                    IfNotMatchReported = ifNotMatchReported,
                }.ToByteArray(),
                ContentType = ProtobufContentType,
            };

            publish.UserProperties.Add(new("type", Encoding.UTF8.GetBytes("get:1")));

            try
            {
                Trace.TraceInformation("Publishing 'GetTwin' request on topic " + publish.Topic);
                MqttPublishAck puback = await _connection.PublishAsync(publish, cancellationToken);

                PublishRejectedException.ThrowIfUnsuccessfulPuback(puback, "Failed to request the twin because the MQTT broker rejected the request.");

                // Wait until IoT hub sends a message to this client with the response to this patch twin request.
                var getTwinResponse = await pendingGetTwinRequest.TwinResponseTask.Task.WaitAsync(cancellationToken).ConfigureAwait(false);
                return getTwinResponse;
            }
            finally
            {
                // This may already be removed during happy path, but this removal covers scenarios where the response is never received
                _ = _pendingGetTwinOperations.Remove(requestId, out _);
            }
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
            ObjectDisposedException.ThrowIf(_isDisposed, this);

            var currentConnectionContext = EnsureCorrectConnectionContext();

            Guid requestId = Guid.NewGuid();
            
            // Note the request as "in progress" before actually sending it so that no matter how quickly the service
            // responds, this layer can correlate the request.
            var pendingReportedPropertiesUpdateRequest = new PendingReportedPropertiesUpdateRequest();
            _pendingReportedPropertyUpdateOperations[requestId] = pendingReportedPropertiesUpdateRequest;

            MqttPublish publish = new MqttPublish()
            {
                Topic = string.Format(AzureEventGridOutgoingTwinPublishTopicFormat, currentConnectionContext.DeviceId),
                QualityOfServiceLevel = MqttQualityOfServiceLevel.AtMostOnce,
                CorrelationData = requestId.ToByteArray(bigEndian: true),
                Payload = new ReportedPatch()
                {
                    IfMatch = patch.IfMatch,
                    Payload = ByteString.CopyFromUtf8(JsonSerializer.Serialize(patch.ReportedProperties))
                }.ToByteArray(),
                ContentType = ProtobufContentType,
            };

            publish.UserProperties.Add(new("type", Encoding.UTF8.GetBytes("reported-patch:1")));

            try
            {
                Trace.TraceInformation("Publishing 'PatchReported' request on topic " + publish.Topic);
                MqttPublishAck puback = await _connection.PublishAsync(publish, cancellationToken);

                PublishRejectedException.ThrowIfUnsuccessfulPuback(puback, "Failed to update the reported properties because the MQTT broker rejected the request.");

                // Wait until IoT hub sends a message to this client with the response to this patch twin request.
                var updateReportedPropertiesResponse = await pendingReportedPropertiesUpdateRequest.ReportedPropertyUpdateResponse.Task.WaitAsync(cancellationToken).ConfigureAwait(false);

                return updateReportedPropertiesResponse;
            }
            finally
            {
                // This may already be removed during happy path, but this removal covers scenarios where the response is never received
                _ = _pendingReportedPropertyUpdateOperations.Remove(requestId, out _);
            }
        }

        private ConnectionContext EnsureCorrectConnectionContext()
        {
            var currentConnectionContext = _connection.GetCurrentConnectionContext();
            if (currentConnectionContext == null)
            {
                throw new NotSupportedException("Must be connected before calling this method.");
            }

            return currentConnectionContext;
        }

        private async Task HandleReceivedMqttPublish(MqttPublishReceivedEventArgs args)
        {
            if (!args.Publish.Topic.StartsWith("ih/") || !args.Publish.Topic.EndsWith("/dev/twin"))
            {
                // The publish is not relevant to this client, so ignore it. This check needs to happen prior to checking the deviceId within the topic b/c deviceId is
                // not available until after provisioning finishes and this client may be setup prior to provisioning. This allows this client to ignore DPS
                // publishes without needing to know the deviceId.
                return;
            }

            var connectionContext = _connection.GetCurrentConnectionContext();

            if (connectionContext == null)
            {
                // Should never happen?
                Trace.TraceWarning("Cannot handle a received MQTT message while disconnected");
                return;
            }

            if (!args.Publish.Topic.Equals(string.Format(AzureEventGridIncomingTwinPublishTopicFormat, connectionContext.DeviceId)))
            {
                // This message wasn't for this device so ignore it
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

        /// <summary>
        /// Releases the unmanaged resources used by this client and optionally disposes of the managed resources.
        /// </summary>
        /// <param name="disposing">true to release both managed and unmanaged resources; false to releases only unmanaged resources.</param>
        public void Dispose(bool disposing)
        {
            _connection.PublishReceivedAsync -= HandleReceivedMqttPublish;
            if (disposing)
            {
                _connection.Dispose();
            }
            _isDisposed = true;
        }

        /// <summary>
        /// Releases the unmanaged resources and disposes of the managed resources used by this client 
        /// </summary>
        public void Dispose()
        {
            _connection.PublishReceivedAsync -= HandleReceivedMqttPublish;
            _connection.Dispose();
            _isDisposed = true;
        }
    }
}
