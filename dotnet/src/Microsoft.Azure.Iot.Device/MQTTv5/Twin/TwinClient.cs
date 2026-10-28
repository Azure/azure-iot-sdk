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
    /// The feature client for sending twin requests to IoT hub and receiving twin messages from it, exactly as IoT hub's twin
    /// API defines them.
    /// </summary>
    /// <remarks>
    /// <para>
    /// Each request is sent once, and completes with IoT hub's response, or fails if it can't be sent or the connection is lost
    /// before IoT hub responds. Requests are neither queued nor retried, and reported properties patches are only conditional
    /// on IoT hub's version of the reported properties when the caller provides that version. Every desired properties update,
    /// desired properties version probe and twin push is raised as received, so they may be raised out of version order, more
    /// than once, or not at all if the device was disconnected when IoT hub sent them.
    /// </para>
    /// <para>
    /// To keep this device's twin in sync with IoT hub as the device twin specification recommends, use
    /// <see cref="ConcurrentTwinClient"/> instead.
    /// </para>
    /// </remarks>
    public class TwinClient : IDisposable
    {
        private bool _isDisposed = false;

        private const string ProtobufContentType = "application/protobuf";

        private readonly IConnectionClient _connection;

        // The connection client substitutes this device's current ID for the placeholder when it sends each request, so a
        // pending request still goes to the right topic if the device ID changes (for example, after re-provisioning).
        private const string AzureEventGridOutgoingTwinPublishTopic = "ih/" + IConnectionClient.DeviceIdPlaceholder + "/srv/twin";
        private const string AzureEventGridIncomingTwinPublishTopic = "ih/" + IConnectionClient.DeviceIdPlaceholder + "/dev/twin";
        private const string AzureEventGridIncomingPresencePublishTopic = "ih/" + IConnectionClient.DeviceIdPlaceholder + "/dev/presence";

        private readonly ConcurrentDictionary<Guid, TaskCompletionSource<TwinGetResponse>> _pendingGets = new();
        private readonly ConcurrentDictionary<Guid, TaskCompletionSource<ReportedPatchResponse>> _pendingReportedPatches = new();

        /// <summary>
        /// An event that executes whenever this client receives a desired properties update from IoT hub.
        /// </summary>
        /// <remarks>
        /// Updates are raised as received, so they may be raised out of version order, and any that IoT hub sent while the
        /// device was disconnected are not raised at all.
        /// </remarks>
        public event Action<DesiredPatchReceivedEventArgs>? DesiredPatchReceived;

        /// <summary>
        /// An event that executes whenever IoT hub reveals its current version of the desired properties without the
        /// properties themselves (for example, when an update was too large to send to the device).
        /// </summary>
        /// <remarks>
        /// If the device doesn't hold this version of the desired properties, it can fetch them with <see cref="GetTwinAsync"/>.
        /// </remarks>
        public event Action<DesiredVersionProbeReceivedEventArgs>? DesiredVersionProbeReceived;

        /// <summary>
        /// An event that executes whenever this client receives a twin push message from IoT hub.
        /// </summary>
        public event Action<TwinPushReceivedEventArgs>? TwinPushReceived;

        // Hooks for ConcurrentTwinClient. Each is awaited as each message is received, before the next one is handled, so that
        // the caller sees the messages in the order that they were received.

        // Called with each get twin response that isn't for a request made by GetTwinAsync.
        internal Func<Guid, TwinGetResponse, Task>? GetResponseHandler { get; set; }

        // Called with each reported properties patch response that isn't for a patch sent by UpdateReportedPropertiesAsync.
        internal Func<Guid, ReportedPatchResponse, Task>? ReportedPatchResponseHandler { get; set; }

        // If set, called with each desired properties patch for the current connection instead of raising this client's events.
        internal Func<Guid?, DesiredPatch, Task>? DesiredPatchHandler { get; set; }

        // If set, called with each twin push for the current connection instead of raising TwinPushReceived.
        internal Func<Guid?, TwinPush, Task>? TwinPushHandler { get; set; }

        // Called with each message that IoT hub sends to this device's presence topic.
        internal Func<MqttPublish, Task>? PresencePublishHandler { get; set; }

        /// <summary>
        /// Construct a new <see cref="TwinClient"/> instance.
        /// </summary>
        /// <param name="connection">The connection client this feature client will use.</param>
        /// <remarks>
        /// The provided connection client does not need to be connected before this constructor is called. However, the provided
        /// connection client must be connected prior to using this feature client to send/receive any twin messages.
        /// </remarks>
        public TwinClient(IConnectionClient connection)
        {
            _connection = connection;
            _connection.PublishReceivedAsync += HandleReceivedMqttPublish;
            _connection.ConnectionLost += HandleConnectionLost;
        }

        /// <summary>
        /// Get this device's twin from IoT hub.
        /// </summary>
        /// <param name="getReported">If true, the reported properties will be included in the returned twin.</param>
        /// <param name="getDesired">If true, the desired properties will be included in the returned twin.</param>
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
        /// The request is sent once. If IoT hub never responds, this method only returns once
        /// <paramref name="cancellationToken"/> is cancelled or the connection is lost.
        /// </remarks>
        /// <exception cref="ArgumentException">Thrown when neither desired nor reported properties are requested.</exception>
        /// <exception cref="NotSupportedException">Thrown when the device isn't connected.</exception>
        /// <exception cref="DeviceException">
        /// Thrown if this get twin request could not be sent, or with <see cref="ErrorRetryability.Retryable"/> if the
        /// connection to IoT hub was lost before the response arrived. The request may be retried once reconnected.
        /// </exception>
        public async Task<DeviceTwin> GetTwinAsync(bool getReported = true, bool getDesired = true, ulong ifNotMatchReported = 0, ulong ifNotMatchDesired = 0, CancellationToken cancellationToken = default)
        {
            ObjectDisposedException.ThrowIf(_isDisposed, this);
            if (!getReported && !getDesired)
            {
                throw new ArgumentException("At least one twin section must be requested.");
            }

            EnsureConnected();

            Guid getId = Guid.NewGuid();
            TaskCompletionSource<TwinGetResponse> response = new(TaskCreationOptions.RunContinuationsAsynchronously);
            _pendingGets[getId] = response;
            TwinGetResponse twinGetResponse;
            try
            {
                await PublishGetAsync(getId, getDesired, getReported, ifNotMatchDesired, ifNotMatchReported, cancellationToken);
                twinGetResponse = await response.Task.WaitAsync(cancellationToken).ConfigureAwait(false);
            }
            finally
            {
                _pendingGets.TryRemove(getId, out _);
            }

            return new DeviceTwin()
            {
                Desired = getDesired && twinGetResponse.HasDesiredPayload
                    ? JsonObject.Parse(twinGetResponse.DesiredPayload.Span)!.AsObject()
                    : null,
                Reported = getReported && twinGetResponse.HasReportedPayload
                    ? JsonObject.Parse(twinGetResponse.ReportedPayload.Span)!.AsObject()
                    : null,
                DesiredVersion = twinGetResponse.DesiredVersion,
                ReportedVersion = twinGetResponse.ReportedVersion,
            };
        }

        /// <summary>
        /// Update this device's reported properties.
        /// </summary>
        /// <param name="patch">The patch of the reported properties to send to IoT hub.</param>
        /// <param name="ifMatch">
        /// If not 0, IoT hub only applies the patch if this is its current version of the reported properties, and otherwise
        /// responds with <see cref="Result.VersionMismatch"/> and its current version. If 0, IoT hub applies the patch
        /// unconditionally.
        /// </param>
        /// <param name="cancellationToken">The cancellation token.</param>
        /// <returns>IoT hub's response.</returns>
        /// <remarks>
        /// The patch is sent once. If IoT hub never responds, this method only returns once
        /// <paramref name="cancellationToken"/> is cancelled or the connection is lost, in which case IoT hub may or may not have
        /// applied the patch.
        /// </remarks>
        /// <exception cref="NotSupportedException">Thrown when the device isn't connected.</exception>
        /// <exception cref="DeviceException">
        /// Thrown if the patch could not be sent, or with <see cref="ErrorRetryability.Retryable"/> if the connection to IoT hub
        /// was lost before the response arrived.
        /// </exception>
        public async Task<ReportedPatchResponse> UpdateReportedPropertiesAsync(ReportedPatchRequest patch, ulong ifMatch = 0, CancellationToken cancellationToken = default)
        {
            ObjectDisposedException.ThrowIf(_isDisposed, this);
            ArgumentNullException.ThrowIfNull(patch);
            ArgumentNullException.ThrowIfNull(patch.ReportedProperties);
            EnsureConnected();

            Guid patchId = Guid.NewGuid();
            TaskCompletionSource<ReportedPatchResponse> response = new(TaskCreationOptions.RunContinuationsAsynchronously);
            _pendingReportedPatches[patchId] = response;
            try
            {
                byte[] payload = Encoding.UTF8.GetBytes(JsonSerializer.Serialize(patch.ReportedProperties));
                await PublishReportedPatchAsync(patchId, ifMatch, payload, cancellationToken);
                return await response.Task.WaitAsync(cancellationToken).ConfigureAwait(false);
            }
            finally
            {
                _pendingReportedPatches.TryRemove(patchId, out _);
            }
        }

        internal async Task PublishGetAsync(Guid getId, bool getDesired, bool getReported, ulong ifNotMatchDesired, ulong ifNotMatchReported, CancellationToken cancellationToken)
        {
            MqttPublish publish = new MqttPublish()
            {
                Topic = AzureEventGridOutgoingTwinPublishTopic,
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
            await _connection.PublishAsync(publish, throwOnErrorReasonCode: true, cancellationToken);
        }

        internal async Task PublishReportedPatchAsync(Guid patchId, ulong ifMatch, byte[] payload, CancellationToken cancellationToken)
        {
            MqttPublish publish = new MqttPublish()
            {
                Topic = AzureEventGridOutgoingTwinPublishTopic,
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
            await _connection.PublishAsync(publish, throwOnErrorReasonCode: true, cancellationToken);
        }

        // Twin requests can't outlive the connection they were sent on, since IoT hub never sends their responses on a later one.
        private void HandleConnectionLost()
        {
            FailPendingRequests(() => new DeviceException("The connection to IoT hub was lost before the response to this twin request arrived. It may be retried once reconnected.")
            {
                Retryability = ErrorRetryability.Retryable,
                IsContained = true,
            });
        }

        private void FailPendingRequests(Func<Exception> createException)
        {
            foreach (Guid getId in _pendingGets.Keys)
            {
                if (_pendingGets.TryRemove(getId, out TaskCompletionSource<TwinGetResponse>? response))
                {
                    response.TrySetException(createException());
                }
            }

            foreach (Guid patchId in _pendingReportedPatches.Keys)
            {
                if (_pendingReportedPatches.TryRemove(patchId, out TaskCompletionSource<ReportedPatchResponse>? response))
                {
                    response.TrySetException(createException());
                }
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
            bool isTwinTopic = args.Publish.Topic.EndsWith("/dev/twin");
            bool isPresenceTopic = args.Publish.Topic.EndsWith("/dev/presence");
            if (!args.Publish.Topic.StartsWith("ih/") || !(isTwinTopic || isPresenceTopic))
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

            string expectedTopic = isTwinTopic ? AzureEventGridIncomingTwinPublishTopic : AzureEventGridIncomingPresencePublishTopic;
            if (!args.Publish.Topic.Equals(expectedTopic.Replace(IConnectionClient.DeviceIdPlaceholder, connectionContext.DeviceId, StringComparison.Ordinal)))
            {
                // This message wasn't for this device so ignore it
                return;
            }

            if (isPresenceTopic)
            {
                if (PresencePublishHandler != null)
                {
                    await PresencePublishHandler(args.Publish);
                }

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
                await HandleReceivedGetResponseAsync(args.Publish);
            }
            else if (type.Equals("reported-patch-response"))
            {
                await HandleReceivedReportedPatchResponseAsync(args.Publish);
            }
            else if (type.Equals("desired-patch"))
            {
                await HandleReceivedDesiredPatchAsync(args.Publish, type);
            }
            else if (type.Equals("twin-push"))
            {
                await HandleReceivedTwinPushAsync(args.Publish, type);
            }
        }

        private async Task HandleReceivedGetResponseAsync(MqttPublish publish)
        {
            if (!GuidExtensions.TryParseBytes(publish.CorrelationData, out Guid? getId))
            {
                Trace.TraceWarning("Received a get twin response with missing or malformed correlation data. Ignoring it.");
                return;
            }

            _pendingGets.TryGetValue(getId.Value, out TaskCompletionSource<TwinGetResponse>? pendingGet);

            TwinGetResponse twinGetResponse;
            try
            {
                twinGetResponse = TwinGetResponse.Parser.ParseFrom(publish.PayloadAsReadOnlySequence);
            }
            catch (InvalidProtocolBufferException e)
            {
                Trace.TraceWarning("Received a malformed get twin response: {0}", e);
                pendingGet?.TrySetException(e);
                return;
            }

            if (pendingGet != null)
            {
                pendingGet.TrySetResult(twinGetResponse);
            }
            else if (GetResponseHandler != null)
            {
                await GetResponseHandler(getId.Value, twinGetResponse);
            }
        }

        private async Task HandleReceivedReportedPatchResponseAsync(MqttPublish publish)
        {
            if (!GuidExtensions.TryParseBytes(publish.CorrelationData, out Guid? patchId))
            {
                Trace.TraceWarning("Dropping a reported properties patch response without a valid correlation ID.");
                return;
            }

            _pendingReportedPatches.TryGetValue(patchId.Value, out TaskCompletionSource<ReportedPatchResponse>? pendingPatch);

            ReportedPatchResponse reportedPatchResponse;
            try
            {
                reportedPatchResponse = ReportedPatchResponse.Parser.ParseFrom(publish.PayloadAsReadOnlySequence);
            }
            catch (InvalidProtocolBufferException e)
            {
                Trace.TraceWarning("Received a malformed reported properties patch response: {0}", e);
                pendingPatch?.TrySetException(e);
                return;
            }

            if (pendingPatch != null)
            {
                pendingPatch.TrySetResult(reportedPatchResponse);
            }
            else if (ReportedPatchResponseHandler != null)
            {
                await ReportedPatchResponseHandler(patchId.Value, reportedPatchResponse);
            }
        }

        private async Task HandleReceivedDesiredPatchAsync(MqttPublish publish, string type)
        {
            if (!IsFromCurrentConnection(publish, type, out Guid? connectionNonce))
            {
                return;
            }

            DesiredPatch desiredPatch;
            try
            {
                desiredPatch = DesiredPatch.Parser.ParseFrom(publish.PayloadAsReadOnlySequence);
            }
            catch (InvalidProtocolBufferException e)
            {
                Trace.TraceWarning("Dropping a malformed desired properties patch: {0}", e);
                return;
            }

            if (DesiredPatchHandler != null)
            {
                await DesiredPatchHandler(connectionNonce, desiredPatch);
                return;
            }

            if (!desiredPatch.HasPayload)
            {
                DesiredVersionProbeReceived?.Invoke(new DesiredVersionProbeReceivedEventArgs() { DesiredPropertiesVersion = desiredPatch.Version });
                return;
            }

            JsonObject desiredProperties;
            try
            {
                desiredProperties = ParseSectionPayload(desiredPatch.Payload);
            }
            catch (JsonException e)
            {
                Trace.TraceWarning("Dropping a desired properties patch whose payload is not a JSON object: {0}", e);
                return;
            }

            DesiredPatchReceived?.Invoke(new DesiredPatchReceivedEventArgs()
            {
                DesiredProperties = desiredProperties,
                DesiredPropertiesVersion = desiredPatch.Version,
            });
        }

        private async Task HandleReceivedTwinPushAsync(MqttPublish publish, string type)
        {
            if (!IsFromCurrentConnection(publish, type, out Guid? connectionNonce))
            {
                return;
            }

            TwinPush twinPush;
            try
            {
                twinPush = TwinPush.Parser.ParseFrom(publish.PayloadAsReadOnlySequence);
            }
            catch (InvalidProtocolBufferException e)
            {
                Trace.TraceWarning("Dropping a malformed twin push: {0}", e);
                return;
            }

            if (TwinPushHandler != null)
            {
                await TwinPushHandler(connectionNonce, twinPush);
                return;
            }

            TwinPushReceivedEventArgs twinPushArgs = new();
            try
            {
                if (twinPush.Desired != null)
                {
                    twinPushArgs.Desired = new()
                    {
                        Properties = ParseSectionPayload(twinPush.Desired.Payload),
                        PropertiesVersion = twinPush.Desired.Version,
                    };
                }

                if (twinPush.Reported != null)
                {
                    twinPushArgs.Reported = new()
                    {
                        Properties = ParseSectionPayload(twinPush.Reported.Payload),
                        PropertiesVersion = twinPush.Reported.Version,
                    };
                }
            }
            catch (JsonException e)
            {
                Trace.TraceWarning("Dropping a twin push whose payload is not a JSON object: {0}", e);
                return;
            }

            TwinPushReceived?.Invoke(twinPushArgs);
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

        private static JsonObject ParseSectionPayload(ByteString payload)
        {
            return payload.IsEmpty ? new JsonObject() : JsonObject.Parse(payload.Span)!.AsObject();
        }

        private void ReleaseResources()
        {
            _connection.PublishReceivedAsync -= HandleReceivedMqttPublish;
            _connection.ConnectionLost -= HandleConnectionLost;
            FailPendingRequests(() => new ObjectDisposedException(nameof(TwinClient)));
        }

        /// <summary>
        /// Releases the unmanaged resources used by this client and optionally disposes of the managed resources.
        /// </summary>
        /// <param name="disposing">true to release both managed and unmanaged resources; false to releases only unmanaged resources.</param>
        public void Dispose(bool disposing)
        {
            ReleaseResources();
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
            ReleaseResources();
            _connection.Dispose();
            _isDisposed = true;
        }
    }
}
