// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Google.Protobuf;
using Microsoft.Azure.Iot.Device.Exceptions;
using Microsoft.Azure.Iot.Device.Models.Twin;
using Microsoft.Azure.Iot.Device.Mqtt;
using Microsoft.Azure.Iot.Device.Unified.Connection;
using System.Diagnostics;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace Microsoft.Azure.Iot.Device.Unified.Twin
{
    /// <summary>
    /// The engine behind <see cref="TwinClient"/>, which keeps this device's copy of its twin in sync with IoT hub.
    /// </summary>
    /// <remarks>
    /// See TwinEngine.Classic.cs for how IoT hub's twin protocol shapes this engine.
    /// </remarks>
    internal sealed partial class TwinEngine : IDisposable
    {
        private bool _isDisposed = false;

        private IConnectionClient _connection;

        // Guards _activeGetTwinFetch, _nextGetTwinFetch, _getTwinLostConnectionNonce and the state of each GetTwinFetch. It may be
        // taken while holding _desiredStateLock, but never the other way around.
        private readonly object _getTwinLock = new();

        // The one GET request that may be out to IoT hub, as the twin protocol recommends. It serves the application's get twin
        // calls and this client's desired properties resync alike. Calls whose request it covers share its response.
        private GetTwinFetch? _activeGetTwinFetch;

        // The calls (and resync) that the active GET doesn't cover, combined into the one GET that is sent once the active one ends.
        private GetTwinFetch? _nextGetTwinFetch;

        // The connection nonce of the latest connection that was lost, as far as GET requests know.
        private Guid? _getTwinLostConnectionNonce;

        // What a desired properties resync requests: the desired properties in full.
        private static readonly GetTwinRequest s_desiredResyncGetTwinRequest = new(GetDesired: true, GetReported: false, IfNotMatchDesired: 0, IfNotMatchReported: 0);

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

        // The connection nonce of the latest connection on which this client completed the connect sync.
        private Guid? _birthAckConnectionNonce;

        // The connection nonce of the latest connection that was lost. Reported patches must not be sent on it again, since
        // they are only sent once the connect sync of the next connection completes.
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
        /// An event that executes whenever this client fetches twin sections from IoT hub that are newer than (or replace) this
        /// device's copy of them.
        /// </summary>
        /// <remarks>
        /// This client fetches the twin upon each connection (the connect sync), and raises this event with the sections that
        /// changed. This event is also raised with just the desired section when this client resynchronizes its desired
        /// properties with IoT hub (for example, after missing a desired properties patch).
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
        /// Construct a new <see cref="TwinEngine"/> instance that starts from a copy of the twin that the application persisted.
        /// </summary>
        /// <param name="connection">The connection client this engine will use.</param>
        /// <param name="initialState">
        /// The latest snapshot raised by <see cref="TwinStateChanged"/> before the application last stopped, or null if the
        /// device holds no copy of its twin.
        /// </param>
        /// <remarks>
        /// The provided connection client does not need to be connected before this constructor is called. However, it must be
        /// connected prior to sending any twin requests, except for reported properties patches, which are held until the device
        /// connects. This device's copy of its twin starts from <paramref name="initialState"/>, so that the connect sync only
        /// raises <see cref="TwinPushReceived"/> with the twin sections that changed. Its pending reported properties patches
        /// are sent, in order, before any made afterwards. Their outcomes are raised by <see cref="ReportedPatchCompleted"/>.
        /// </remarks>
        public TwinEngine(IConnectionClient connection, TwinState? initialState)
        {
            _connection = connection;
            if (initialState != null)
            {
                _localDesiredVersion = initialState.DesiredVersion;
                _desiredProperties = initialState.Desired.DeepClone().AsObject();
                _localReportedVersion = initialState.ReportedVersion;
                _reportedProperties = initialState.Reported.DeepClone().AsObject();
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
                // This client missed the start of the current connection, so catch up with it now.
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
        /// <param name="getReported">Include the reported properties in the returned twin.</param>
        /// <param name="getDesired">Include the desired properties in the returned twin.</param>
        /// <param name="ifNotMatchReported">
        /// If provided, and the version matches or exceeds the version of the reported properties held by the service, then the 
        /// reported properties are not included in the returned twin.
        /// </param>
        /// <param name="ifNotMatchDesired">
        /// If provided, and the version matches or exceeds the version of the desired properties held by the service, then the 
        /// desired properties are not included in the returned twin.
        /// </param>
        /// <param name="cancellationToken">The cancellation token.</param>
        /// <returns>The returned twin.</returns>
        /// <remarks>
        /// <para>
        /// IoT hub always returns the full twin, so this client filters out the sections that weren't requested, or that the
        /// if-not-match versions exclude.
        /// </para>
        /// <para>
        /// This client has at most one GET request out to IoT hub at a time, which it shares with its own desired properties
        /// resync and the GET it sends upon connecting. IoT hub always returns the full twin, so concurrent calls share that
        /// request's response. Each call's twin only includes the sections that it asked for and that its own if-not-match
        /// versions don't exclude. If IoT hub doesn't respond within a defensive timeout (5 to 15 minutes, growing with each
        /// attempt), the request is sent again.
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

            GetTwinWaiter waiter = new(new GetTwinRequest(getDesired, getReported, ifNotMatchDesired, ifNotMatchReported));
            lock (_getTwinLock)
            {
                ObjectDisposedException.ThrowIf(_isDisposed, typeof(TwinClient));
                AddToGetTwinFetch(waiter.Request, waiter, null, isNewAttemptRequired: false);
            }

            try
            {
                return await waiter.Response.Task.WaitAsync(cancellationToken).ConfigureAwait(false);
            }
            finally
            {
                if (!waiter.Response.Task.IsCompleted)
                {
                    lock (_getTwinLock)
                    {
                        RemoveGetTwinWaiter(waiter);
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
        /// This client sends one reported properties patch at a time. Patches made while another patch is in flight (or while
        /// the device is reconnecting) are combined, in order, into the next patch that this client sends, so they share its
        /// response. Patches that can't be combined into one JSON merge patch (such as one that removes a property followed by
        /// one that sets some of that property's children), or whose combination would exceed 32 KiB, are sent one after another.
        /// </para>
        /// <para>
        /// If IoT hub doesn't respond within a defensive timeout (5 to 15 minutes, growing with each attempt), the patch is
        /// sent again. If the connection is lost, the patch is sent again once the device has reconnected and this client has
        /// fetched the twin. IoT hub may have applied the earlier attempt, so each patch is applied at least once.
        /// </para>
        /// <para>
        /// The device doesn't need to be connected to call this method. Patches made before the device connects (or while it is
        /// disconnected) are held, in order, and sent once the device has connected and this client has fetched the twin.
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
        // one is already in flight, there are none, the device isn't connected, or this client hasn't fetched the twin on the
        // current connection yet. Patches held while the device isn't connected are sent once this client has fetched the twin
        // on the current connection.
        private ReportedPatchAttempt? TryStartReportedPatchAttempt()
        {
            if (_isDisposed || _reportedPatchAttempt != null || _pendingReportedPatches.Count == 0 || !IsConnected())
            {
                return null;
            }

            // Each connection's connect sync brings this device's copy of the reported properties up to date, so wait for it.
            // Connection clients that don't expose a connection nonce can't tell connections apart, so don't wait for it.
            Guid? connectionNonce = _connection.ConnectionNonce;
            if (connectionNonce != null && !HasBirthAck(connectionNonce.Value))
            {
                return null;
            }

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

        // Must be called while holding _reportedPatchLock. Whether this client completed the connect sync on this connection.
        private bool HasBirthAck(Guid connectionNonce)
        {
            if (connectionNonce == _lostConnectionNonce)
            {
                return false;
            }

            return _birthAckConnectionNonce == connectionNonce;
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
            MqttPublish publish = CreateClassicReportedPatchPublish(attempt);

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
            List<PendingReportedPatch> resolvedPatches;
            lock (_reportedPatchLock)
            {
                ReportedPatchAttempt? attempt = _reportedPatchAttempt;
                if (attempt == null || attempt.PatchId != patchId)
                {
                    // A response to an earlier attempt. The latest attempt carries the same changes.
                    Trace.TraceInformation("Dropping a reported properties patch response that does not match the reported properties patch in flight.");
                    return;
                }

                EndReportedPatchAttempt(attempt);
                _reportedPatchTimeoutAttempt = 1;

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

                nextAttempt = TryStartReportedPatchAttempt();
            }

            foreach (PendingReportedPatch pendingPatch in resolvedPatches)
            {
                pendingPatch.Response?.TrySetResult(response);
            }

            RaiseTwinEvents();
            SendReportedPatchAttempt(nextAttempt);
        }

        private static JsonObject ParseAttemptPatch(ReportedPatchAttempt attempt)
        {
            return JsonNode.Parse(attempt.Payload)!.AsObject();
        }

        // Must be called while holding _getTwinLock. Adds a call (or the desired properties resync) to the active GET if that GET
        // covers its request, and otherwise to the next GET. If isNewAttemptRequired, the active GET is sent again (now covering
        // this request too) even if an earlier attempt covers it, since the response to that attempt may never arrive.
        private void AddToGetTwinFetch(GetTwinRequest request, GetTwinWaiter? waiter, DesiredResync? resync, bool isNewAttemptRequired)
        {
            GetTwinFetch? active = _activeGetTwinFetch;
            if (active == null)
            {
                active = new GetTwinFetch(request);
                AttachToGetTwinFetch(active, waiter, resync);
                _activeGetTwinFetch = active;
                StartGetTwinAttempt(active);
            }
            else if (active.IsFailed || isNewAttemptRequired)
            {
                // A failed GET only waits for its timeout so that the resync isn't retried in a tight loop, so a new call
                // retries it straight away.
                active.Request = CombineGetTwinRequests(active.Request, request);
                AttachToGetTwinFetch(active, waiter, resync);
                StartGetTwinAttempt(active);
            }
            else if (CoversGetTwinRequest(active.Request, request))
            {
                AttachToGetTwinFetch(active, waiter, resync);
            }
            else
            {
                if (_nextGetTwinFetch == null)
                {
                    _nextGetTwinFetch = new GetTwinFetch(request);
                }
                else
                {
                    _nextGetTwinFetch.Request = CombineGetTwinRequests(_nextGetTwinFetch.Request, request);
                }

                AttachToGetTwinFetch(_nextGetTwinFetch, waiter, resync);
            }
        }

        private static void AttachToGetTwinFetch(GetTwinFetch fetch, GetTwinWaiter? waiter, DesiredResync? resync)
        {
            if (waiter != null)
            {
                waiter.Fetch = fetch;
                fetch.Waiters.Add(waiter);
            }

            if (resync != null)
            {
                fetch.Resync = resync;
            }
        }

        // Whether the response to a GET with fetchRequest also answers request. IoT hub always returns the full twin, so it
        // always does.
        private static bool CoversGetTwinRequest(GetTwinRequest fetchRequest, GetTwinRequest request)
        {
            return true;
        }

        // The request whose response answers both requests: it asks for every section that either asks for, with an
        // if-not-match version only where both ask for that section with the same one.
        private static GetTwinRequest CombineGetTwinRequests(GetTwinRequest first, GetTwinRequest second)
        {
            return new GetTwinRequest(
                first.GetDesired || second.GetDesired,
                first.GetReported || second.GetReported,
                CombineIfNotMatch(first.GetDesired, first.IfNotMatchDesired, second.GetDesired, second.IfNotMatchDesired),
                CombineIfNotMatch(first.GetReported, first.IfNotMatchReported, second.GetReported, second.IfNotMatchReported));

            static ulong CombineIfNotMatch(bool firstGets, ulong firstIfNotMatch, bool secondGets, ulong secondIfNotMatch)
            {
                if (!firstGets || !secondGets)
                {
                    return firstGets ? firstIfNotMatch : secondGets ? secondIfNotMatch : 0;
                }

                return firstIfNotMatch == secondIfNotMatch ? firstIfNotMatch : 0;
            }
        }

        // Must be called while holding _getTwinLock. A call stopped waiting for its response.
        private void RemoveGetTwinWaiter(GetTwinWaiter waiter)
        {
            GetTwinFetch? fetch = waiter.Fetch;
            if (fetch != null && fetch.Waiters.Remove(waiter))
            {
                waiter.Fetch = null;
                UpdateGetTwinFetchAfterRemoval(fetch);
            }
        }

        // Must be called while holding _desiredStateLock. The resync no longer needs a GET.
        private void DetachDesiredResyncFromGetTwinFetch(DesiredResync resync)
        {
            lock (_getTwinLock)
            {
                foreach (GetTwinFetch? fetch in new[] { _activeGetTwinFetch, _nextGetTwinFetch })
                {
                    if (fetch != null && fetch.Resync == resync)
                    {
                        fetch.Resync = null;
                        UpdateGetTwinFetchAfterRemoval(fetch);
                        return;
                    }
                }
            }
        }

        // Must be called while holding _getTwinLock, once a call or the resync no longer waits for this GET. A GET that nobody
        // needs is stopped (or never sent), and a GET that isn't sent yet only asks for what is still needed.
        private void UpdateGetTwinFetchAfterRemoval(GetTwinFetch fetch)
        {
            if (fetch.IsEnded)
            {
                return;
            }

            if (fetch.Waiters.Count == 0 && fetch.Resync == null)
            {
                EndGetTwinFetch(fetch);
            }
            else if (fetch == _nextGetTwinFetch)
            {
                GetTwinRequest? request = fetch.Resync != null ? s_desiredResyncGetTwinRequest : null;
                foreach (GetTwinWaiter remaining in fetch.Waiters)
                {
                    request = request == null ? remaining.Request : CombineGetTwinRequests(request.Value, remaining.Request);
                }

                fetch.Request = request!.Value;
            }
        }

        // Must be called while holding _getTwinLock. Sends the first attempt of the active GET, or a new attempt (with a new ID,
        // so that responses to earlier attempts are dropped). A new attempt also carries everything waiting for the next GET.
        private void StartGetTwinAttempt(GetTwinFetch fetch)
        {
            if (fetch.IsEnded)
            {
                return;
            }

            GetTwinFetch? next = _nextGetTwinFetch;
            if (next != null && fetch == _activeGetTwinFetch)
            {
                _nextGetTwinFetch = null;
                fetch.Request = CombineGetTwinRequests(fetch.Request, next.Request);
                foreach (GetTwinWaiter waiter in next.Waiters)
                {
                    AttachToGetTwinFetch(fetch, waiter, null);
                }

                AttachToGetTwinFetch(fetch, null, next.Resync);
            }

            Guid getId = Guid.NewGuid();
            fetch.GetId = getId;
            fetch.IsFailed = false;
            fetch.Timer?.Dispose();
            fetch.Timer = null;

            // An earlier attempt that is still waiting to be sent (for example, held until the device reconnects) is superseded
            // by this one, so it must never be sent.
            _ = fetch.AttemptCancellation?.CancelAsync();
            CancellationTokenSource attemptCancellation = CancellationTokenSource.CreateLinkedTokenSource(fetch.Cancellation.Token);
            fetch.AttemptCancellation = attemptCancellation;

            // The connection client may hold the publish until the device has reconnected, so it is sent off this thread. That
            // way, a caller can stop waiting for the response, and the publish is never sent while holding a lock.
            _ = Task.Run(() => PublishGetTwinAsync(fetch, getId, attemptCancellation.Token));
        }

        private async Task PublishGetTwinAsync(GetTwinFetch fetch, Guid getId, CancellationToken attemptCancellationToken)
        {
            MqttPublish publish = CreateClassicGetTwinPublish(getId);

            try
            {
                lock (_getTwinLock)
                {
                    // The GET ended, or a newer attempt superseded this one, before this attempt got to be sent.
                    if (fetch.IsEnded || fetch.GetId != getId)
                    {
                        return;
                    }
                }

                Trace.TraceInformation("Publishing 'GetTwin' request on topic " + publish.Topic);

                // The connection client returns the PUBACK as-is, so a rejected request must be surfaced here.
                MqttPublishAck puback = await _connection.PublishAsync(publish, attemptCancellationToken);
                MqttAckValidator.ThrowIfPubackHasErrorCode(puback);
                List<GetTwinWaiter> abandoned;
                lock (_getTwinLock)
                {
                    // For example, the response already arrived.
                    if (fetch.IsEnded || fetch.GetId != getId)
                    {
                        return;
                    }

                    // The request went out on the current connection or an earlier one. If the current one was already lost,
                    // HandleConnectionLost may have run before this attempt counted as sent, so its response can never arrive.
                    Guid? connectionNonce = _connection.ConnectionNonce;
                    if (connectionNonce == null || connectionNonce != _getTwinLostConnectionNonce)
                    {
                        fetch.SentGetId = getId;

                        // Only once sent, so that a request that is waiting for the device to reconnect isn't sent again.
                        ArmGetTwinTimer(fetch, getId);
                        return;
                    }

                    // The resync (if any) ends along with the connection.
                    abandoned = TakeGetTwinWaiters(fetch);
                    EndGetTwinFetch(fetch);
                }

                foreach (GetTwinWaiter waiter in abandoned)
                {
                    waiter.Response.TrySetException(CreateGetTwinConnectionLostException());
                }
            }
            catch (OperationCanceledException) when (attemptCancellationToken.IsCancellationRequested)
            {
                // The GET already ended, or a newer attempt superseded this one.
            }
            catch (Exception e)
            {
                Trace.TraceWarning("Failed to publish a get twin request: {0}", e);
                FailGetTwinFetchAttempt(getId, e);
            }
        }

        // Must be called while holding _getTwinLock.
        private void ArmGetTwinTimer(GetTwinFetch fetch, Guid getId)
        {
            fetch.Timer?.Dispose();
            TimeSpan timeout = fetch.Resync != null ? DesiredResyncTimeoutSchedule(fetch.Attempt) : GetTwinTimeoutSchedule(fetch.Attempt);
            fetch.Timer = new System.Threading.Timer(
                _ => HandleGetTwinTimeout(fetch, getId),
                null,
                timeout,
                Timeout.InfiniteTimeSpan);
        }

        private void HandleGetTwinTimeout(GetTwinFetch fetch, Guid getId)
        {
            DesiredResync? resync;
            lock (_getTwinLock)
            {
                if (_isDisposed || fetch.IsEnded || fetch.GetId != getId)
                {
                    return;
                }

                resync = fetch.Resync;
            }

            if (resync != null)
            {
                // A resync doesn't outlive its connection. _desiredStateLock must be taken before _getTwinLock.
                lock (_desiredStateLock)
                {
                    DiscardDesiredResyncFromPreviousConnection(_connection.ConnectionNonce);
                }
            }

            lock (_getTwinLock)
            {
                if (_isDisposed || fetch.IsEnded || fetch.GetId != getId)
                {
                    return;
                }

                Trace.TraceWarning("Timed out waiting for the response to a get twin request. Sending it again.");
                fetch.Attempt++;
                StartGetTwinAttempt(fetch);
            }
        }

        // The latest attempt of the active GET failed (for example, IoT hub rejected it), so the calls waiting for it fail. If the
        // resync is waiting for it too, it is sent again once it times out, or sooner if another call needs a GET meanwhile.
        private void FailGetTwinFetchAttempt(Guid getId, Exception exception)
        {
            List<GetTwinWaiter> failed;
            lock (_getTwinLock)
            {
                GetTwinFetch? fetch = _activeGetTwinFetch;
                if (fetch == null || fetch.GetId != getId)
                {
                    return;
                }

                failed = TakeGetTwinWaiters(fetch);
                if (fetch.Resync == null)
                {
                    EndGetTwinFetch(fetch);
                }
                else if (_nextGetTwinFetch != null)
                {
                    StartGetTwinAttempt(fetch);
                }
                else
                {
                    fetch.IsFailed = true;
                    if (fetch.Timer == null)
                    {
                        ArmGetTwinTimer(fetch, getId);
                    }
                }
            }

            foreach (GetTwinWaiter waiter in failed)
            {
                waiter.Response.TrySetException(exception);
            }
        }

        // Must be called while holding _getTwinLock.
        private static List<GetTwinWaiter> TakeGetTwinWaiters(GetTwinFetch fetch)
        {
            List<GetTwinWaiter> waiters = new(fetch.Waiters);
            fetch.Waiters.Clear();
            foreach (GetTwinWaiter waiter in waiters)
            {
                waiter.Fetch = null;
            }

            return waiters;
        }

        // Must be called while holding _getTwinLock. Once the active GET ends, the next one is sent.
        private void EndGetTwinFetch(GetTwinFetch fetch)
        {
            if (fetch.IsEnded)
            {
                return;
            }

            fetch.IsEnded = true;
            fetch.Timer?.Dispose();

            // Callbacks registered on these tokens run asynchronously so that they never run while holding the lock.
            _ = fetch.Cancellation.CancelAsync();
            _ = fetch.AttemptCancellation?.CancelAsync();

            if (_nextGetTwinFetch == fetch)
            {
                _nextGetTwinFetch = null;
            }

            if (_activeGetTwinFetch == fetch)
            {
                // Once disposed, the next GET is never sent; it is left for AbandonAllGetTwinFetches to fail its waiters.
                if (_isDisposed)
                {
                    _activeGetTwinFetch = null;
                    return;
                }

                _activeGetTwinFetch = _nextGetTwinFetch;
                _nextGetTwinFetch = null;
                if (_activeGetTwinFetch != null)
                {
                    StartGetTwinAttempt(_activeGetTwinFetch);
                }
            }
        }

        // Ends the active GET if its latest attempt has this ID, and returns who was waiting for its response.
        private bool TryTakeGetTwinFetch(
            Guid getId,
            [System.Diagnostics.CodeAnalysis.NotNullWhen(true)] out List<GetTwinWaiter>? waiters,
            out DesiredResync? resync)
        {
            lock (_getTwinLock)
            {
                GetTwinFetch? fetch = _activeGetTwinFetch;
                if (fetch == null || fetch.GetId != getId)
                {
                    waiters = null;
                    resync = null;
                    return false;
                }

                waiters = TakeGetTwinWaiters(fetch);
                resync = fetch.Resync;
                EndGetTwinFetch(fetch);
                return true;
            }
        }

        // Delivers the response to the active GET whose latest attempt has this ID (if any) to the resync and the calls waiting for it.
        private async Task HandleGetTwinResponseAsync(Guid getId, TwinGetResponse response)
        {
            JsonObject? desired;
            JsonObject? reported;
            try
            {
                desired = response.HasDesiredPayload ? ParseSectionPayload(response.DesiredPayload) : null;
                reported = response.HasReportedPayload ? ParseSectionPayload(response.ReportedPayload) : null;
            }
            catch (JsonException e)
            {
                Trace.TraceWarning("Received a get twin response with a malformed twin section: {0}", e);
                FailGetTwinFetchAttempt(getId, e);
                return;
            }

            if (!TryTakeGetTwinFetch(getId, out List<GetTwinWaiter>? waiters, out DesiredResync? resync))
            {
                Trace.TraceInformation("Dropping a get twin response that does not match the outstanding get twin request.");
                return;
            }

            // The resync goes first, since it may find this device's copy of the twin invalid (see ResetLocalDesiredVersionIfInvalid).
            DesiredStateOutcome outcome = new();
            try
            {
                if (resync != null)
                {
                    ApplyDesiredResyncGetResponse(resync, response, desired, outcome);
                }

                if (desired != null && waiters.Any(waiter => waiter.Request.GetDesired))
                {
                    lock (_desiredStateLock)
                    {
                        // The application fetched the desired properties, so this device's view of them is now at least this version.
                        AdvanceLocalDesiredVersion(response.DesiredVersion, desired, isPatch: false);
                    }
                }

                if (reported != null && waiters.Any(waiter => waiter.Request.GetReported))
                {
                    AdvanceLocalReportedVersion(response.ReportedVersion, reported);
                }
            }
            catch (Exception e)
            {
                // These calls are no longer attached to any GET, so nothing else would complete them (for example, when a
                // malformed patch that the resync buffered fails to parse).
                foreach (GetTwinWaiter waiter in waiters)
                {
                    waiter.Response.TrySetException(e);
                }

                throw;
            }

            foreach (GetTwinWaiter waiter in waiters)
            {
                GetTwinRequest request = waiter.Request;
                waiter.Response.TrySetResult(new DeviceTwin()
                {
                    Desired = IsSectionIncluded(request.GetDesired, request.IfNotMatchDesired, response.DesiredVersion, desired)
                        ? desired!.DeepClone().AsObject()
                        : null,
                    Reported = IsSectionIncluded(request.GetReported, request.IfNotMatchReported, response.ReportedVersion, reported)
                        ? reported!.DeepClone().AsObject()
                        : null,
                    DesiredVersion = response.DesiredVersion,
                    ReportedVersion = response.ReportedVersion,
                });
            }

            await CompleteAsync(outcome);

            // Each call only gets the sections it asked for, without those its if-not-match versions exclude, as if IoT hub had
            // answered its request alone. IoT hub doesn't support if-not-match, so this client applies it.
            bool IsSectionIncluded(bool isRequested, ulong ifNotMatch, ulong version, JsonObject? properties)
            {
                return isRequested
                    && properties != null
                    && (ifNotMatch == 0 || ifNotMatch < version);
            }
        }

        // Twin exchanges can't outlive the connection they were sent on, since IoT hub never sends their responses on a later one.
        private void HandleConnectionLost()
        {
            // A GET that is still waiting to be sent (for example, one made while reconnecting) is sent on the next connection
            // instead. The resync (if any) ends along with the connection.
            List<GetTwinWaiter> abandoned = new();
            lock (_getTwinLock)
            {
                _getTwinLostConnectionNonce = _connection.ConnectionNonce;
                GetTwinFetch? active = _activeGetTwinFetch;
                if (active != null && active.GetId != null && active.SentGetId == active.GetId)
                {
                    abandoned = TakeGetTwinWaiters(active);
                    EndGetTwinFetch(active);
                }
            }

            foreach (GetTwinWaiter waiter in abandoned)
            {
                waiter.Response.TrySetException(CreateGetTwinConnectionLostException());
            }

            // Reported patches are kept and sent again on the next connection. IoT hub may have applied the attempt in flight,
            // in which case it is applied again, since IoT hub doesn't support conditional patches.
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

        private void AbandonAllGetTwinFetches(Func<Exception> createException)
        {
            List<GetTwinWaiter> abandoned = new();
            lock (_getTwinLock)
            {
                // The next GET first, so that ending the active one doesn't send it.
                foreach (GetTwinFetch? fetch in new[] { _nextGetTwinFetch, _activeGetTwinFetch })
                {
                    if (fetch != null)
                    {
                        abandoned.AddRange(TakeGetTwinWaiters(fetch));
                        EndGetTwinFetch(fetch);
                    }
                }
            }

            foreach (GetTwinWaiter waiter in abandoned)
            {
                waiter.Response.TrySetException(createException());
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
            if (!args.Publish.Topic.StartsWith(ClassicTwinTopicPrefix, StringComparison.Ordinal))
            {
                // The publish is not relevant to this client, so ignore it.
                return;
            }

            await HandleReceivedClassicPublishAsync(args.Publish);
        }

        // Must be called while holding _desiredStateLock.
        private void ProcessDesiredPatch(DesiredPatch patch, Guid? connectionNonce, DesiredStateOutcome outcome)
        {
            if (_desiredResync != null)
            {
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
                    RequestDesiredResyncGet(_desiredResync, isNewAttemptRequired: true);
                    return;
                }

                _desiredResync.Buffer.Add(patch);
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
                StartDesiredResync(connectionNonce, patch);
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
        private TwinPushSection? TryApplyDesiredSnapshot(ulong version, JsonObject desiredProperties)
        {
            if (version <= _localDesiredVersion)
            {
                return null;
            }

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
        private void StartDesiredResync(Guid? connectionNonce, DesiredPatch? firstBufferedPatch)
        {
            _desiredResync = new DesiredResync(connectionNonce);
            if (firstBufferedPatch != null)
            {
                _desiredResync.Buffer.Add(firstBufferedPatch);
            }

            RequestDesiredResyncGet(_desiredResync, isNewAttemptRequired: false);
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

        // Must be called while holding _desiredStateLock. The resync shares the one GET request to IoT hub with the application's
        // get twin calls: it waits for the active GET if that returns the desired properties in full, and otherwise for the next.
        // If isNewAttemptRequired, the GET it waits for is sent again (for example, because earlier attempts may not reach this
        // client), unless it isn't sent yet.
        private void RequestDesiredResyncGet(DesiredResync resync, bool isNewAttemptRequired)
        {
            lock (_getTwinLock)
            {
                if (_isDisposed || _nextGetTwinFetch?.Resync == resync)
                {
                    return;
                }

                if (_activeGetTwinFetch?.Resync == resync)
                {
                    if (isNewAttemptRequired)
                    {
                        StartGetTwinAttempt(_activeGetTwinFetch);
                    }

                    return;
                }

                AddToGetTwinFetch(s_desiredResyncGetTwinRequest, null, resync, isNewAttemptRequired);
            }
        }

        // Must be called while holding _desiredStateLock.
        private void CancelDesiredResync()
        {
            if (_desiredResync != null)
            {
                DetachDesiredResyncFromGetTwinFetch(_desiredResync);
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

        // Applies the response to a GET that the resync waited for, unless the resync already ended (for example, a twin push resolved it).
        private void ApplyDesiredResyncGetResponse(DesiredResync resync, TwinGetResponse response, JsonObject? desired, DesiredStateOutcome outcome)
        {
            DesiredResync? connectSync = null;
            TwinPushReceivedEventArgs connectSyncPush = new();
            lock (_desiredStateLock)
            {
                Guid? connectionNonce = _connection.ConnectionNonce;
                DiscardDesiredResyncFromPreviousConnection(connectionNonce);

                if (_desiredResync != resync)
                {
                    return;
                }

                if (resync.IsConnectSync)
                {
                    connectSync = resync;
                    ResetLocalDesiredVersionIfInvalid(connectSync, response);
                }

                TwinPushSection? snapshot = desired != null
                    ? TryApplyDesiredSnapshot(response.DesiredVersion, desired)
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
        }

        // Raises the events that the desired section state machine decided on, outside of _desiredStateLock.
        private Task CompleteAsync(DesiredStateOutcome outcome)
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

            return Task.CompletedTask;
        }

        private static JsonObject ParseSectionPayload(ByteString payload)
        {
            return payload.IsEmpty ? new JsonObject() : JsonObject.Parse(payload.Span)!.AsObject();
        }

        // The escalating, jittered schedule of defensive timeouts that IoT hub prescribes for exchanges that should complete
        // on a healthy connection.
        private static class DefensiveTimeoutSchedule
        {
            // Get how long to wait for the given attempt (starting at 1) of an exchange before recovering.
            public static TimeSpan GetTimeout(int attempt)
            {
                int baseMinutes = attempt switch
                {
                    1 => 5,
                    2 => 6,
                    3 => 8,
                    _ => 10,
                };

                // The wide jitter keeps devices that time out together from all recovering in lockstep.
                return TimeSpan.FromMinutes(baseMinutes) + TimeSpan.FromMinutes(5 * Random.Shared.NextDouble());
            }
        }

        private sealed class DesiredResync(Guid? connectionNonce)
        {
            public Guid? ConnectionNonce { get; } = connectionNonce;

            public List<DesiredPatch> Buffer { get; } = new();

            // Whether this is the GET that this client sends upon each connection to IoT hub (the connect sync).
            public bool IsConnectSync { get; set; }

            // This device's versions of the twin sections when the connect sync started.
            public ulong DesiredVersionAtStart { get; set; }

            public ulong ReportedVersionAtStart { get; set; }
        }

        private sealed class DesiredStateOutcome
        {
            public List<EventArgs> Deliveries { get; } = new();

            // The connect sync that completed, after whose deliveries this client starts sending reported properties patches.
            public DesiredResync? CompletedConnectSync { get; set; }
        }

        private readonly record struct GetTwinRequest(bool GetDesired, bool GetReported, ulong IfNotMatchDesired, ulong IfNotMatchReported);

        // An application's get twin call, waiting for the response to a GET.
        private sealed class GetTwinWaiter(GetTwinRequest request)
        {
            public GetTwinRequest Request { get; } = request;

            public TaskCompletionSource<DeviceTwin> Response { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);

            // Guarded by _getTwinLock. The GET this call waits for, if it still waits.
            public GetTwinFetch? Fetch { get; set; }
        }

        // A GET request to IoT hub (sent, or waiting to be sent), and who waits for its response. Guarded by _getTwinLock.
        private sealed class GetTwinFetch(GetTwinRequest request)
        {
            // What this GET asks for, which covers what each of its waiters (and the resync) asked for.
            public GetTwinRequest Request { get; set; } = request;

            public List<GetTwinWaiter> Waiters { get; } = new();

            // The desired properties resync waiting for this GET, if any.
            public DesiredResync? Resync { get; set; }

            // Cancelled when the GET ends, so that an attempt still waiting to be sent is never sent.
            public CancellationTokenSource Cancellation { get; } = new();

            // Cancelled when a newer attempt starts (or the GET ends), so that a superseded attempt still waiting to be sent is
            // never sent.
            public CancellationTokenSource? AttemptCancellation { get; set; }

            // The ID of the latest attempt.
            public Guid? GetId { get; set; }

            // The ID of the latest attempt that was sent. Only sent attempts can lose their response to a connection loss.
            public Guid? SentGetId { get; set; }

            public int Attempt { get; set; } = 1;

            // Whether the latest attempt failed, in which case this GET only waits for its timeout to be sent again for the resync.
            public bool IsFailed { get; set; }

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
        private sealed class ReportedPatchAttempt(Guid patchId, List<PendingReportedPatch> patches, byte[] payload)
        {
            public Guid PatchId { get; } = patchId;

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

            AbandonAllGetTwinFetches(() => new ObjectDisposedException(nameof(TwinClient)));
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
