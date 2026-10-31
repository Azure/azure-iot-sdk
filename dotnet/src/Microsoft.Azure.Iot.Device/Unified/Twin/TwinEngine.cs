// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Google.Protobuf;
using Microsoft.Azure.Iot.Device.Exceptions;
using Microsoft.Azure.Iot.Device.MQTTv5;
using Microsoft.Azure.Iot.Device.MQTTv5.Connection;
using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Models.Twin;
using Microsoft.Azure.Iot.Device.Mqtt;
using System.Collections.Concurrent;
using System.Diagnostics;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace Microsoft.Azure.Iot.Device.Unified.Twin
{
    /// <summary>
    /// The engine behind the unified <see cref="TwinClient"/> and the MQTTv5 <see cref="MQTTv5.Twin.TwinClient"/>, which keeps
    /// this device's copy of its twin in sync with IoT hub.
    /// </summary>
    /// <remarks>
    /// See TwinEngine.Classic.cs for how MQTTv3 IoT hubs differ.
    /// </remarks>
    internal sealed partial class TwinEngine : IDisposable
    {
        private bool _isDisposed = false;

        private const string ProtobufContentType = "application/protobuf";

        private IConnectionClient _connection;

        // The connection client substitutes this device's current ID for the placeholder when it sends each request, so a
        // pending request still goes to the right topic if the device ID changes (for example, after re-provisioning).
        private const string AzureEventGridOutgoingTwinPublishTopic = "ih/" + IConnectionClient.DeviceIdPlaceholder + "/srv/twin";
        private const string AzureEventGridIncomingTwinPublishTopic = "ih/" + IConnectionClient.DeviceIdPlaceholder + "/dev/twin";
        private const string AzureEventGridIncomingPresencePublishTopic = "ih/" + IConnectionClient.DeviceIdPlaceholder + "/dev/presence";

        // Guards _getTwinExchangesByRequest, _getTwinExchangesById and _getTwinLostConnectionNonce.
        private readonly object _getTwinExchangesLock = new();

        // The outstanding get twin exchange for each distinct get twin request. Identical concurrent requests share one exchange.
        private readonly Dictionary<GetTwinRequest, GetTwinExchange> _getTwinExchangesByRequest = new();

        // The outstanding get twin exchanges, keyed by the ID of their latest attempt. Responses to earlier attempts are dropped.
        private readonly Dictionary<Guid, GetTwinExchange> _getTwinExchangesById = new();

        // The connection nonce of the latest connection that was lost, as far as get twin exchanges know.
        private Guid? _getTwinLostConnectionNonce;

        // Guards the reported patch state below.
        private readonly object _reportedPatchLock = new();

        // The application's reported property patches that IoT hub hasn't applied yet, in the order they were made. These
        // survive disconnects. The attempt in flight (if any) carries a prefix of them, combined into one patch.
        private readonly List<PendingReportedPatch> _pendingReportedPatches = new();

        // The one reported patch attempt in flight, if any.
        private ReportedPatchAttempt? _reportedPatchAttempt;

        // The defensive timeout attempt (starting at 1) for the next reported patch attempt. It escalates each time an attempt
        // times out, and resets once a response arrives or the connection is lost.
        private int _reportedPatchTimeoutAttempt = 1;

        // The connection nonce of the latest birth ack that this client observed.
        private Guid? _birthAckConnectionNonce;

        // The connection nonce of the latest connection that was lost. Reported patches must not be sent on it again, since they
        // would carry if-match versions from before the connection was lost.
        private Guid? _lostConnectionNonce;

        // The maximum number of desired patches buffered while resyncing the desired properties.
        internal const int DesiredResyncBufferCapacity = 8;

        // The largest reported properties patch that combining patches can produce. IoT hub limits the reported properties to
        // 32 KiB, so a larger patch may be rejected even if each patch it combines would be accepted on its own.
        internal const int MaxCoalescedReportedPatchSize = 32 * 1024;

        // Guards the device's view of the desired properties (the desired section state machine).
        private readonly object _desiredStateLock = new();

        // The version of the desired properties that this device has most recently observed. 0 means the device has not
        // observed the desired properties yet. Unlike the resync state, this survives disconnects.
        private ulong _localDesiredVersion;

        // Guards _localReportedVersion.
        private readonly object _reportedStateLock = new();

        // The version of the reported properties that this device holds in full. 0 means the device does not hold them.
        private ulong _localReportedVersion;

        // Guards this device's copy of its twin, which TwinState snapshots are made from. Writes to _localDesiredVersion and
        // _localReportedVersion also take it, so that each version always matches its properties. It is always taken last.
        private readonly object _twinStateLock = new();

        // The desired properties as of _localDesiredVersion.
        private JsonObject _desiredProperties = new();

        // The reported properties that IoT hub has applied, as of _localReportedVersion.
        private JsonObject _reportedProperties = new();

        // Whether this device's copy of its twin (including its pending reported patches) changed since TwinStateChanged was last raised.
        private bool _isTwinStateChanged;

        // Guarded by _reportedPatchLock. The ReportedPatchCompleted events that are yet to be raised, in order.
        private readonly Queue<ReportedPatchCompletedEventArgs> _reportedPatchCompletions = new();

        // Held while raising TwinStateChanged and ReportedPatchCompleted, so that they are raised one at a time, in order.
        private readonly object _twinEventsLock = new();

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
        /// How long to wait for the response to the given consecutive attempt (starting at 1) of a reported properties patch
        /// before sending it again.
        /// </summary>
        internal Func<int, TimeSpan> ReportedPatchTimeoutSchedule { get; set; } = DefensiveTimeoutSchedule.GetTimeout;

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
        /// An event that executes whenever this device's copy of its twin changes, with a snapshot of it to persist.
        /// </summary>
        /// <remarks>
        /// <para>
        /// The SDK doesn't persist the twin. An application that persists each snapshot, and passes the latest one to
        /// <see cref="TwinEngine(IConnectionClient, TwinState?)"/> when it starts again, only receives the twin sections that
        /// changed while it was stopped, and its reported properties patches that IoT hub hadn't applied are sent again.
        /// </para>
        /// <para>
        /// Snapshots are raised one at a time, in the order the changes were made, so the latest one raised is always the
        /// latest state. Changes made by a handler of this event (or of <see cref="ReportedPatchCompleted"/>) are raised once
        /// that handler returns. The snapshot belongs to the handler, which may keep it.
        /// </para>
        /// </remarks>
        public event Action<TwinState>? TwinStateChanged;

        /// <summary>
        /// An event that executes whenever IoT hub responds to a reported properties patch that this client sent, or the patch
        /// couldn't be sent. Raised in the order the patches were sent.
        /// </summary>
        /// <remarks>
        /// Unlike awaiting <see cref="UpdateReportedPropertiesCoreAsync"/>, this is raised for every patch, including those made by
        /// <see cref="EnqueueReportedPropertiesUpdate"/> or restored from a <see cref="TwinState"/>.
        /// </remarks>
        public event Action<ReportedPatchCompletedEventArgs>? ReportedPatchCompleted;

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
        /// (re)connection, and in every twin push, get twin response and reported properties update response. This client
        /// makes each reported properties patch conditional on this version. This value is reset whenever the device reconnects.
        /// </remarks>
        public ulong? AuthoritativeReportedPropertiesVersion => GetAuthoritativeVersion(desired: false);

        /// <summary>
        /// Construct a new <see cref="TwinEngine"/> instance that starts from a copy of the twin that the application persisted.
        /// </summary>
        /// <param name="connection">The connection client this engine will use. It must not be connected yet.</param>
        /// <param name="initialState">
        /// The latest snapshot raised by <see cref="TwinStateChanged"/> before the application last stopped, or null if the
        /// device holds no copy of its twin.
        /// </param>
        /// <remarks>
        /// Reported properties patches are held until the device connects. The versions in <paramref name="initialState"/>
        /// replace any initial versions in the connection client's options, so that IoT hub only pushes (or, for MQTTv3, the
        /// connect sync only raises) the twin sections that changed. Its pending reported properties patches are sent, in order,
        /// before any made afterwards. Their outcomes are raised by <see cref="ReportedPatchCompleted"/>.
        /// </remarks>
        public TwinEngine(IConnectionClient connection, TwinState? initialState)
        {
            _connection = connection;
            _localDesiredVersion = connection.InitialDesiredPropertiesVersion;
            _localReportedVersion = connection.InitialReportedPropertiesVersion;
            if (initialState != null)
            {
                _localDesiredVersion = initialState.DesiredVersion;
                _desiredProperties = initialState.Desired.DeepClone().AsObject();
                _localReportedVersion = initialState.ReportedVersion;
                _reportedProperties = initialState.Reported.DeepClone().AsObject();
                _connection.ReplaceDesiredPropertiesVersion(initialState.DesiredVersion);
                _connection.ReplaceReportedPropertiesVersion(initialState.ReportedVersion);
                foreach (JsonObject patch in initialState.PendingReportedPatches)
                {
                    _pendingReportedPatches.Add(new PendingReportedPatch(patch.DeepClone().AsObject(), hasWaiter: false));
                }
            }

            _connection.PublishReceivedAsync += HandleReceivedMqttPublish;
            _connection.ConnectionLost += HandleConnectionLost;
            _connection.ClassicDevicePresenceEstablished += HandleClassicDevicePresenceEstablished;

            if (_connection.IsClassicDevicePresenceEstablished)
            {
                // This client missed the start of the current connection to an MQTTv3 IoT hub, so catch up with it now.
                HandleClassicDevicePresenceEstablished();
            }
        }

        /// <summary>
        /// Get a snapshot of this device's copy of its twin, as last raised by <see cref="TwinStateChanged"/> (or changed since).
        /// </summary>
        public TwinState GetTwinState()
        {
            lock (_reportedPatchLock)
            {
                lock (_twinStateLock)
                {
                    return CreateTwinState();
                }
            }
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
        /// connection to IoT hub was lost after the request was sent but before the response arrived. The request may be
        /// retried once reconnected. A request made while the device is reconnecting is sent once it has reconnected.
        /// </exception>
        public async Task<DeviceTwin> GetTwinAsync(bool getReported = true, bool getDesired = true, ulong ifNotMatchReported = 0, ulong ifNotMatchDesired = 0, CancellationToken cancellationToken = default)
        {
            ObjectDisposedException.ThrowIf(_isDisposed, typeof(TwinClient));
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
        /// <param name="reportedProperties">The patch of the reported properties to send to IoT hub</param>
        /// <param name="cancellationToken">
        /// The cancellation token. Cancelling stops waiting for the response. The patch is discarded if this client hasn't
        /// started sending it yet. Otherwise, IoT hub may already have applied it, so it stays pending until IoT hub responds.
        /// </param>
        /// <returns>
        /// IoT hub's response to the patch that carried these changes: <see cref="Result.Ok"/> with the new version of the
        /// reported properties, or the reason why IoT hub rejected the changes.
        /// </returns>
        /// <remarks>
        /// <para>
        /// This client sends one reported properties patch at a time, and makes it conditional on IoT hub's current version
        /// of the reported properties so that IoT hub applies each change at most once. Patches made while another patch is
        /// in flight (or while the device is reconnecting) are combined, in order, into the next patch that this client sends,
        /// so they share its response. Patches that can't be combined into one JSON merge patch (such as one that removes a
        /// property followed by one that sets some of that property's children), or whose combination would exceed 32 KiB,
        /// are sent one after another.
        /// </para>
        /// <para>
        /// If IoT hub's version of the reported properties has moved on (for example, because IoT hub applied an earlier attempt
        /// whose response was lost), this client sends the patch again against IoT hub's version. If IoT hub doesn't respond
        /// within a defensive timeout (5 to 15 minutes, growing with each attempt), the patch is sent again. If the
        /// connection is lost, the patch is sent again once the device has reconnected and IoT hub has acknowledged its presence.
        /// </para>
        /// <para>
        /// The device doesn't need to be connected to call this method. Patches made before the device connects (or while it is
        /// disconnected) are held, in order, and sent once the device has connected and IoT hub has acknowledged its presence.
        /// Patches are only discarded if <paramref name="cancellationToken"/> is cancelled before this client starts sending
        /// them, or if this
        /// client is disposed.
        /// </para>
        /// </remarks>
        /// <exception cref="DeviceException">Thrown if the patch could not be sent.</exception>
        /// <exception cref="ObjectDisposedException">Thrown if this client is disposed before IoT hub responds.</exception>
        internal async Task<ReportedPatchResponse> UpdateReportedPropertiesCoreAsync(JsonObject reportedProperties, CancellationToken cancellationToken)
        {
            ObjectDisposedException.ThrowIf(_isDisposed, typeof(TwinClient));
            ArgumentNullException.ThrowIfNull(reportedProperties);
            cancellationToken.ThrowIfCancellationRequested();

            PendingReportedPatch pendingPatch = AddPendingReportedPatch(reportedProperties, hasWaiter: true);

            try
            {
                return await pendingPatch.Response!.Task.WaitAsync(cancellationToken).ConfigureAwait(false);
            }
            catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
            {
                lock (_reportedPatchLock)
                {
                    // A patch that any attempt carried may already be applied, even if that attempt ended (for example, the
                    // connection was lost before its response arrived), so it stays until IoT hub responds to it.
                    if (!pendingPatch.IsSendStarted && _pendingReportedPatches.Remove(pendingPatch))
                    {
                        MarkTwinStateChanged();
                    }
                }

                RaiseTwinEvents();
                throw;
            }
        }

        /// <summary>
        /// Queue an update to this device's reported properties, without waiting for IoT hub to apply it.
        /// </summary>
        /// <param name="reportedProperties">The patch of the reported properties to send to IoT hub.</param>
        /// <remarks>
        /// <para>
        /// The patch is sent exactly as by <see cref="UpdateReportedPropertiesCoreAsync"/>, including when the device isn't
        /// connected yet, and it is sent after every patch made before it. Its outcome is raised by
        /// <see cref="ReportedPatchCompleted"/>.
        /// </para>
        /// <para>
        /// The patch is part of the <see cref="TwinState"/> raised by <see cref="TwinStateChanged"/> before this method returns,
        /// so an application that persists that state doesn't lose the patch if it stops before IoT hub applies it.
        /// </para>
        /// </remarks>
        public void EnqueueReportedPropertiesUpdate(JsonObject reportedProperties)
        {
            ObjectDisposedException.ThrowIf(_isDisposed, typeof(TwinClient));
            ArgumentNullException.ThrowIfNull(reportedProperties);
            AddPendingReportedPatch(reportedProperties, hasWaiter: false);
        }

        private PendingReportedPatch AddPendingReportedPatch(JsonObject reportedProperties, bool hasWaiter)
        {
            // Copy the patch so that later changes the application makes to it don't affect what is sent.
            PendingReportedPatch pendingPatch = new(reportedProperties.DeepClone().AsObject(), hasWaiter);
            ReportedPatchAttempt? attempt;
            lock (_reportedPatchLock)
            {
                ObjectDisposedException.ThrowIf(_isDisposed, typeof(TwinClient));
                _pendingReportedPatches.Add(pendingPatch);
                MarkTwinStateChanged();
                attempt = TryStartReportedPatchAttempt();
            }

            RaiseTwinEvents();
            SendReportedPatchAttempt(attempt);
            return pendingPatch;
        }

        // Must be called while holding _reportedPatchLock. Starts the next reported patch attempt from the pending patches, unless
        // one is already in flight, there are none, the device isn't connected, or IoT hub's version of the reported properties
        // is not known yet. Patches held while the device isn't connected are sent once IoT hub acknowledges its presence (or,
        // for an MQTTv3 IoT hub, once this client has fetched the twin on the current connection).
        private ReportedPatchAttempt? TryStartReportedPatchAttempt()
        {
            if (_isDisposed || _reportedPatchAttempt != null || _pendingReportedPatches.Count == 0 || !IsConnected())
            {
                return null;
            }

            // Each connection's birth ack reveals IoT hub's current version of the reported properties, so wait for it.
            // Connection clients that don't expose a connection nonce don't announce this device's presence at all.
            Guid? connectionNonce = _connection.ConnectionNonce;
            if (connectionNonce != null && !HasBirthAck(connectionNonce.Value))
            {
                return null;
            }

            // MQTTv3 IoT hubs don't support if-match. Otherwise, if IoT hub didn't reveal its version, an if-match of 0 makes
            // this patch unconditional.
            ulong ifMatch = IsClassicConnection() ? 0 : GetAuthoritativeVersion(desired: false) ?? 0;

            JsonObject payload = _pendingReportedPatches[0].Patch.DeepClone().AsObject();
            byte[] payloadBytes = JsonSerializer.SerializeToUtf8Bytes(payload);
            int patchCount = 1;
            while (patchCount < _pendingReportedPatches.Count)
            {
                JsonObject combined = payload.DeepClone().AsObject();
                if (!TryCombineMergePatches(combined, _pendingReportedPatches[patchCount].Patch))
                {
                    break;
                }

                // Patches that IoT hub would accept one at a time could be rejected as one that is too large, so the rest are
                // sent separately.
                byte[] combinedBytes = JsonSerializer.SerializeToUtf8Bytes(combined);
                if (combinedBytes.Length > MaxCoalescedReportedPatchSize)
                {
                    break;
                }

                payload = combined;
                payloadBytes = combinedBytes;
                patchCount++;
            }

            ReportedPatchAttempt attempt = new(
                Guid.NewGuid(),
                ifMatch,
                _pendingReportedPatches.GetRange(0, patchCount),
                payloadBytes);
            foreach (PendingReportedPatch pendingPatch in attempt.Patches)
            {
                pendingPatch.IsSendStarted = true;
            }
            attempt.Timer = new System.Threading.Timer(
                _ => HandleReportedPatchTimeout(attempt),
                null,
                ReportedPatchTimeoutSchedule(_reportedPatchTimeoutAttempt),
                Timeout.InfiniteTimeSpan);
            _reportedPatchAttempt = attempt;
            return attempt;
        }

        // Must be called while holding _reportedPatchLock.
        private bool HasBirthAck(Guid connectionNonce)
        {
            if (connectionNonce == _lostConnectionNonce)
            {
                return false;
            }

            return _birthAckConnectionNonce == connectionNonce || _connection.LatestBirthAck?.ConnectionNonce == connectionNonce;
        }

        // Must be called while holding _reportedPatchLock. The patches it carried stay pending unless the caller removes them.
        private void EndReportedPatchAttempt(ReportedPatchAttempt attempt)
        {
            _reportedPatchAttempt = null;
            attempt.Timer?.Dispose();

            // Callbacks registered on this token run asynchronously so that they never run while holding the lock.
            _ = attempt.Cancellation.CancelAsync();
        }

        // Combines a JSON merge patch (RFC 7396) into another, in place, so that it has the same effect as applying both in order.
        // Returns false if no single merge patch has that effect. The first patch may be partially modified in that case.
        private static bool TryCombineMergePatches(JsonObject first, JsonObject second)
        {
            foreach (KeyValuePair<string, JsonNode?> property in second)
            {
                if (property.Value is JsonObject secondChild)
                {
                    if (!first.TryGetPropertyValue(property.Key, out JsonNode? firstValue))
                    {
                        first[property.Key] = secondChild.DeepClone();
                    }
                    else if (firstValue is JsonObject firstChild)
                    {
                        if (!TryCombineMergePatches(firstChild, secondChild))
                        {
                            return false;
                        }
                    }
                    else
                    {
                        // The first patch replaces or removes this property, and the second merges into what the first left. If
                        // the property was an object to begin with, a single merge patch would merge into that object instead.
                        return false;
                    }
                }
                else
                {
                    first[property.Key] = property.Value?.DeepClone();
                }
            }

            return true;
        }

        private void SendReportedPatchAttempt(ReportedPatchAttempt? attempt)
        {
            if (attempt != null)
            {
                _ = PublishReportedPatchAttemptAsync(attempt);
            }
        }

        private async Task PublishReportedPatchAttemptAsync(ReportedPatchAttempt attempt)
        {
            MqttPublish publish = IsClassicConnection() ? CreateClassicReportedPatchPublish(attempt) : CreateReportedPatchPublish(attempt);

            try
            {
                Trace.TraceInformation("Publishing 'PatchReported' request on topic " + publish.Topic);

                // The connection client returns the PUBACK as-is, so a rejected request must be surfaced here.
                MqttPublishAck puback = await _connection.PublishAsync(publish, attempt.Cancellation.Token);
                MqttAckValidator.ThrowIfPubackHasErrorCode(puback);
            }
            catch (OperationCanceledException) when (attempt.Cancellation.IsCancellationRequested)
            {
                // The attempt already ended (for example, the connection was lost), so it must not be sent.
            }
            catch (Exception e) when (IsTransientPublishFailure(e))
            {
                // For example, the connection dropped while this QoS 0 request was being written, possibly before this client
                // learns that the connection was lost. Like a request that IoT hub never received, the attempt stays in flight:
                // it is sent again after the next connect sync once the connection loss arrives, or when it times out otherwise.
                Trace.TraceWarning("Failed to publish a reported properties patch. It will be sent again: {0}", e);
            }
            catch (Exception e)
            {
                ReportedPatchAttempt? nextAttempt;
                lock (_reportedPatchLock)
                {
                    if (_reportedPatchAttempt != attempt)
                    {
                        return;
                    }

                    EndReportedPatchAttempt(attempt);
                    _pendingReportedPatches.RemoveRange(0, attempt.Patches.Count);
                    MarkTwinStateChanged();
                    _reportedPatchCompletions.Enqueue(new ReportedPatchCompletedEventArgs() { Patch = ParseAttemptPatch(attempt), Error = e });
                    nextAttempt = TryStartReportedPatchAttempt();
                }

                foreach (PendingReportedPatch pendingPatch in attempt.Patches)
                {
                    pendingPatch.Response?.TrySetException(e);
                }

                RaiseTwinEvents();

                SendReportedPatchAttempt(nextAttempt);
            }
        }

        private static bool IsTransientPublishFailure(Exception e)
        {
            return e is MqttClientNotConnectedException or OperationCanceledException
                || (e is DeviceException deviceException && deviceException.Retryability == ErrorRetryability.Retryable);
        }

        private MqttPublish CreateReportedPatchPublish(ReportedPatchAttempt attempt)
        {
            MqttPublish publish = new MqttPublish()
            {
                Topic = AzureEventGridOutgoingTwinPublishTopic,
                QualityOfServiceLevel = MqttQualityOfServiceLevel.AtMostOnce,
                CorrelationData = attempt.PatchId.ToByteArray(bigEndian: true),
                Payload = new ReportedPatch()
                {
                    IfMatch = attempt.IfMatch, // If this value is 0, the if-match field will not be included in the serialized payload and the request is treated as unconditional
                    Payload = ByteString.CopyFrom(attempt.Payload),
                }.ToByteArray(),
                ContentType = ProtobufContentType,
            };

            publish.UserProperties.Add(new("type", Encoding.UTF8.GetBytes("reported-patch:1")));
            return publish;
        }

        private void HandleReportedPatchTimeout(ReportedPatchAttempt attempt)
        {
            ReportedPatchAttempt? nextAttempt;
            lock (_reportedPatchLock)
            {
                if (_isDisposed || _reportedPatchAttempt != attempt)
                {
                    return;
                }

                EndReportedPatchAttempt(attempt);
                _reportedPatchTimeoutAttempt++;
                nextAttempt = TryStartReportedPatchAttempt();
            }

            Trace.TraceWarning("Timed out waiting for the response to a reported properties patch. Sending it again.");
            SendReportedPatchAttempt(nextAttempt);
        }

        private void HandleReportedPatchResponse(Guid patchId, ReportedPatchResponse response)
        {
            ReportedPatchAttempt? nextAttempt;
            List<PendingReportedPatch>? resolvedPatches = null;
            lock (_reportedPatchLock)
            {
                ReportedPatchAttempt? attempt = _reportedPatchAttempt;
                if (attempt == null || attempt.PatchId != patchId)
                {
                    // A response to an earlier attempt. The latest attempt carries the same changes.
                    Trace.TraceInformation("Dropping a reported properties patch response that does not match the reported properties patch in flight.");
                    return;
                }

                // Whatever the result, the response carries IoT hub's current version of the reported properties.
                RecordAuthoritativeVersions(_connection.ConnectionNonce, 0, response.Version);
                EndReportedPatchAttempt(attempt);
                _reportedPatchTimeoutAttempt = 1;

                // IoT hub rejects a stale if-match with its current version, which the next attempt uses instead. If IoT hub's
                // version matches the if-match, retrying would be rejected the same way, so give up instead.
                if (response.Result == Result.VersionMismatch && response.Version != attempt.IfMatch)
                {
                    Trace.TraceInformation("IoT hub's version of the reported properties is {0}, not {1}. Sending the reported properties patch again.", response.Version, attempt.IfMatch);
                }
                else
                {
                    _pendingReportedPatches.RemoveRange(0, attempt.Patches.Count);
                    resolvedPatches = attempt.Patches;
                    if (response.Result == Result.Ok)
                    {
                        lock (_reportedStateLock)
                        {
                            lock (_twinStateLock)
                            {
                                // IoT hub applied these patches in order, so this device's copy of the reported properties has them too.
                                foreach (PendingReportedPatch pendingPatch in attempt.Patches)
                                {
                                    ApplyMergePatch(_reportedProperties, pendingPatch.Patch);
                                }
                            }

                            // Only the device writes its reported properties, so if it held the version before this patch, it now holds
                            // them in full at the patched version. If it already holds this version or a newer one (for example, from a
                            // get twin response that arrived first), its copy already has this patch. Otherwise (for example, IoT hub
                            // also applied an earlier attempt whose response was lost), it can't tell which version its copy matches, so
                            // it no longer holds them in full. The next connect sync or get twin request brings its copy up to date.
                            if (_localReportedVersion != 0 && response.Version == _localReportedVersion + 1)
                            {
                                AdvanceLocalReportedVersion(response.Version);
                            }
                            else if (_localReportedVersion != 0 && (response.Version == 0 || response.Version > _localReportedVersion + 1))
                            {
                                SetLocalReportedState(0, null);
                            }
                        }
                    }

                    MarkTwinStateChanged();
                    _reportedPatchCompletions.Enqueue(new ReportedPatchCompletedEventArgs() { Patch = ParseAttemptPatch(attempt), Response = response });
                }

                nextAttempt = TryStartReportedPatchAttempt();
            }

            if (resolvedPatches != null)
            {
                foreach (PendingReportedPatch pendingPatch in resolvedPatches)
                {
                    pendingPatch.Response?.TrySetResult(response);
                }
            }

            RaiseTwinEvents();
            SendReportedPatchAttempt(nextAttempt);
        }

        private static JsonObject ParseAttemptPatch(ReportedPatchAttempt attempt)
        {
            return JsonNode.Parse(attempt.Payload)!.AsObject();
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
                exchange.Timer = null;
            }

            // The connection client may hold the publish until the device has reconnected, so it is sent off this thread. That
            // way, a caller can stop waiting for the response.
            _ = Task.Run(() => PublishGetTwinAsync(exchange, getId));
        }

        private async Task PublishGetTwinAsync(GetTwinExchange exchange, Guid getId)
        {
            GetTwinRequest request = exchange.Request;
            MqttPublish publish = IsClassicConnection()
                ? CreateClassicGetTwinPublish(getId)
                : CreateGetTwinPublish(
                    getId,
                    request.GetReported ? request.GetDesired ? Sections.Both : Sections.Reported : Sections.Desired,
                    request.IfNotMatchDesired,
                    request.IfNotMatchReported);

            try
            {
                Trace.TraceInformation("Publishing 'GetTwin' request on topic " + publish.Topic);

                // The connection client returns the PUBACK as-is, so a rejected request must be surfaced here.
                MqttPublishAck puback = await _connection.PublishAsync(publish, exchange.Cancellation.Token);
                MqttAckValidator.ThrowIfPubackHasErrorCode(puback);
                lock (_getTwinExchangesLock)
                {
                    // For example, the response already arrived.
                    if (exchange.IsEnded || exchange.GetId != getId)
                    {
                        return;
                    }

                    // The request went out on the current connection or an earlier one. If the current one was already lost,
                    // HandleConnectionLost may have run before this attempt counted as sent, so its response can never arrive.
                    Guid? connectionNonce = _connection.ConnectionNonce;
                    if (connectionNonce == null || connectionNonce != _getTwinLostConnectionNonce)
                    {
                        exchange.SentGetId = getId;

                        // Only once sent, so that a request that is waiting for the device to reconnect isn't sent again.
                        exchange.Timer = new System.Threading.Timer(
                            _ => HandleGetTwinTimeout(exchange, getId),
                            null,
                            GetTwinTimeoutSchedule(exchange.Attempt),
                            Timeout.InfiniteTimeSpan);
                        return;
                    }

                    EndGetTwinExchange(exchange);
                }

                exchange.Response.TrySetException(CreateGetTwinConnectionLostException());
            }
            catch (OperationCanceledException) when (exchange.Cancellation.IsCancellationRequested)
            {
                // The exchange already ended.
            }
            catch (Exception e)
            {
                lock (_getTwinExchangesLock)
                {
                    if (exchange.IsEnded || exchange.GetId != getId)
                    {
                        return;
                    }

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
            lock (_getTwinExchangesLock)
            {
                _getTwinLostConnectionNonce = _connection.ConnectionNonce;
            }

            // A request that is still waiting to be sent (for example, one made while reconnecting) is sent on the next
            // connection instead.
            AbandonGetTwinExchanges(
                exchange => exchange.GetId != null && exchange.SentGetId == exchange.GetId,
                CreateGetTwinConnectionLostException);

            // Reported patches are kept and sent again on the next connection. IoT hub may have applied the attempt in flight,
            // in which case the next attempt's if-match is rejected with the version that it was applied at, and so on.
            ReportedPatchAttempt? nextAttempt;
            lock (_reportedPatchLock)
            {
                _lostConnectionNonce = _connection.ConnectionNonce;
                if (_reportedPatchAttempt != null)
                {
                    EndReportedPatchAttempt(_reportedPatchAttempt);
                }

                _reportedPatchTimeoutAttempt = 1;
                nextAttempt = TryStartReportedPatchAttempt();
            }

            SendReportedPatchAttempt(nextAttempt);

            lock (_desiredStateLock)
            {
                CancelDesiredResync();
            }
        }

        private static DeviceException CreateGetTwinConnectionLostException()
        {
            return new DeviceException("The connection to IoT hub was lost before the response to this twin request arrived. It may be retried once reconnected.")
            {
                Retryability = ErrorRetryability.Retryable,
                IsContained = true,
            };
        }

        private void AbandonGetTwinExchanges(Func<GetTwinExchange, bool> shouldAbandon, Func<Exception> createException)
        {
            List<GetTwinExchange> getTwinExchanges;
            lock (_getTwinExchangesLock)
            {
                getTwinExchanges = _getTwinExchangesByRequest.Values.Where(shouldAbandon).ToList();
                foreach (GetTwinExchange exchange in getTwinExchanges)
                {
                    EndGetTwinExchange(exchange);
                }
            }

            foreach (GetTwinExchange exchange in getTwinExchanges)
            {
                exchange.Response.TrySetException(createException());
            }
        }

        private void EnsureConnected()
        {
            if (!IsConnected())
            {
                throw new NotSupportedException("Must be connected before calling this method.");
            }
        }

        private bool IsConnected()
        {
            return _connection.GetCurrentConnectionContext() != null && _connection.IsConnectionTargetSet;
        }

        private async Task HandleReceivedMqttPublish(MqttPublishReceivedEventArgs args)
        {
            try
            {
                await HandleReceivedMqttPublishCoreAsync(args);
            }
            finally
            {
                RaiseTwinEvents();
            }
        }

        private async Task HandleReceivedMqttPublishCoreAsync(MqttPublishReceivedEventArgs args)
        {
            if (args.Publish.Topic.StartsWith(ClassicTwinTopicPrefix, StringComparison.Ordinal))
            {
                await HandleReceivedClassicPublishAsync(args.Publish);
                return;
            }

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

                GetTwinExchange? exchange = TryTakeGetTwinExchange(getResponseCorrelationData.Value);
                if (exchange == null)
                {
                    await HandleDesiredResyncGetResponseAsync(getResponseCorrelationData.Value, twinGetResponse);
                    return;
                }

                HandleGetTwinExchangeResponse(exchange, twinGetResponse);
            }
            else if (type.Equals("reported-patch-response"))
            {
                if (!GuidExtensions.TryParseBytes(args.Publish.CorrelationData, out Guid? patchId))
                {
                    Trace.TraceWarning("Dropping a reported properties patch response without a valid correlation ID.");
                    return;
                }

                ReportedPatchResponse reportedPatchResponse;
                try
                {
                    reportedPatchResponse = ReportedPatchResponse.Parser.ParseFrom(args.Publish.PayloadAsReadOnlySequence);
                }
                catch (InvalidProtocolBufferException e)
                {
                    // The patch is sent again once its response times out.
                    Trace.TraceWarning("Dropping a malformed reported properties patch response: {0}", e);
                    return;
                }

                HandleReportedPatchResponse(patchId.Value, reportedPatchResponse);
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
                    SetLocalReportedVersionFromSnapshot(receivedTwinPush.Reported.Version, twinPushArgs.Reported.Properties);
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

        // Returns the get twin exchange whose latest attempt has this ID, if any, and ends it.
        private GetTwinExchange? TryTakeGetTwinExchange(Guid getId)
        {
            lock (_getTwinExchangesLock)
            {
                if (_getTwinExchangesById.TryGetValue(getId, out GetTwinExchange? exchange))
                {
                    EndGetTwinExchange(exchange);
                }

                return exchange;
            }
        }

        private void HandleGetTwinExchangeResponse(GetTwinExchange exchange, TwinGetResponse twinGetResponse)
        {
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
                    AdvanceLocalDesiredVersion(twinGetResponse.DesiredVersion, twin.Desired!, isPatch: false);
                }
            }

            if (request.GetReported && twinGetResponse.HasReportedPayload)
            {
                AdvanceLocalReportedVersion(twinGetResponse.ReportedVersion, twin.Reported);
            }

            exchange.Response.TrySetResult(twin);
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
                        SetLocalReportedState(0, null);
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
                        lock (_twinStateLock)
                        {
                            _localDesiredVersion = 0;
                            _isTwinStateChanged = true;
                        }

                        _connection.ReplaceDesiredPropertiesVersion(0);
                        if (_desiredResync == null)
                        {
                            StartDesiredResync(connectionNonce, null, outcome);
                        }
                    }
                }
            }

            // IoT hub's version of the reported properties is now known (if it has one), so conditional patches can be sent.
            ReportedPatchAttempt? nextAttempt;
            lock (_reportedPatchLock)
            {
                _birthAckConnectionNonce = connectionNonce;
                nextAttempt = TryStartReportedPatchAttempt();
            }

            SendReportedPatchAttempt(nextAttempt);

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
            // This client may have been created after the current connection's birth ack arrived.
            Guid? connectionNonce = _connection.ConnectionNonce;
            BirthAckVersions? latestBirthAck = _connection.LatestBirthAck;
            if (connectionNonce != null && latestBirthAck?.ConnectionNonce == connectionNonce)
            {
                RecordAuthoritativeVersions(connectionNonce, latestBirthAck.DesiredVersion, latestBirthAck.ReportedVersion);
            }

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

                // Until the connect sync's response arrives, this device's version may not be valid (see
                // ResetLocalDesiredVersionIfInvalid), so patches that look obsolete are buffered too. Those that are still obsolete
                // once the response is adopted are dropped then.
                bool mayResetLocalVersion = _desiredResync.IsConnectSync && _localDesiredVersion == _desiredResync.DesiredVersionAtStart;
                if (patch.Version <= _localDesiredVersion && !mayResetLocalVersion)
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
            AdvanceLocalDesiredVersion(patch.Version, desiredProperties, isPatch: true);
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
            AdvanceLocalDesiredVersion(version, desiredProperties, isPatch: false);
            return new TwinPushSection()
            {
                Properties = desiredProperties,
                PropertiesVersion = version,
            };
        }

        // Must be called while holding _desiredStateLock. The properties are either the desired properties in full at this version,
        // or (if isPatch) the patch that takes them from the previous version to this one.
        private void AdvanceLocalDesiredVersion(ulong version, JsonObject properties, bool isPatch)
        {
            if (version > _localDesiredVersion)
            {
                lock (_twinStateLock)
                {
                    _localDesiredVersion = version;
                    if (isPatch)
                    {
                        ApplyMergePatch(_desiredProperties, properties);
                    }
                    else
                    {
                        _desiredProperties = properties.DeepClone().AsObject();
                    }

                    _isTwinStateChanged = true;
                }

                _connection.ObserveDesiredPropertiesVersion(version);
            }
        }

        // The properties, if any, are the reported properties in full at this version. Otherwise, the caller already updated them.
        private void AdvanceLocalReportedVersion(ulong version, JsonObject? properties = null)
        {
            lock (_reportedStateLock)
            {
                if (version > _localReportedVersion)
                {
                    SetLocalReportedState(version, properties);
                    _connection.ObserveReportedPropertiesVersion(version);
                }
            }
        }

        // A twin push on the current connection carries IoT hub's current reported properties in full, so this device now holds
        // exactly that version, even if it previously claimed to hold a newer (and therefore invalid) one.
        private void SetLocalReportedVersionFromSnapshot(ulong version, JsonObject properties)
        {
            lock (_reportedStateLock)
            {
                if (version >= _localReportedVersion)
                {
                    AdvanceLocalReportedVersion(version, properties);
                }
                else
                {
                    Trace.TraceWarning("This device held version {0} of the reported properties, but IoT hub pushed version {1}. Adopting version {1}.", _localReportedVersion, version);
                    SetLocalReportedState(version, properties);
                    _connection.ReplaceReportedPropertiesVersion(version);
                }
            }
        }

        // Must be called while holding _reportedStateLock.
        private void SetLocalReportedState(ulong version, JsonObject? properties)
        {
            lock (_twinStateLock)
            {
                _localReportedVersion = version;
                if (properties != null)
                {
                    _reportedProperties = properties.DeepClone().AsObject();
                }

                _isTwinStateChanged = true;
            }
        }

        private void MarkTwinStateChanged()
        {
            lock (_twinStateLock)
            {
                _isTwinStateChanged = true;
            }
        }

        // Must be called while holding _reportedPatchLock and _twinStateLock, so that the pending patches and the properties match.
        private TwinState CreateTwinState()
        {
            return new TwinState()
            {
                DesiredVersion = _localDesiredVersion,
                Desired = _desiredProperties.DeepClone().AsObject(),
                ReportedVersion = _localReportedVersion,
                Reported = _reportedProperties.DeepClone().AsObject(),
                PendingReportedPatches = _pendingReportedPatches.Select(pendingPatch => pendingPatch.Patch.DeepClone().AsObject()).ToList(),
            };
        }

        // Applies a JSON merge patch (RFC 7396) to the target, in place.
        private static void ApplyMergePatch(JsonObject target, JsonObject patch)
        {
            foreach (KeyValuePair<string, JsonNode?> property in patch)
            {
                if (property.Value == null)
                {
                    target.Remove(property.Key);
                }
                else if (property.Value is JsonObject patchObject)
                {
                    if (target[property.Key] is not JsonObject targetObject)
                    {
                        targetObject = new JsonObject();
                        target[property.Key] = targetObject;
                    }

                    ApplyMergePatch(targetObject, patchObject);
                }
                else
                {
                    target[property.Key] = property.Value.DeepClone();
                }
            }
        }

        // Raises the twin events that are due, one at a time and in order. Must be called without holding any of this client's
        // other locks, so that handlers can call back into this client. If called from a handler, the outer call raises them.
        private void RaiseTwinEvents()
        {
            if (Monitor.IsEntered(_twinEventsLock))
            {
                return;
            }

            lock (_twinEventsLock)
            {
                while (!_isDisposed)
                {
                    ReportedPatchCompletedEventArgs? completion = null;
                    TwinState? state = null;
                    lock (_reportedPatchLock)
                    {
                        if (!_reportedPatchCompletions.TryDequeue(out completion))
                        {
                            lock (_twinStateLock)
                            {
                                if (_isTwinStateChanged)
                                {
                                    _isTwinStateChanged = false;
                                    state = CreateTwinState();
                                }
                            }
                        }
                    }

                    try
                    {
                        if (completion != null)
                        {
                            ReportedPatchCompleted?.Invoke(completion);
                        }
                        else if (state != null)
                        {
                            TwinStateChanged?.Invoke(state);
                        }
                        else
                        {
                            return;
                        }
                    }
                    catch (Exception e)
                    {
                        Trace.TraceWarning("A twin event handler threw an exception: {0}", e);
                    }
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
            outcome.ResyncCancellation = resync.Cancellation.Token;
        }

        // Must be called while holding _desiredStateLock.
        private void CancelDesiredResync()
        {
            if (_desiredResync != null)
            {
                _desiredResync.Timer?.Dispose();

                // Callbacks registered on this token run asynchronously so that they never run while holding the lock.
                _ = _desiredResync.Cancellation.CancelAsync();
                _desiredResync = null;
            }
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
            DesiredResync? connectSync = null;
            TwinPushReceivedEventArgs connectSyncPush = new();
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
                if (_desiredResync.IsConnectSync)
                {
                    connectSync = _desiredResync;
                    ResetLocalDesiredVersionIfInvalid(connectSync, response);
                }

                TwinPushSection? snapshot = response.HasDesiredPayload
                    ? TryApplyDesiredSnapshot(response.DesiredVersion, response.DesiredPayload)
                    : null;

                if (snapshot != null)
                {
                    connectSyncPush.Desired = snapshot;
                    outcome.Deliveries.Add(connectSyncPush);
                }

                ResolveDesiredResync(connectionNonce, outcome);
            }

            if (connectSync != null)
            {
                CompleteClassicConnectSync(connectSync, response, connectSyncPush, outcome);
            }

            await CompleteAsync(outcome);
        }

        // Raises the events and sends the GET request that the desired section state machine decided on, outside of _desiredStateLock.
        private async Task CompleteAsync(DesiredStateOutcome outcome)
        {
            foreach (EventArgs delivery in outcome.Deliveries)
            {
                try
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
                catch (Exception e)
                {
                    Trace.TraceWarning("A twin event handler threw an exception: {0}", e);
                }
            }

            // Only after the connect sync's events, so that the patches their handlers make are coalesced with the held ones.
            if (outcome.CompletedConnectSync != null)
            {
                StartSendingReportedPatchesAfterConnectSync(outcome.CompletedConnectSync);
            }

            if (outcome.ResyncGetId is Guid getId)
            {
                await SendDesiredResyncGetAsync(getId, outcome.ResyncCancellation);
            }
        }

        private async Task SendDesiredResyncGetAsync(Guid getId, CancellationToken cancellationToken)
        {
            MqttPublish publish = IsClassicConnection()
                ? CreateClassicGetTwinPublish(getId)
                : CreateGetTwinPublish(getId, Sections.Desired, ifNotMatchDesired: 0, ifNotMatchReported: 0);

            try
            {
                Trace.TraceInformation("Publishing 'GetTwin' request on topic " + publish.Topic + " to resync the desired properties");
                MqttPublishAck puback = await _connection.PublishAsync(publish, cancellationToken);
                MqttAckValidator.ThrowIfPubackHasErrorCode(puback);
            }
            catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
            {
                // The resync ended (for example, the connection was lost), so this request must not be sent.
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

        private static MqttPublish CreateGetTwinPublish(Guid getId, Sections sections, ulong ifNotMatchDesired, ulong ifNotMatchReported)
        {
            MqttPublish publish = new MqttPublish()
            {
                Topic = AzureEventGridOutgoingTwinPublishTopic,
                QualityOfServiceLevel = MqttQualityOfServiceLevel.AtMostOnce,
                CorrelationData = getId.ToByteArray(bigEndian: true),
                Payload = new TwinGet()
                {
                    Sections = sections,
                    IfNotMatchDesired = ifNotMatchDesired,
                    IfNotMatchReported = ifNotMatchReported,
                }.ToByteArray(),
                ContentType = ProtobufContentType,
            };

            publish.UserProperties.Add(new("type", Encoding.UTF8.GetBytes("get:1")));
            return publish;
        }

        private sealed class DesiredResync(Guid? connectionNonce)
        {
            public Guid? ConnectionNonce { get; } = connectionNonce;

            public Guid GetId { get; set; }

            public int Attempt { get; set; } = 1;

            public List<DesiredPatch> Buffer { get; } = new();

            public System.Threading.Timer? Timer { get; set; }

            // Whether this is the GET that this client sends upon connecting to an MQTTv3 IoT hub, in place of a twin push.
            public bool IsConnectSync { get; set; }

            // This device's versions of the twin sections when the connect sync started.
            public ulong DesiredVersionAtStart { get; set; }

            public ulong ReportedVersionAtStart { get; set; }

            // Cancelled when the resync ends, so that a GET request still waiting to be sent is never sent.
            public CancellationTokenSource Cancellation { get; } = new();
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

            public CancellationToken ResyncCancellation { get; set; }

            // The connect sync that completed, after whose deliveries this client starts sending reported properties patches.
            public DesiredResync? CompletedConnectSync { get; set; }
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

            // The ID of the latest attempt that was sent. Only sent attempts can lose their response to a connection loss.
            public Guid? SentGetId { get; set; }

            public int Attempt { get; set; } = 1;

            public int WaiterCount { get; set; }

            public bool IsEnded { get; set; }

            public System.Threading.Timer? Timer { get; set; }
        }

        // A reported properties patch that the application made, which is pending until IoT hub responds to an attempt carrying it.
        private sealed class PendingReportedPatch(JsonObject patch, bool hasWaiter)
        {
            public JsonObject Patch { get; } = patch;

            // Guarded by _reportedPatchLock. Whether any attempt carried this patch, in which case IoT hub may have applied it.
            public bool IsSendStarted { get; set; }

            // Null if nobody awaits this patch, so that its failure isn't an unobserved exception.
            public TaskCompletionSource<ReportedPatchResponse>? Response { get; } = hasWaiter ? new(TaskCreationOptions.RunContinuationsAsynchronously) : null;
        }

        // One attempt to send a prefix of the pending reported patches, combined into one patch.
        private sealed class ReportedPatchAttempt(Guid patchId, ulong ifMatch, List<PendingReportedPatch> patches, byte[] payload)
        {
            public Guid PatchId { get; } = patchId;

            public ulong IfMatch { get; } = ifMatch;

            public List<PendingReportedPatch> Patches { get; } = patches;

            public byte[] Payload { get; } = payload;

            // Cancelled when the attempt ends, so that an attempt still waiting to be sent is never sent.
            public CancellationTokenSource Cancellation { get; } = new();

            public System.Threading.Timer? Timer { get; set; }
        }

        private void ReleaseResources()
        {
            _isDisposed = true;
            _connection.PublishReceivedAsync -= HandleReceivedMqttPublish;
            _connection.ConnectionLost -= HandleConnectionLost;
            _connection.ClassicDevicePresenceEstablished -= HandleClassicDevicePresenceEstablished;
            lock (_desiredStateLock)
            {
                CancelDesiredResync();
            }

            List<PendingReportedPatch> pendingPatches;
            lock (_reportedPatchLock)
            {
                if (_reportedPatchAttempt != null)
                {
                    EndReportedPatchAttempt(_reportedPatchAttempt);
                }

                pendingPatches = new(_pendingReportedPatches);
                _pendingReportedPatches.Clear();
            }

            foreach (PendingReportedPatch pendingPatch in pendingPatches)
            {
                pendingPatch.Response?.TrySetException(new ObjectDisposedException(nameof(TwinClient)));
            }

            AbandonGetTwinExchanges(_ => true, () => new ObjectDisposedException(nameof(TwinClient)));
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
