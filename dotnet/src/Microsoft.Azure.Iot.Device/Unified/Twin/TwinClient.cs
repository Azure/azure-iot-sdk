// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Models.Twin;
using Microsoft.Azure.Iot.Device.Mqtt;
using Microsoft.Azure.Iot.Device.Unified.Connection;
using System.Collections.Concurrent;
using System.Collections.Specialized;
using System.Diagnostics;
using System.Globalization;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;
using System.Web;

namespace Microsoft.Azure.Iot.Device.Unified.Twin
{
    /// <summary>
    /// The feature client for interacting with a device's twin properties.
    /// </summary>
    public class TwinClient : IDisposable
    {
        private bool _isDisposed = false;

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

        private const string RequestIdTopicKey = "$rid";
        internal const string VersionKey = "$version";

        private readonly ConcurrentDictionary<Guid, PendingGetTwinRequest> _pendingGetTwinOperations = new();
        private readonly ConcurrentDictionary<Guid, PendingReportedPropertiesUpdateRequest> _pendingReportedPropertyUpdateOperations = new();

        /// <summary>
        /// An event that executes whenever this client receives a desired properties update from IoT hub.
        /// </summary>
        public event Action<DesiredPatchReceivedEventArgs>? DesiredPatchReceived;

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

        private void HandleMQTTv5DesiredPatchReceivedAsync(DesiredPatchReceivedEventArgs args)
        {
            if (DesiredPatchReceived != null)
            {
                DesiredPatchReceived.Invoke(args);
            }
        }

        /// <summary>
        /// Get the full twin.
        /// </summary>
        /// <param name="cancellationToken">The cancellation token.</param>
        /// <returns>The returned twin.</returns>
        /// <exception cref="PublishRejectedException">Thrown if this get twin request is rejected by IoT Hub for any reason.</exception>
        public async Task<DeviceTwin> GetTwinAsync(CancellationToken cancellationToken = default)
        {
            ObjectDisposedException.ThrowIf(_isDisposed, this);

            var currentConnectionContext = EnsureCorrectConnectionContext();

            Guid requestId = Guid.NewGuid();

            // Note the request as "in progress" before actually sending it so that no matter how quickly the service
            // responds, this layer can correlate the request.
            var pendingGetTwinRequest = new PendingGetTwinRequest()
            {
                GetDesired = true,
                GetReported = true,
                IfNotMatchDesired = 0,
                IfNotMatchReported = 0,
            };
            _pendingGetTwinOperations[requestId] = pendingGetTwinRequest;

            MqttPublish publish = new MqttPublish()
            {
                Topic = string.Format(CultureInfo.InvariantCulture, ClassicTwinGetTopicFormat, requestId.ToString()),
                QualityOfServiceLevel = MqttQualityOfServiceLevel.AtLeastOnce,
            };

            try
            {
                Trace.TraceInformation("Publishing 'GetTwin' request on topic " + publish.Topic);

                // Puback is checked for non-success cases under this layer, so no need to check it here as well
                MqttPublishAck puback = await _connection.PublishAsync(publish, cancellationToken);

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
        /// <param name="reportedProperties">The patch of the reported properties to send to IoT hub</param>
        /// <param name="cancellationToken">The cancellation token.</param>
        /// <returns>Whether IoT hub accepted this patch.</returns>
        /// <exception cref="PublishRejectedException">Thrown if this reported property update message is rejected by IoT Hub for any reason.</exception>
        public async Task<ReportedPatchResponse> UpdateReportedPropertiesAsync(JsonObject reportedProperties, CancellationToken cancellationToken = default)
        {
            ObjectDisposedException.ThrowIf(_isDisposed, this);

            var currentConnectionContext = EnsureCorrectConnectionContext();

            MqttPublish publish;
            Guid requestId = Guid.NewGuid();

            // Note the request as "in progress" before actually sending it so that no matter how quickly the service
            // responds, this layer can correlate the request.
            var pendingReportedPropertiesUpdateRequest = new PendingReportedPropertiesUpdateRequest();
            _pendingReportedPropertyUpdateOperations[requestId] = pendingReportedPropertiesUpdateRequest;

            string topic = string.Format(CultureInfo.InvariantCulture, ClassicTwinReportedPropertiesPatchTopicFormat, requestId);

            publish = new MqttPublish()
            {
                Topic = topic,
                QualityOfServiceLevel = MqttQualityOfServiceLevel.AtLeastOnce,
                Payload = JsonSerializer.SerializeToUtf8Bytes(reportedProperties),
            };

            try
            {
                Trace.TraceInformation("Publishing 'PatchReported' request on topic " + publish.Topic);

                // Puback is checked for non-success cases under this layer, so no need to check it here as well
                MqttPublishAck puback = await _connection.PublishAsync(publish, cancellationToken);

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

        private async Task HandleReceivedMqttPublish(MqttPublishReceivedEventArgs args)
        {
            if (!args.Publish.Topic.StartsWith("$iothub/twin/", StringComparison.Ordinal))
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

            if (connectionContext.ConnectionProfile == Provisioning.Models.ConnectionProfile.MqttV5)
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

                        // Remove the "$version" entry so that the twin object more closely mimics how it would in MQTTv5 scenario
                        clientTwinProperties["desired"]!.AsObject().Remove(VersionKey);

                        var reportedVersion = clientTwinProperties["reported"]![VersionKey];
                        ulong reportedPropertiesVersion = (ulong)reportedVersion!.AsValue();

                        // Remove the "$version" entry so that the twin object more closely mimics how it would in MQTTv5 scenario
                        clientTwinProperties["reported"]!.AsObject().Remove(VersionKey);

                        var twinGetResponse = new DeviceTwin()
                        {
                            DesiredVersion = desiredPropertiesVersion,
                            ReportedVersion = reportedPropertiesVersion,
                        };

                        // These user-supplied configurations are handled by the service if it is an MQTTv5 broker, but classic hub does not actually support them. The below
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
                            //TODO this is probably not common enough between hub types. Remove this field in the unified namespace
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

        private ConnectionContext EnsureCorrectConnectionContext()
        {
            var currentConnectionContext = _connection.GetCurrentConnectionContext();
            if (currentConnectionContext == null)
            {
                throw new NotSupportedException("Must be connected before calling this method.");
            }

            return currentConnectionContext;
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
            GC.SuppressFinalize(this);
        }
    }
}
