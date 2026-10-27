// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Google.Protobuf;
using Microsoft.Azure.Iot.Device.Exceptions;
using Microsoft.Azure.Iot.Device.MQTTv5.Connection;
using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Models.Twin;
using Microsoft.Azure.Iot.Device.Mqtt;
using System.Collections.Concurrent;
using System.Diagnostics;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace Microsoft.Azure.Iot.Device.MQTTv5.Twin
{
    /// <summary>
    /// The feature client for interacting with a device's twin properties.
    /// </summary>
    public class TwinClient : IDisposable
    {
        private bool _isDisposed = false;

        private const string ProtobufContentType = "application/protobuf";

        private IConnectionClient _connection;

        // The connection client substitutes this device's current ID for the placeholder when it sends each request, so a
        // pending request still goes to the right topic if the device ID changes (for example, after re-provisioning).
        private const string AzureEventGridOutgoingTwinPublishTopic = "ih/" + IConnectionClient.DeviceIdPlaceholder + "/srv/twin";
        private const string AzureEventGridIncomingTwinPublishTopic = "ih/" + IConnectionClient.DeviceIdPlaceholder + "/dev/twin";

        private readonly ConcurrentDictionary<Guid, PendingGetTwinRequest> _pendingGetTwinOperations = new();
        private readonly ConcurrentDictionary<Guid, PendingReportedPropertiesUpdateRequest> _pendingReportedPropertyUpdateOperations = new();

        // The maximum number of desired patches buffered while resyncing the desired properties.
        internal const int DesiredResyncBufferCapacity = 8;

        // Guards the device's view of the desired properties (the desired section state machine).
        private readonly object _desiredStateLock = new();

        // The version of the desired properties that this device has most recently observed. 0 means the device has not
        // observed the desired properties yet. Unlike the resync state, this survives disconnects.
        private ulong _localDesiredVersion;

        // Non-null while this client is resyncing its desired properties with IoT hub.
        private DesiredResync? _desiredResync;

        /// <summary>
        /// How long to wait for the response to the given attempt (starting at 1) of a desired properties resync GET
        /// request before sending it again.
        /// </summary>
        internal Func<int, TimeSpan> DesiredResyncTimeoutSchedule { get; set; } = GetDefensiveTimeout;

        /// <summary>
        /// An event that executes whenever this client receives a desired properties update from IoT hub.
        /// </summary>
        /// <remarks>
        /// Patches are only raised in version order, each exactly one version newer than the desired properties this client
        /// last observed. If a patch is missed, this client fetches the full desired properties from IoT hub and raises
        /// <see cref="TwinPushReceived"/> with them instead, followed by any newer patches.
        /// </remarks>
        public event Action<DesiredPatchReceivedEventArgs>? DesiredPatchReceived;

        /// <summary>
        /// An event that executes whenever this client receives a Twin push message from IoT hub.
        /// </summary>
        /// <remarks>
        /// This feature is only supported by IoT hubs using MQTTv5. Older IoT hubs will never encounter this event.
        /// The desired section is only included if it is newer than the desired properties this client last observed. This
        /// event is also raised with just the desired section when this client resynchronizes its desired properties
        /// with IoT hub (for example, after missing a desired properties patch).
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
        /// Only IoT hubs that use MQTTv5 will actually respond to the getReported/getDesired/ifNotMatch flags as this feature is unsupported in older IoT hubs. 
        /// However, this SDK will parse the twin that the service returns to filter out unrequested sections to mimic the behavior of MQTTv5 IoT hubs.
        /// </remarks>
        /// <exception cref="ArgumentException">Thrown when neither desired nor reported properties are requested.</exception>
        /// <exception cref="PublishRejectedException">Thrown if this get twin request is rejected by IoT Hub for any reason.</exception>
        public async Task<DeviceTwin> GetTwinAsync(bool getReported = true, bool getDesired = true, ulong ifNotMatchReported = 0, ulong ifNotMatchDesired = 0, CancellationToken cancellationToken = default)
        {
            //TODO need to handle case where get twin request is successfully published, but connection + session is lost before receiving response.
            // Would need to re-send the get twin request upon device ready
            ObjectDisposedException.ThrowIf(_isDisposed, this);
            if (!getReported && !getDesired)
            {
                throw new ArgumentException("At least one twin section must be requested.");
            }

            EnsureConnected();

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
                Topic = AzureEventGridOutgoingTwinPublishTopic,
                QualityOfServiceLevel = MqttQualityOfServiceLevel.AtMostOnce,
                CorrelationData = requestId.ToByteArray(bigEndian: true),
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

            try
            {
                Trace.TraceInformation("Publishing 'GetTwin' request on topic " + publish.Topic);

                // Puback is checked for non-success cases under this layer, so no need to check it here as well
                MqttPublishAck puback = await _connection.PublishAsync(publish, throwOnErrorReasonCode: true, cancellationToken);

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

            EnsureConnected();

            Guid requestId = Guid.NewGuid();

            // Note the request as "in progress" before actually sending it so that no matter how quickly the service
            // responds, this layer can correlate the request.
            var pendingReportedPropertiesUpdateRequest = new PendingReportedPropertiesUpdateRequest();
            _pendingReportedPropertyUpdateOperations[requestId] = pendingReportedPropertiesUpdateRequest;

            MqttPublish publish = new MqttPublish()
            {
                Topic = AzureEventGridOutgoingTwinPublishTopic,
                QualityOfServiceLevel = MqttQualityOfServiceLevel.AtMostOnce,
                CorrelationData = requestId.ToByteArray(bigEndian: true),
                Payload = new ReportedPatch()
                {
                    IfMatch = patch.IfMatch, // If this value is 0, the if-match field will not be included in the serialized payload and the request is treated as unconditional
                    Payload = ByteString.CopyFromUtf8(JsonSerializer.Serialize(patch.ReportedProperties))
                }.ToByteArray(),
                ContentType = ProtobufContentType,
            };

            publish.UserProperties.Add(new("type", Encoding.UTF8.GetBytes("reported-patch:1")));

            try
            {
                Trace.TraceInformation("Publishing 'PatchReported' request on topic " + publish.Topic);

                // Puback is checked for non-success cases under this layer, so no need to check it here as well
                MqttPublishAck puback = await _connection.PublishAsync(publish, throwOnErrorReasonCode: true, cancellationToken);

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

        private void EnsureConnected()
        {
            if (_connection.GetCurrentConnectionContext() == null || !_connection.IsConnectionTargetSet)
            {
                throw new NotSupportedException("Must be connected before calling this method.");
            }
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

            if (!args.Publish.Topic.Equals(AzureEventGridIncomingTwinPublishTopic.Replace(IConnectionClient.DeviceIdPlaceholder, connectionContext.DeviceId, StringComparison.Ordinal)))
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

            if (type.Equals("get-response"))
            {
                if (!GuidExtensions.TryParseBytes(args.Publish.CorrelationData, out Guid? getResponseCorrelationData))
                {
                    Trace.TraceWarning("Received a get twin response with missing or malformed correlation data. Ignoring it.");
                    return;
                }

                if (!_pendingGetTwinOperations.TryRemove(getResponseCorrelationData.Value, out PendingGetTwinRequest? pendingGetTwinRequest))
                {
                    await HandleDesiredResyncGetResponseAsync(getResponseCorrelationData.Value, args.Publish);
                    return;
                }

                TwinGetResponse twinGetResponse = TwinGetResponse.Parser.ParseFrom(args.Publish.PayloadAsReadOnlySequence);

                if (pendingGetTwinRequest.GetDesired && twinGetResponse.HasDesiredPayload)
                {
                    lock (_desiredStateLock)
                    {
                        // The application fetched the desired properties, so this device's view of them is now at least this version.
                        _localDesiredVersion = Math.Max(_localDesiredVersion, twinGetResponse.DesiredVersion);
                    }
                }

                pendingGetTwinRequest.TwinResponseTask.TrySetResult(new()
                {
                    Desired = pendingGetTwinRequest.GetDesired
                        && twinGetResponse.HasDesiredPayload
                        ? JsonObject.Parse(twinGetResponse.DesiredPayload.Span)!.AsObject()
                        : null,
                    Reported = pendingGetTwinRequest.GetReported
                        && twinGetResponse.HasReportedPayload
                        ? JsonObject.Parse(twinGetResponse.ReportedPayload.Span)!.AsObject()
                        : null,
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
            else if (type.Equals("desired-patch"))
            {
                if (!IsFromCurrentConnection(args.Publish, type, out Guid? connectionNonce))
                {
                    return;
                }

                DesiredPatch receivedDesiredPatch = DesiredPatch.Parser.ParseFrom(args.Publish.PayloadAsReadOnlySequence);

                DesiredStateOutcome outcome = new();
                lock (_desiredStateLock)
                {
                    DiscardDesiredResyncFromPreviousConnection(connectionNonce);
                    ProcessDesiredPatch(receivedDesiredPatch, connectionNonce, outcome);
                }

                await CompleteAsync(outcome);
            }
            else if (type.Equals("twin-push"))
            {
                if (!IsFromCurrentConnection(args.Publish, type, out Guid? connectionNonce))
                {
                    return;
                }

                TwinPush receivedTwinPush = TwinPush.Parser.ParseFrom(args.Publish.PayloadAsReadOnlySequence);
                var twinPushArgs = new TwinPushReceivedEventArgs();
                if (receivedTwinPush.Reported != null)
                {
                    twinPushArgs.Reported = new()
                    {
                        Properties = ParseSectionPayload(receivedTwinPush.Reported.Payload),
                        PropertiesVersion = receivedTwinPush.Reported.Version
                    };
                }

                DesiredStateOutcome outcome = new();
                lock (_desiredStateLock)
                {
                    DiscardDesiredResyncFromPreviousConnection(connectionNonce);

                    if (receivedTwinPush.Desired != null)
                    {
                        twinPushArgs.Desired = TryApplyDesiredSnapshot(receivedTwinPush.Desired.Version, receivedTwinPush.Desired.Payload);
                        if (twinPushArgs.Desired == null)
                        {
                            Trace.TraceInformation("Dropping the desired section of a twin push with version {0} because this device has already observed version {1}.", receivedTwinPush.Desired.Version, _localDesiredVersion);
                        }
                    }

                    if (twinPushArgs.Desired != null || twinPushArgs.Reported != null)
                    {
                        outcome.Deliveries.Add(twinPushArgs);
                    }

                    if (twinPushArgs.Desired != null)
                    {
                        // The snapshot resolves any resync in progress.
                        ResolveDesiredResync(connectionNonce, outcome);
                    }
                }

                await CompleteAsync(outcome);
            }
        }

        // Returns false if the publish is a connection scoped message (a twin push or desired patch) for a different connection than the current one.
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

        // Must be called while holding _desiredStateLock.
        private void ProcessDesiredPatch(DesiredPatch patch, Guid? connectionNonce, DesiredStateOutcome outcome)
        {
            if (_desiredResync != null)
            {
                if (!patch.HasPayload)
                {
                    // A version probe. The outstanding resync GET will return at least this version.
                    return;
                }

                if (patch.Version <= _localDesiredVersion)
                {
                    return;
                }

                if (_desiredResync.Buffer.Count >= DesiredResyncBufferCapacity)
                {
                    // The response to a new GET will include every patch buffered so far (and this one), so they can be dropped.
                    Trace.TraceWarning("Too many desired property patches were received while resyncing the desired properties. Requesting the desired properties again.");
                    _desiredResync.Buffer.Clear();
                    SendNewDesiredResyncGet(_desiredResync, outcome);
                    return;
                }

                _desiredResync.Buffer.Add(patch);
                return;
            }

            if (!patch.HasPayload)
            {
                // A version probe from IoT hub.
                if (patch.Version > _localDesiredVersion)
                {
                    Trace.TraceInformation("Received a desired properties version probe with version {0}, but this device has only observed version {1}. Resyncing the desired properties.", patch.Version, _localDesiredVersion);
                    StartDesiredResync(connectionNonce, null, outcome);
                }

                // Otherwise, this device is already up to date (or the probe is stale), so drop it.
                return;
            }

            if (patch.Version <= _localDesiredVersion)
            {
                Trace.TraceInformation("Dropping desired property patch with version {0} because this device has already observed version {1}.", patch.Version, _localDesiredVersion);
                return;
            }

            if (patch.Version > _localDesiredVersion + 1)
            {
                Trace.TraceInformation("Received desired property patch with version {0}, but this device has only observed version {1}. Resyncing the desired properties.", patch.Version, _localDesiredVersion);
                StartDesiredResync(connectionNonce, patch, outcome);
                return;
            }

            JsonObject desiredProperties = ParseSectionPayload(patch.Payload);
            _localDesiredVersion = patch.Version;
            outcome.Deliveries.Add(new DesiredPatchReceivedEventArgs()
            {
                DesiredProperties = desiredProperties,
                DesiredPropertiesVersion = patch.Version,
            });
        }

        // Must be called while holding _desiredStateLock. Returns null if the snapshot is not newer than this device's view.
        private TwinPushSection? TryApplyDesiredSnapshot(ulong version, ByteString payload)
        {
            if (version <= _localDesiredVersion)
            {
                return null;
            }

            JsonObject desiredProperties = ParseSectionPayload(payload);
            _localDesiredVersion = version;
            return new TwinPushSection()
            {
                Properties = desiredProperties,
                PropertiesVersion = version,
            };
        }

        // Must be called while holding _desiredStateLock.
        private void StartDesiredResync(Guid? connectionNonce, DesiredPatch? firstBufferedPatch, DesiredStateOutcome outcome)
        {
            _desiredResync = new DesiredResync(connectionNonce);
            if (firstBufferedPatch != null)
            {
                _desiredResync.Buffer.Add(firstBufferedPatch);
            }

            SendNewDesiredResyncGet(_desiredResync, outcome);
        }

        // Must be called while holding _desiredStateLock. Ends the resync (if any) and applies its buffered patches that are newer than this device's view.
        private void ResolveDesiredResync(Guid? connectionNonce, DesiredStateOutcome outcome)
        {
            DesiredResync? resync = _desiredResync;
            if (resync == null)
            {
                return;
            }

            CancelDesiredResync();

            // If there is a gap in the buffered patches, this starts a new resync that buffers the rest of them.
            foreach (DesiredPatch bufferedPatch in resync.Buffer.OrderBy(patch => patch.Version))
            {
                ProcessDesiredPatch(bufferedPatch, connectionNonce, outcome);
            }
        }

        // Must be called while holding _desiredStateLock.
        private void SendNewDesiredResyncGet(DesiredResync resync, DesiredStateOutcome outcome)
        {
            Guid getId = Guid.NewGuid();
            resync.GetId = getId;
            resync.Timer?.Dispose();
            resync.Timer = new System.Threading.Timer(
                _ => HandleDesiredResyncTimeout(resync, getId),
                null,
                DesiredResyncTimeoutSchedule(resync.Attempt),
                Timeout.InfiniteTimeSpan);
            outcome.ResyncGetId = getId;
        }

        // Must be called while holding _desiredStateLock.
        private void CancelDesiredResync()
        {
            _desiredResync?.Timer?.Dispose();
            _desiredResync = null;
        }

        // Must be called while holding _desiredStateLock. A resync does not survive a disconnect, since its GET response cannot arrive on a later connection.
        private void DiscardDesiredResyncFromPreviousConnection(Guid? connectionNonce)
        {
            if (_desiredResync != null && _desiredResync.ConnectionNonce != connectionNonce)
            {
                CancelDesiredResync();
            }
        }

        private void HandleDesiredResyncTimeout(DesiredResync resync, Guid getId)
        {
            DesiredStateOutcome outcome = new();
            lock (_desiredStateLock)
            {
                if (_isDisposed || _desiredResync != resync || resync.GetId != getId)
                {
                    return;
                }

                if (resync.ConnectionNonce != _connection.ConnectionNonce)
                {
                    CancelDesiredResync();
                    return;
                }

                Trace.TraceWarning("Timed out waiting for the response to the GET request for resyncing the desired properties. Sending it again.");
                resync.Attempt++;
                SendNewDesiredResyncGet(resync, outcome);
            }

            _ = CompleteAsync(outcome);
        }

        private async Task HandleDesiredResyncGetResponseAsync(Guid getId, MqttPublish publish)
        {
            DesiredStateOutcome outcome = new();
            lock (_desiredStateLock)
            {
                Guid? connectionNonce = _connection.ConnectionNonce;
                DiscardDesiredResyncFromPreviousConnection(connectionNonce);

                if (_desiredResync == null || _desiredResync.GetId != getId)
                {
                    Trace.TraceInformation("Dropping a get twin response that does not match any outstanding get twin request.");
                    return;
                }

                TwinGetResponse response = TwinGetResponse.Parser.ParseFrom(publish.PayloadAsReadOnlySequence);
                TwinPushSection? snapshot = response.HasDesiredPayload
                    ? TryApplyDesiredSnapshot(response.DesiredVersion, response.DesiredPayload)
                    : null;

                if (snapshot != null)
                {
                    outcome.Deliveries.Add(new TwinPushReceivedEventArgs() { Desired = snapshot });
                }

                ResolveDesiredResync(connectionNonce, outcome);
            }

            await CompleteAsync(outcome);
        }

        // Raises the events and sends the GET request that the desired section state machine decided on, outside of _desiredStateLock.
        private async Task CompleteAsync(DesiredStateOutcome outcome)
        {
            foreach (EventArgs delivery in outcome.Deliveries)
            {
                if (delivery is DesiredPatchReceivedEventArgs desiredPatchArgs)
                {
                    DesiredPatchReceived?.Invoke(desiredPatchArgs);
                }
                else if (delivery is TwinPushReceivedEventArgs twinPushArgs)
                {
                    TwinPushReceived?.Invoke(twinPushArgs);
                }
            }

            if (outcome.ResyncGetId is Guid getId)
            {
                await SendDesiredResyncGetAsync(getId);
            }
        }

        private async Task SendDesiredResyncGetAsync(Guid getId)
        {
            MqttPublish publish = new MqttPublish()
            {
                Topic = AzureEventGridOutgoingTwinPublishTopic,
                QualityOfServiceLevel = MqttQualityOfServiceLevel.AtMostOnce,
                CorrelationData = getId.ToByteArray(bigEndian: true),
                Payload = new TwinGet()
                {
                    Sections = Sections.Desired,
                }.ToByteArray(),
                ContentType = ProtobufContentType,
            };

            publish.UserProperties.Add(new("type", Encoding.UTF8.GetBytes("get:1")));

            try
            {
                Trace.TraceInformation("Publishing 'GetTwin' request on topic " + publish.Topic + " to resync the desired properties");
                await _connection.PublishAsync(publish, throwOnErrorReasonCode: true);
            }
            catch (Exception e)
            {
                // The defensive timeout sends this request again later.
                Trace.TraceWarning("Failed to publish the GET request for resyncing the desired properties: {0}", e);
            }
        }

        private static JsonObject ParseSectionPayload(ByteString payload)
        {
            return payload.IsEmpty ? new JsonObject() : JsonObject.Parse(payload.Span)!.AsObject();
        }

        private static TimeSpan GetDefensiveTimeout(int attempt)
        {
            int baseMinutes = attempt switch
            {
                1 => 5,
                2 => 6,
                3 => 8,
                _ => 10,
            };

            return TimeSpan.FromMinutes(baseMinutes) + TimeSpan.FromMinutes(5 * Random.Shared.NextDouble());
        }

        private sealed class DesiredResync(Guid? connectionNonce)
        {
            public Guid? ConnectionNonce { get; } = connectionNonce;

            public Guid GetId { get; set; }

            public int Attempt { get; set; } = 1;

            public List<DesiredPatch> Buffer { get; } = new();

            public System.Threading.Timer? Timer { get; set; }
        }

        private sealed class DesiredStateOutcome
        {
            public List<EventArgs> Deliveries { get; } = new();

            public Guid? ResyncGetId { get; set; }
        }

        /// <summary>
        /// Releases the unmanaged resources used by this client and optionally disposes of the managed resources.
        /// </summary>
        /// <param name="disposing">true to release both managed and unmanaged resources; false to releases only unmanaged resources.</param>
        public void Dispose(bool disposing)
        {
            _connection.PublishReceivedAsync -= HandleReceivedMqttPublish;
            lock (_desiredStateLock)
            {
                CancelDesiredResync();
            }
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
            lock (_desiredStateLock)
            {
                CancelDesiredResync();
            }
            _connection.Dispose();
            _isDisposed = true;
        }
    }
}
