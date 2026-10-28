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
        private const string AzureEventGridIncomingPresencePublishTopic = "ih/" + IConnectionClient.DeviceIdPlaceholder + "/dev/presence";

        // Guards _getTwinExchangesByRequest and _getTwinExchangesById.
        private readonly object _getTwinExchangesLock = new();

        // The outstanding get twin exchange for each distinct get twin request. Identical concurrent requests share one exchange.
        private readonly Dictionary<GetTwinRequest, GetTwinExchange> _getTwinExchangesByRequest = new();

        // The outstanding get twin exchanges, keyed by the ID of their latest attempt. Responses to earlier attempts are dropped.
        private readonly Dictionary<Guid, GetTwinExchange> _getTwinExchangesById = new();

        private readonly ConcurrentDictionary<Guid, ReportedPatchExchange> _pendingReportedPropertyUpdateOperations = new();

        // The maximum number of desired patches buffered while resyncing the desired properties.
        internal const int DesiredResyncBufferCapacity = 8;

        // Guards the device's view of the desired properties (the desired section state machine).
        private readonly object _desiredStateLock = new();

        // The version of the desired properties that this device has most recently observed. 0 means the device has not
        // observed the desired properties yet. Unlike the resync state, this survives disconnects.
        private ulong _localDesiredVersion;

        // Guards _localReportedVersion.
        private readonly object _reportedStateLock = new();

        // The version of the reported properties that this device holds in full. 0 means the device does not hold them.
        private ulong _localReportedVersion;

        // Non-null while this client is resyncing its desired properties with IoT hub.
        private DesiredResync? _desiredResync;

        // Guards _authoritativeVersions.
        private readonly object _authoritativeVersionsLock = new();

        // The latest versions of the twin sections that IoT hub has revealed on the current connection.
        private AuthoritativeVersions? _authoritativeVersions;

        /// <summary>
        /// How long to wait for the response to the given attempt (starting at 1) of a desired properties resync GET
        /// request before sending it again.
        /// </summary>
        internal Func<int, TimeSpan> DesiredResyncTimeoutSchedule { get; set; } = DefensiveTimeoutSchedule.GetTimeout;

        /// <summary>
        /// How long to wait for the response to the given attempt (starting at 1) of an application's get twin request
        /// before sending it again.
        /// </summary>
        internal Func<int, TimeSpan> GetTwinTimeoutSchedule { get; set; } = DefensiveTimeoutSchedule.GetTimeout;

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
        /// The latest version of the desired properties that IoT hub has revealed on the current connection, or null if it
        /// hasn't revealed one yet.
        /// </summary>
        /// <remarks>
        /// IoT hub reveals its version of the desired properties when it acknowledges this device's presence upon each
        /// (re)connection, and in every desired properties patch, twin push and get twin response. This value is reset
        /// whenever the device reconnects.
        /// </remarks>
        public ulong? AuthoritativeDesiredPropertiesVersion => GetAuthoritativeVersion(desired: true);

        /// <summary>
        /// The latest version of the reported properties that IoT hub has revealed on the current connection, or null if it
        /// hasn't revealed one yet.
        /// </summary>
        /// <remarks>
        /// IoT hub reveals its version of the reported properties when it acknowledges this device's presence upon each
        /// (re)connection, and in every twin push, get twin response and reported properties update response. This is the
        /// value to use as <see cref="ReportedPatchRequest.IfMatch"/> to only update the reported properties if nothing
        /// else has updated them since. This value is reset whenever the device reconnects.
        /// </remarks>
        public ulong? AuthoritativeReportedPropertiesVersion => GetAuthoritativeVersion(desired: false);

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
            _localDesiredVersion = connection.InitialDesiredPropertiesVersion;
            _localReportedVersion = connection.InitialReportedPropertiesVersion;
            _connection.PublishReceivedAsync += HandleReceivedMqttPublish;
            _connection.ConnectionLost += HandleConnectionLost;
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
        /// <para>
        /// Only IoT hubs that use MQTTv5 will actually respond to the getReported/getDesired/ifNotMatch flags as this feature is unsupported in older IoT hubs. 
        /// However, this SDK will parse the twin that the service returns to filter out unrequested sections to mimic the behavior of MQTTv5 IoT hubs.
        /// </para>
        /// <para>
        /// Concurrent calls with identical arguments share a single request to IoT hub and all receive its response. If IoT hub
        /// doesn't respond within a defensive timeout (5 to 15 minutes, growing with each attempt), the request is sent again.
        /// </para>
        /// </remarks>
        /// <exception cref="ArgumentException">Thrown when neither desired nor reported properties are requested.</exception>
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

            GetTwinRequest request = new(getDesired, getReported, ifNotMatchDesired, ifNotMatchReported);
            GetTwinExchange? exchange;
            bool isNewExchange = false;
            lock (_getTwinExchangesLock)
            {
                if (!_getTwinExchangesByRequest.TryGetValue(request, out exchange))
                {
                    exchange = new GetTwinExchange(request);
                    _getTwinExchangesByRequest[request] = exchange;
                    isNewExchange = true;
                }

                exchange.WaiterCount++;
            }

            try
            {
                if (isNewExchange)
                {
                    SendNextGetTwinAttempt(exchange);
                }

                return await exchange.Response.Task.WaitAsync(cancellationToken).ConfigureAwait(false);
            }
            finally
            {
                lock (_getTwinExchangesLock)
                {
                    // Stop retrying the request once nobody is waiting for its response.
                    if (--exchange.WaiterCount == 0)
                    {
                        EndGetTwinExchange(exchange);
                    }
                }
            }
        }

        /// <summary>
        /// Update this device's reported properties.
        /// </summary>
        /// <param name="patch">The patch of the reported properties to send to IoT hub</param>
        /// <param name="cancellationToken">The cancellation token.</param>
        /// <returns>Whether IoT hub accepted this patch.</returns>
        /// <exception cref="DeviceException">
        /// Thrown if this patch could not be sent, or with <see cref="ErrorRetryability.Retryable"/> if the connection to
        /// IoT hub was lost before the response arrived. In that case, IoT hub may or may not have applied the patch.
        /// </exception>
        public async Task<ReportedPatchResponse> UpdateReportedPropertiesAsync(ReportedPatchRequest patch, CancellationToken cancellationToken = default)
        {
            ObjectDisposedException.ThrowIf(_isDisposed, this);

            EnsureConnected();

            Guid requestId = Guid.NewGuid();

            // Note the request as "in progress" before actually sending it so that no matter how quickly the service
            // responds, this layer can correlate the request.
            var exchange = new ReportedPatchExchange();
            _pendingReportedPropertyUpdateOperations[requestId] = exchange;

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

                using (var publishCancellation = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken, exchange.Cancellation.Token))
                {
                    try
                    {
                        // Puback is checked for non-success cases under this layer, so no need to check it here as well
                        await _connection.PublishAsync(publish, throwOnErrorReasonCode: true, publishCancellation.Token);
                    }
                    catch (OperationCanceledException) when (exchange.Response.Task.IsCompleted && !cancellationToken.IsCancellationRequested)
                    {
                        // The connection was lost before this patch was sent. The response task surfaces why below.
                    }
                }

                // Wait until IoT hub sends a message to this client with the response to this patch twin request.
                return await exchange.Response.Task.WaitAsync(cancellationToken).ConfigureAwait(false);
            }
            finally
            {
                // This may already be removed during happy path, but this removal covers scenarios where the response is never received
                _ = _pendingReportedPropertyUpdateOperations.Remove(requestId, out _);
            }
        }

        // Sends the first attempt of the exchange, or a new attempt (with a new ID, so that responses to earlier attempts are dropped).
        private void SendNextGetTwinAttempt(GetTwinExchange exchange)
        {
            Guid getId = Guid.NewGuid();
            lock (_getTwinExchangesLock)
            {
                if (exchange.IsEnded)
                {
                    return;
                }

                if (exchange.GetId is Guid previousGetId)
                {
                    _getTwinExchangesById.Remove(previousGetId);
                }

                exchange.GetId = getId;
                _getTwinExchangesById[getId] = exchange;
                exchange.Timer?.Dispose();
                exchange.Timer = new System.Threading.Timer(
                    _ => HandleGetTwinTimeout(exchange, getId),
                    null,
                    GetTwinTimeoutSchedule(exchange.Attempt),
                    Timeout.InfiniteTimeSpan);
            }

            _ = PublishGetTwinAsync(exchange, getId);
        }

        private async Task PublishGetTwinAsync(GetTwinExchange exchange, Guid getId)
        {
            GetTwinRequest request = exchange.Request;
            MqttPublish publish = new MqttPublish()
            {
                Topic = AzureEventGridOutgoingTwinPublishTopic,
                QualityOfServiceLevel = MqttQualityOfServiceLevel.AtMostOnce,
                CorrelationData = getId.ToByteArray(bigEndian: true),
                Payload = new TwinGet()
                {
                    Sections = request.GetReported
                        ? request.GetDesired ? Sections.Both : Sections.Reported
                        : Sections.Desired,
                    IfNotMatchDesired = request.IfNotMatchDesired,
                    IfNotMatchReported = request.IfNotMatchReported,
                }.ToByteArray(),
                ContentType = ProtobufContentType,
            };

            publish.UserProperties.Add(new("type", Encoding.UTF8.GetBytes("get:1")));

            try
            {
                Trace.TraceInformation("Publishing 'GetTwin' request on topic " + publish.Topic);

                // Puback is checked for non-success cases under this layer, so no need to check it here as well
                await _connection.PublishAsync(publish, throwOnErrorReasonCode: true, exchange.Cancellation.Token);
            }
            catch (OperationCanceledException) when (exchange.Cancellation.IsCancellationRequested)
            {
                // The exchange already ended.
            }
            catch (Exception e)
            {
                lock (_getTwinExchangesLock)
                {
                    EndGetTwinExchange(exchange);
                }

                exchange.Response.TrySetException(e);
            }
        }

        private void HandleGetTwinTimeout(GetTwinExchange exchange, Guid getId)
        {
            lock (_getTwinExchangesLock)
            {
                if (_isDisposed || exchange.IsEnded || exchange.GetId != getId)
                {
                    return;
                }

                exchange.Attempt++;
            }

            Trace.TraceWarning("Timed out waiting for the response to a get twin request. Sending it again.");
            SendNextGetTwinAttempt(exchange);
        }

        // Must be called while holding _getTwinExchangesLock.
        private void EndGetTwinExchange(GetTwinExchange exchange)
        {
            if (exchange.IsEnded)
            {
                return;
            }

            exchange.IsEnded = true;
            if (_getTwinExchangesByRequest.TryGetValue(exchange.Request, out GetTwinExchange? current) && current == exchange)
            {
                _getTwinExchangesByRequest.Remove(exchange.Request);
            }

            if (exchange.GetId is Guid getId)
            {
                _getTwinExchangesById.Remove(getId);
            }

            exchange.Timer?.Dispose();

            // Callbacks registered on this token run asynchronously so that they never run while holding the lock.
            _ = exchange.Cancellation.CancelAsync();
        }

        // Twin exchanges can't outlive the connection they were sent on, since IoT hub never sends their responses on a later one.
        private void HandleConnectionLost()
        {
            AbandonOutstandingExchanges(() => new DeviceException("The connection to IoT hub was lost before the response to this twin request arrived. It may be retried once reconnected.")
            {
                Retryability = ErrorRetryability.Retryable,
                IsContained = true,
            });

            lock (_desiredStateLock)
            {
                CancelDesiredResync();
            }
        }

        private void AbandonOutstandingExchanges(Func<Exception> createException)
        {
            List<GetTwinExchange> getTwinExchanges;
            lock (_getTwinExchangesLock)
            {
                getTwinExchanges = _getTwinExchangesByRequest.Values.ToList();
                foreach (GetTwinExchange exchange in getTwinExchanges)
                {
                    EndGetTwinExchange(exchange);
                }
            }

            foreach (GetTwinExchange exchange in getTwinExchanges)
            {
                exchange.Response.TrySetException(createException());
            }

            foreach (Guid requestId in _pendingReportedPropertyUpdateOperations.Keys)
            {
                if (_pendingReportedPropertyUpdateOperations.TryRemove(requestId, out ReportedPatchExchange? exchange))
                {
                    // Complete the response before cancelling, so that a patch that wasn't sent yet surfaces this exception.
                    exchange.Response.TrySetException(createException());
                    exchange.Cancellation.Cancel();
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
                await HandleReceivedPresencePublishAsync(args.Publish);
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

                TwinGetResponse twinGetResponse;
                try
                {
                    twinGetResponse = TwinGetResponse.Parser.ParseFrom(args.Publish.PayloadAsReadOnlySequence);
                }
                catch (InvalidProtocolBufferException e)
                {
                    // If this was the response to an outstanding request, the request is sent again once it times out.
                    Trace.TraceWarning("Received a malformed get twin response. Ignoring it: {0}", e);
                    return;
                }

                GetTwinExchange? exchange;
                lock (_getTwinExchangesLock)
                {
                    if (_getTwinExchangesById.TryGetValue(getResponseCorrelationData.Value, out exchange))
                    {
                        EndGetTwinExchange(exchange);
                    }
                }

                if (exchange == null)
                {
                    await HandleDesiredResyncGetResponseAsync(getResponseCorrelationData.Value, twinGetResponse);
                    return;
                }

                GetTwinRequest request = exchange.Request;
                RecordAuthoritativeVersions(_connection.ConnectionNonce, twinGetResponse.DesiredVersion, twinGetResponse.ReportedVersion);

                DeviceTwin twin;
                try
                {
                    twin = new()
                    {
                        Desired = request.GetDesired
                            && twinGetResponse.HasDesiredPayload
                            ? JsonObject.Parse(twinGetResponse.DesiredPayload.Span)!.AsObject()
                            : null,
                        Reported = request.GetReported
                            && twinGetResponse.HasReportedPayload
                            ? JsonObject.Parse(twinGetResponse.ReportedPayload.Span)!.AsObject()
                            : null,
                        DesiredVersion = twinGetResponse.DesiredVersion,
                        ReportedVersion = twinGetResponse.ReportedVersion,
                    };
                }
                catch (Exception e)
                {
                    exchange.Response.TrySetException(e);
                    return;
                }

                if (request.GetDesired && twinGetResponse.HasDesiredPayload)
                {
                    lock (_desiredStateLock)
                    {
                        // The application fetched the desired properties, so this device's view of them is now at least this version.
                        AdvanceLocalDesiredVersion(twinGetResponse.DesiredVersion);
                    }
                }

                if (request.GetReported && twinGetResponse.HasReportedPayload)
                {
                    AdvanceLocalReportedVersion(twinGetResponse.ReportedVersion);
                }

                exchange.Response.TrySetResult(twin);
            }
            else if (type.Equals("reported-patch-response")
                && GuidExtensions.TryParseBytes(args.Publish.CorrelationData, out Guid? patchResponseCorrelationData)
                && _pendingReportedPropertyUpdateOperations.TryRemove(patchResponseCorrelationData.Value, out ReportedPatchExchange? reportedPatchExchange))
            {
                ReportedPatchResponse reportedPatchResponse = ReportedPatchResponse.Parser.ParseFrom(args.Publish.PayloadAsReadOnlySequence);

                // Whatever the result, the response carries IoT hub's current version of the reported properties.
                RecordAuthoritativeVersions(_connection.ConnectionNonce, 0, reportedPatchResponse.Version);
                if (reportedPatchResponse.Result == Result.Ok)
                {
                    lock (_reportedStateLock)
                    {
                        // Only the device writes its reported properties, so if it held the version before this patch, it now holds
                        // them in full at the patched version. Otherwise, it still doesn't hold them in full.
                        if (_localReportedVersion != 0 && reportedPatchResponse.Version == _localReportedVersion + 1)
                        {
                            AdvanceLocalReportedVersion(reportedPatchResponse.Version);
                        }
                    }
                }

                reportedPatchExchange.Response.TrySetResult(reportedPatchResponse);
            }
            else if (type.Equals("desired-patch"))
            {
                if (!IsFromCurrentConnection(args.Publish, type, out Guid? connectionNonce))
                {
                    return;
                }

                DesiredPatch receivedDesiredPatch = DesiredPatch.Parser.ParseFrom(args.Publish.PayloadAsReadOnlySequence);
                RecordAuthoritativeVersions(connectionNonce, receivedDesiredPatch.Version, 0);

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
                RecordAuthoritativeVersions(connectionNonce, receivedTwinPush.Desired?.Version ?? 0, receivedTwinPush.Reported?.Version ?? 0);
                var twinPushArgs = new TwinPushReceivedEventArgs();
                if (receivedTwinPush.Reported != null)
                {
                    twinPushArgs.Reported = new()
                    {
                        Properties = ParseSectionPayload(receivedTwinPush.Reported.Payload),
                        PropertiesVersion = receivedTwinPush.Reported.Version
                    };
                    SetLocalReportedVersionFromSnapshot(receivedTwinPush.Reported.Version);
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

        private async Task HandleReceivedPresencePublishAsync(MqttPublish publish)
        {
            // The connection client validates and acts on the birth ack's other contents. This client only adopts its twin versions.
            if (!publish.UserProperties.TryGetType(out string? type, out int? typeVersion) || !type.Equals("birth-ack") || typeVersion != 1)
            {
                return;
            }

            if (!IsFromCurrentConnection(publish, type, out Guid? connectionNonce))
            {
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

            RecordAuthoritativeVersions(connectionNonce, birthAck.DesiredVersion, birthAck.ReportedVersion);

            // IoT hub's versions never decrease, so if this device holds a newer version than IoT hub, the version it holds is
            // not valid (for example, the application restored the wrong version, or the twin was recreated).
            if (birthAck.ReportedVersion != 0)
            {
                lock (_reportedStateLock)
                {
                    if (_localReportedVersion > birthAck.ReportedVersion)
                    {
                        Trace.TraceWarning("This device held version {0} of the reported properties, but IoT hub only has version {1}. This device no longer considers itself to hold the reported properties.", _localReportedVersion, birthAck.ReportedVersion);
                        _localReportedVersion = 0;
                        _connection.ReplaceReportedPropertiesVersion(0);
                    }
                }
            }

            DesiredStateOutcome outcome = new();
            if (birthAck.DesiredVersion != 0)
            {
                lock (_desiredStateLock)
                {
                    DiscardDesiredResyncFromPreviousConnection(connectionNonce);
                    if (_localDesiredVersion > birthAck.DesiredVersion)
                    {
                        Trace.TraceWarning("This device held version {0} of the desired properties, but IoT hub only has version {1}. Resyncing the desired properties.", _localDesiredVersion, birthAck.DesiredVersion);
                        _localDesiredVersion = 0;
                        _connection.ReplaceDesiredPropertiesVersion(0);
                        if (_desiredResync == null)
                        {
                            StartDesiredResync(connectionNonce, null, outcome);
                        }
                    }
                }
            }

            await CompleteAsync(outcome);
        }

        private void RecordAuthoritativeVersions(Guid? connectionNonce, ulong desiredVersion, ulong reportedVersion)
        {
            lock (_authoritativeVersionsLock)
            {
                if (_authoritativeVersions == null || _authoritativeVersions.ConnectionNonce != connectionNonce)
                {
                    _authoritativeVersions = new AuthoritativeVersions(connectionNonce);
                }

                // IoT hub's versions never decrease, so an older one is from a message that IoT hub sent earlier.
                _authoritativeVersions.Desired = Math.Max(_authoritativeVersions.Desired, desiredVersion);
                _authoritativeVersions.Reported = Math.Max(_authoritativeVersions.Reported, reportedVersion);
            }
        }

        private ulong? GetAuthoritativeVersion(bool desired)
        {
            lock (_authoritativeVersionsLock)
            {
                if (_authoritativeVersions == null || _authoritativeVersions.ConnectionNonce != _connection.ConnectionNonce)
                {
                    return null;
                }

                ulong version = desired ? _authoritativeVersions.Desired : _authoritativeVersions.Reported;

                // IoT hub's versions are never 0, so 0 means IoT hub has not revealed this section's version yet.
                return version == 0 ? null : version;
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
            AdvanceLocalDesiredVersion(patch.Version);
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
            AdvanceLocalDesiredVersion(version);
            return new TwinPushSection()
            {
                Properties = desiredProperties,
                PropertiesVersion = version,
            };
        }

        // Must be called while holding _desiredStateLock.
        private void AdvanceLocalDesiredVersion(ulong version)
        {
            if (version > _localDesiredVersion)
            {
                _localDesiredVersion = version;
                _connection.ObserveDesiredPropertiesVersion(version);
            }
        }

        private void AdvanceLocalReportedVersion(ulong version)
        {
            lock (_reportedStateLock)
            {
                if (version > _localReportedVersion)
                {
                    _localReportedVersion = version;
                    _connection.ObserveReportedPropertiesVersion(version);
                }
            }
        }

        // A twin push on the current connection carries IoT hub's current reported properties in full, so this device now holds
        // exactly that version, even if it previously claimed to hold a newer (and therefore invalid) one.
        private void SetLocalReportedVersionFromSnapshot(ulong version)
        {
            lock (_reportedStateLock)
            {
                if (version >= _localReportedVersion)
                {
                    AdvanceLocalReportedVersion(version);
                }
                else
                {
                    Trace.TraceWarning("This device held version {0} of the reported properties, but IoT hub pushed version {1}. Adopting version {1}.", _localReportedVersion, version);
                    _localReportedVersion = version;
                    _connection.ReplaceReportedPropertiesVersion(version);
                }
            }
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

        private async Task HandleDesiredResyncGetResponseAsync(Guid getId, TwinGetResponse response)
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

                RecordAuthoritativeVersions(connectionNonce, response.DesiredVersion, response.ReportedVersion);
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

        private sealed class DesiredResync(Guid? connectionNonce)
        {
            public Guid? ConnectionNonce { get; } = connectionNonce;

            public Guid GetId { get; set; }

            public int Attempt { get; set; } = 1;

            public List<DesiredPatch> Buffer { get; } = new();

            public System.Threading.Timer? Timer { get; set; }
        }

        private sealed class AuthoritativeVersions(Guid? connectionNonce)
        {
            public Guid? ConnectionNonce { get; } = connectionNonce;

            public ulong Desired { get; set; }

            public ulong Reported { get; set; }
        }

        private sealed class DesiredStateOutcome
        {
            public List<EventArgs> Deliveries { get; } = new();

            public Guid? ResyncGetId { get; set; }
        }

        private readonly record struct GetTwinRequest(bool GetDesired, bool GetReported, ulong IfNotMatchDesired, ulong IfNotMatchReported);

        // A get twin request that is outstanding until IoT hub responds, the connection is lost or nobody is waiting for it.
        private sealed class GetTwinExchange(GetTwinRequest request)
        {
            public GetTwinRequest Request { get; } = request;

            public TaskCompletionSource<DeviceTwin> Response { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);

            // Cancelled when the exchange ends, so that an attempt still waiting to be sent is never sent.
            public CancellationTokenSource Cancellation { get; } = new();

            // The ID of the latest attempt.
            public Guid? GetId { get; set; }

            public int Attempt { get; set; } = 1;

            public int WaiterCount { get; set; }

            public bool IsEnded { get; set; }

            public System.Threading.Timer? Timer { get; set; }
        }

        private sealed class ReportedPatchExchange
        {
            public TaskCompletionSource<ReportedPatchResponse> Response { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);

            // Cancelled when the connection is lost, so that a patch still waiting to be sent is never sent.
            public CancellationTokenSource Cancellation { get; } = new();
        }

        private void ReleaseResources()
        {
            _connection.PublishReceivedAsync -= HandleReceivedMqttPublish;
            _connection.ConnectionLost -= HandleConnectionLost;
            lock (_desiredStateLock)
            {
                CancelDesiredResync();
            }

            AbandonOutstandingExchanges(() => new ObjectDisposedException(nameof(TwinClient)));
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
