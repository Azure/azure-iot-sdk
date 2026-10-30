// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Exceptions;
using Microsoft.Azure.Iot.Device.Models.Twin;
using Microsoft.Azure.Iot.Device.MQTTv5.Connection;
using System.Diagnostics;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace Microsoft.Azure.Iot.Device.Twin
{
    internal sealed class TwinEngineOptions
    {
        // Whether each reported properties patch is conditional on IoT hub's version of the reported properties (if known).
        public bool ConditionalReportedPatches { get; init; }

        // Whether a reported properties patch attempt whose publish failed with a retryable error stays in flight (to be sent
        // again once it times out or the device reconnects), rather than failing its patches.
        public bool KeepAttemptOnRetryablePublishFailure { get; init; }
    }

    // Keeps this device's copy of its twin in sync with IoT hub, as the device twin specification recommends, over whichever
    // transport the connection uses. It tracks the versions of the twin that this device holds, delivers desired properties
    // updates in version order (resyncing the desired properties when it misses one), and queues reported properties
    // patches, sending them one at a time across reconnects until IoT hub responds. The public twin clients wrap it.
    internal sealed class TwinEngine : ITwinTransportHandler
    {
        private volatile bool _isDisposed;

        private readonly ITwinTransport _transport;

        // The public client that wraps this engine, which is named by ObjectDisposedException.
        private readonly object _owner;

        private readonly TwinEngineOptions _options;

        // Guards _getTwinExchangesByRequest and _getTwinExchangesById.
        private readonly object _getTwinExchangesLock = new();

        // The outstanding get twin exchange for each distinct get twin request. Identical concurrent requests share one exchange.
        private readonly Dictionary<GetTwinRequest, GetTwinExchange> _getTwinExchangesByRequest = new();

        // The outstanding get twin exchanges, keyed by the ID of their latest attempt. Responses to earlier attempts are dropped.
        private readonly Dictionary<Guid, GetTwinExchange> _getTwinExchangesById = new();

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

        // The connection nonce of the latest birth ack that this engine processed.
        private Guid? _birthAckConnectionNonce;

        // Guarded by _reportedPatchLock. The connection nonce of the latest connection whose connect twin snapshot (see
        // ITwinTransport.ConnectTwinSnapshot) this engine reconciled with. Like the birth ack, it gates reported patches.
        private Guid? _connectTwinReconciledNonce;

        // The connection nonce of the latest connection that was lost. Reported patches must not be sent on it again, since they
        // would carry if-match versions from before the connection was lost.
        private Guid? _lostConnectionNonce;

        // The maximum number of desired patches buffered while resyncing the desired properties.
        internal const int DesiredResyncBufferCapacity = 8;

        // Guards the device's view of the desired properties (the desired section state machine).
        private readonly object _desiredStateLock = new();

        // The version of the desired properties that this device has most recently observed. 0 means the device has not
        // observed the desired properties yet. Unlike the resync state, this survives disconnects.
        private ulong _localDesiredVersion;

        // Guarded by _desiredStateLock. This device's versions of the twin sections when the current connection started, which
        // is before the connection got its connect twin snapshot (see ApplyConnectTwinSnapshotAsync), and the connection nonce
        // of the latest connection whose connect twin snapshot this engine applied.
        private ulong _desiredVersionAtConnectionStart;
        private ulong _reportedVersionAtConnectionStart;
        private Guid? _connectTwinAppliedNonce;

        // Guards _localReportedVersion.
        private readonly object _reportedStateLock = new();

        // The version of the reported properties that this device holds in full. 0 means the device does not hold them.
        private ulong _localReportedVersion;

        // Guarded by _reportedStateLock. The connection nonce of the connection on which IoT hub revealed that it has
        // _localReportedVersion, or null if it hasn't (for example, the version is an initial or restored one). IoT hub's
        // versions never decrease, so a version revealed on the current connection can't be invalidated by an older twin push
        // sent on it.
        private Guid? _localReportedVersionConfirmedNonce;

        // Guards this device's copy of its twin, which TwinState snapshots are made from. Writes to _localDesiredVersion and
        // _localReportedVersion also take it, so that each version always matches its properties. It is always taken last.
        private readonly object _twinStateLock = new();

        // The desired properties as of _localDesiredVersion.
        private JsonObject _desiredProperties = new();

        // Whether _desiredProperties are the desired properties in full at _localDesiredVersion. A version that the application
        // only knows the number of (from the connection client's options) orders the patches, but isn't held in full.
        private bool _isDesiredPropertiesComplete;

        // The reported properties that IoT hub has applied, as of _localReportedVersion.
        private JsonObject _reportedProperties = new();

        // Whether _reportedProperties are the reported properties in full at _localReportedVersion.
        private bool _isReportedPropertiesComplete;

        // Whether this device's copy of its twin (including its pending reported patches) changed since TwinStateChanged was last raised.
        private bool _isTwinStateChanged;

        // Guarded by _reportedPatchLock. The ReportedPatchCompleted events that are yet to be raised, in order.
        private readonly Queue<ReportedPatchCompletedEventArgs> _reportedPatchCompletions = new();

        // Held while raising TwinStateChanged and ReportedPatchCompleted, so that they are raised one at a time, in order.
        private readonly object _twinEventsLock = new();

        // Non-null while this engine is resyncing its desired properties (and, if the transport doesn't announce twin versions,
        // reconciling its reported properties) with IoT hub.
        private DesiredResync? _desiredResync;

        // Guards _authoritativeVersions.
        private readonly object _authoritativeVersionsLock = new();

        // The latest versions of the twin sections that IoT hub has revealed on the current connection.
        private AuthoritativeVersions? _authoritativeVersions;

        internal Func<int, TimeSpan> DesiredResyncTimeoutSchedule { get; set; } = DefensiveTimeoutSchedule.GetTimeout;

        internal Func<int, TimeSpan> GetTwinTimeoutSchedule { get; set; } = DefensiveTimeoutSchedule.GetTimeout;

        internal Func<int, TimeSpan> ReportedPatchTimeoutSchedule { get; set; } = DefensiveTimeoutSchedule.GetTimeout;

        public event Action<DesiredPatchReceivedEventArgs>? DesiredPatchReceived;

        public event Action<TwinPushReceivedEventArgs>? TwinPushReceived;

        public event Action<TwinState>? TwinStateChanged;

        public event Action<ReportedPatchCompletedEventArgs>? ReportedPatchCompleted;

        public ulong? AuthoritativeDesiredPropertiesVersion => GetAuthoritativeVersion(desired: true);

        public ulong? AuthoritativeReportedPropertiesVersion => GetAuthoritativeVersion(desired: false);

        public TwinEngine(ITwinTransport transport, object owner, TwinEngineOptions options, TwinState? initialState)
        {
            _transport = transport;
            _owner = owner;
            _options = options;
            _localDesiredVersion = transport.InitialDesiredPropertiesVersion;
            _localReportedVersion = transport.InitialReportedPropertiesVersion;
            _isDesiredPropertiesComplete = _localDesiredVersion == 0;
            _isReportedPropertiesComplete = _localReportedVersion == 0;

            if (initialState != null)
            {
                _localDesiredVersion = initialState.DesiredVersion;
                _desiredProperties = initialState.Desired.DeepClone().AsObject();
                _isDesiredPropertiesComplete = true;
                _localReportedVersion = initialState.ReportedVersion;
                _reportedProperties = initialState.Reported.DeepClone().AsObject();
                _isReportedPropertiesComplete = true;
                _transport.ReplaceDesiredPropertiesVersion(initialState.DesiredVersion);
                _transport.ReplaceReportedPropertiesVersion(initialState.ReportedVersion);
                foreach (JsonObject patch in initialState.PendingReportedPatches)
                {
                    _pendingReportedPatches.Add(new PendingReportedPatch(patch.DeepClone().AsObject(), hasWaiter: false, isReleased: true));
                }
            }

            _desiredVersionAtConnectionStart = _localDesiredVersion;
            _reportedVersionAtConnectionStart = _localReportedVersion;

            _transport.Start(this);

            // This engine may be created after the current connection's birth ack arrived, in which case it checks the
            // versions it starts with against that birth ack, just as it would have when the birth ack arrived.
            if (_transport.AnnouncesTwinVersions)
            {
                Guid? connectionNonce = _transport.ConnectionNonce;
                BirthAckVersions? latestBirthAck = _transport.LatestBirthAck;
                if (connectionNonce != null && latestBirthAck?.ConnectionNonce == connectionNonce)
                {
                    _ = CompleteAsync(ReconcileHeldVersions(connectionNonce, latestBirthAck.DesiredVersion, latestBirthAck.ReportedVersion));
                }
            }
            else if (_transport.IsReady && _transport.ConnectTwinSnapshot != null)
            {
                // The connection became ready (with the twin it got as it connected) before this engine was created, so this
                // engine reconciles with that twin now (before the owner can add any event handler) rather than waiting for
                // the next connection.
                ((ITwinTransportHandler)this).HandleConnectionReady();
            }
        }

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

        public async Task<DeviceTwin> GetTwinAsync(bool getReported, bool getDesired, ulong ifNotMatchReported, ulong ifNotMatchDesired, CancellationToken cancellationToken)
        {
            ObjectDisposedException.ThrowIf(_isDisposed, _owner);
            if (!getReported && !getDesired)
            {
                throw new ArgumentException("At least one twin section must be requested.");
            }

            if (!_transport.IsConnected)
            {
                throw new NotSupportedException("Must be connected before calling this method.");
            }

            GetTwinRequest request = new(getDesired, getReported, ifNotMatchDesired, ifNotMatchReported);
            GetTwinExchange? exchange;
            bool isNewExchange = false;
            lock (_getTwinExchangesLock)
            {
                // Disposal may have abandoned the outstanding exchanges since the check above, and would never end a new one.
                ObjectDisposedException.ThrowIf(_isDisposed, _owner);
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

        public async Task<ReportedPatchResponse> UpdateReportedPropertiesAsync(JsonObject reportedProperties, CancellationToken cancellationToken)
        {
            ObjectDisposedException.ThrowIf(_isDisposed, _owner);
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
                    // A patch that was ever sent may already be applied, so it stays until IoT hub responds to it.
                    if (!pendingPatch.HasBeenSent && _pendingReportedPatches.Remove(pendingPatch))
                    {
                        MarkTwinStateChanged();
                    }
                }

                RaiseTwinEvents();
                throw;
            }
        }

        public void EnqueueReportedPropertiesUpdate(JsonObject reportedProperties)
        {
            ObjectDisposedException.ThrowIf(_isDisposed, _owner);
            ArgumentNullException.ThrowIfNull(reportedProperties);
            AddPendingReportedPatch(reportedProperties, hasWaiter: false);
        }

        private PendingReportedPatch AddPendingReportedPatch(JsonObject reportedProperties, bool hasWaiter)
        {
            // A patch is only sent once RaiseTwinEvents has raised a snapshot containing it (and its handlers have returned),
            // so that it is persisted before being sent. If called from a twin event handler, the outer call to
            // RaiseTwinEvents does so.
            // Copy the patch so that later changes the application makes to it don't affect what is sent.
            PendingReportedPatch pendingPatch = new(reportedProperties.DeepClone().AsObject(), hasWaiter, isReleased: false);
            ReportedPatchAttempt? attempt;
            lock (_reportedPatchLock)
            {
                ObjectDisposedException.ThrowIf(_isDisposed, _owner);
                _pendingReportedPatches.Add(pendingPatch);
                MarkTwinStateChanged();
                attempt = TryStartReportedPatchAttempt();
            }

            RaiseTwinEvents();
            SendReportedPatchAttempt(attempt);
            return pendingPatch;
        }

        // Must be called while holding _reportedPatchLock. Starts the next reported patch attempt from the pending patches, unless
        // one is already in flight, there are none, the transport isn't ready, or (if the transport announces twin versions)
        // IoT hub hasn't acknowledged the current connection's birth yet.
        private ReportedPatchAttempt? TryStartReportedPatchAttempt()
        {
            if (_isDisposed || _reportedPatchAttempt != null || _pendingReportedPatches.Count == 0 || !_pendingReportedPatches[0].IsReleased || !_transport.IsReady)
            {
                return null;
            }

            // Each connection's birth ack reveals IoT hub's current version of the reported properties, so wait for it.
            // Transports that don't expose a connection nonce don't announce this device's presence at all.
            Guid? connectionNonce = _transport.ConnectionNonce;
            if (_transport.AnnouncesTwinVersions && connectionNonce != null && !HasBirthAck(connectionNonce.Value))
            {
                return null;
            }

            // Likewise, the twin that a connection got as it connected reveals IoT hub's reported properties, so wait until
            // this engine has reconciled with it.
            if (!_transport.AnnouncesTwinVersions
                && connectionNonce != null
                && _connectTwinReconciledNonce != connectionNonce
                && _transport.ConnectTwinSnapshot != null)
            {
                return null;
            }

            // If IoT hub didn't reveal its version, an if-match of 0 makes this patch unconditional.
            ulong ifMatch = _options.ConditionalReportedPatches ? GetAuthoritativeVersion(desired: false) ?? 0 : 0;

            JsonObject payload = _pendingReportedPatches[0].Patch.DeepClone().AsObject();
            int patchCount = 1;
            while (patchCount < _pendingReportedPatches.Count && _pendingReportedPatches[patchCount].IsReleased)
            {
                JsonObject combined = payload.DeepClone().AsObject();
                if (!TryCombineMergePatches(combined, _pendingReportedPatches[patchCount].Patch))
                {
                    break;
                }

                payload = combined;
                patchCount++;
            }

            List<PendingReportedPatch> patches = _pendingReportedPatches.GetRange(0, patchCount);
            foreach (PendingReportedPatch pendingPatch in patches)
            {
                pendingPatch.HasBeenSent = true;
            }

            ReportedPatchAttempt attempt = new(
                Guid.NewGuid(),
                ifMatch,
                patches,
                Encoding.UTF8.GetBytes(JsonSerializer.Serialize(payload)));
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

            return _birthAckConnectionNonce == connectionNonce || _transport.LatestBirthAck?.ConnectionNonce == connectionNonce;
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
            try
            {
                await _transport.PublishReportedPatchAsync(attempt.PatchId, attempt.IfMatch, attempt.Payload, attempt.Cancellation.Token).ConfigureAwait(false);
            }
            catch (OperationCanceledException) when (attempt.Cancellation.IsCancellationRequested)
            {
                // The attempt already ended (for example, the connection was lost), so it must not be sent.
            }
            catch (DeviceException e) when (_options.KeepAttemptOnRetryablePublishFailure && e.Retryability == ErrorRetryability.Retryable)
            {
                // The attempt stays in flight, so it is sent again once it times out or the device reconnects.
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

        // Returns false if the response isn't for the reported patch attempt in flight.
        private async Task<bool> TryHandleReportedPatchResponseAsync(Guid? connectionNonce, Guid patchId, TwinResponse twinResponse)
        {
            ReportedPatchAttempt? nextAttempt;
            ReportedPatchResponse response = twinResponse.PatchResponse!;
            List<PendingReportedPatch>? resolvedPatches = null;
            bool isReportedStateStale = false;
            lock (_reportedPatchLock)
            {
                ReportedPatchAttempt? attempt = _reportedPatchAttempt;
                if (attempt == null || attempt.PatchId != patchId)
                {
                    return false;
                }

                if (twinResponse.Status == TwinResponseStatus.Transient)
                {
                    // Keep the attempt in flight, so that it is sent again once its defensive timeout elapses.
                    Trace.TraceWarning("IoT hub responded to a reported properties patch with status {0}. Sending it again later.", twinResponse.StatusCode);
                    attempt.Timer?.Change(ReportedPatchTimeoutSchedule(_reportedPatchTimeoutAttempt), Timeout.InfiniteTimeSpan);
                    return true;
                }

                // Whatever the result, the response carries IoT hub's current version of the reported properties (if any).
                RecordAuthoritativeVersions(connectionNonce, 0, response.Version);
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
                            // A transport that doesn't announce twin versions may not reveal the version a patch was applied at (0).
                            if (_localReportedVersion != 0 && response.Version <= _localReportedVersion && (response.Version != 0 || _transport.AnnouncesTwinVersions))
                            {
                                // This device already holds a version of the reported properties (for example, from a concurrent
                                // get twin response) that includes these patches, and possibly newer changes that they must not overwrite.
                                Trace.TraceInformation("IoT hub applied a reported properties patch at version {0}, but this device already holds version {1}. Not applying the patch to this device's copy.", response.Version, _localReportedVersion);
                            }
                            else
                            {
                                lock (_twinStateLock)
                                {
                                    // IoT hub applied these patches in order, so this device's copy of the reported properties has them too.
                                    foreach (PendingReportedPatch pendingPatch in attempt.Patches)
                                    {
                                        ApplyMergePatch(_reportedProperties, pendingPatch.Patch);
                                    }
                                }
                            }

                            // If this device held the version before this patch, it now holds them at the patched version (in full, if it
                            // held them in full before). Otherwise, it still doesn't hold them in full.
                            if (_localReportedVersion != 0 && response.Version == _localReportedVersion + 1)
                            {
                                AdvanceLocalReportedVersion(response.Version);
                            }
                            else if (_localReportedVersion != 0 && response.Version > _localReportedVersion + 1)
                            {
                                // Someone else changed the reported properties in between, so this device's copy lacks their changes.
                                Trace.TraceWarning("This device held version {0} of the reported properties, but IoT hub applied its patch at version {1}. This device no longer considers itself to hold the reported properties.", _localReportedVersion, response.Version);
                                SetLocalReportedState(0, null);
                                _transport.ReplaceReportedPropertiesVersion(0);

                                // Without twin pushes, the only way to get the reported properties again is to get the twin.
                                isReportedStateStale = !_transport.AnnouncesTwinVersions;
                            }
                        }
                    }
                    else
                    {
                        Trace.TraceWarning("IoT hub rejected a reported properties patch: {0}.", response.Result);
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

            if (isReportedStateStale)
            {
                await StartTwinResyncAsync(connectionNonce).ConfigureAwait(false);
            }

            return true;
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
            try
            {
                await _transport.PublishGetAsync(getId, request.GetDesired, request.GetReported, request.IfNotMatchDesired, request.IfNotMatchReported, exchange.Cancellation.Token).ConfigureAwait(false);
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

        // Returns false if the response isn't for the latest attempt of an application's get twin exchange.
        private bool TryHandleGetTwinResponse(Guid? connectionNonce, Guid getId, TwinResponse response)
        {
            GetTwinExchange? exchange;
            lock (_getTwinExchangesLock)
            {
                if (!_getTwinExchangesById.TryGetValue(getId, out exchange))
                {
                    return false;
                }

                if (response.Status == TwinResponseStatus.Transient)
                {
                    // Keep the exchange outstanding, so that it is sent again once its defensive timeout elapses.
                    Trace.TraceWarning("IoT hub responded to a get twin request with status {0}. Sending it again later.", response.StatusCode);
                    exchange.Timer?.Change(GetTwinTimeoutSchedule(exchange.Attempt), Timeout.InfiniteTimeSpan);
                    return true;
                }

                EndGetTwinExchange(exchange);
            }

            if (response.Status == TwinResponseStatus.Rejected)
            {
                exchange.Response.TrySetException(new DeviceException($"IoT hub rejected the get twin request with status {response.StatusCode}.")
                {
                    Retryability = ErrorRetryability.Terminal,
                    IsContained = true,
                });
                return true;
            }

            GetTwinRequest request = exchange.Request;
            TwinSnapshot snapshot;
            try
            {
                snapshot = response.ParseSnapshot!();
            }
            catch (Exception e)
            {
                exchange.Response.TrySetException(e);
                return true;
            }

            RecordAuthoritativeVersions(connectionNonce, snapshot.DesiredVersion, snapshot.ReportedVersion);

            DeviceTwin twin = new()
            {
                Desired = request.GetDesired ? snapshot.Desired : null,
                Reported = request.GetReported ? snapshot.Reported : null,
                DesiredVersion = snapshot.DesiredVersion,
                ReportedVersion = snapshot.ReportedVersion,
            };

            // A transport that doesn't announce twin versions returns empty sections (at version 0) for a twin it doesn't have.
            if (twin.Desired != null && (snapshot.DesiredVersion != 0 || _transport.AnnouncesTwinVersions))
            {
                lock (_desiredStateLock)
                {
                    // The application fetched the desired properties, so this device's view of them is now at least this version.
                    AdvanceLocalDesiredVersion(snapshot.DesiredVersion, twin.Desired, isPatch: false);
                }
            }

            if (twin.Reported != null)
            {
                AdvanceLocalReportedVersion(snapshot.ReportedVersion, twin.Reported);
            }

            exchange.Response.TrySetResult(twin);
            return true;
        }

        // Twin exchanges can't outlive the connection they were sent on, since IoT hub never sends their responses on a later one.
        void ITwinTransportHandler.HandleConnectionLost(Guid? lostConnectionNonce)
        {
            AbandonGetTwinExchanges(() => new DeviceException("The connection to IoT hub was lost before the response to this twin request arrived. It may be retried once reconnected.")
            {
                Retryability = ErrorRetryability.Retryable,
                IsContained = true,
            });

            // Reported patches are kept and sent again on the next connection. IoT hub may have applied the attempt in flight,
            // in which case a conditional attempt's if-match is rejected with the version that it was applied at, and so on,
            // while an unconditional one is applied again.
            ReportedPatchAttempt? nextAttempt;
            lock (_reportedPatchLock)
            {
                _lostConnectionNonce = lostConnectionNonce;
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

                _desiredVersionAtConnectionStart = _localDesiredVersion;
                lock (_reportedStateLock)
                {
                    _reportedVersionAtConnectionStart = _localReportedVersion;
                }
            }
        }

        // Sends the reported patches held while the device wasn't connected. If IoT hub doesn't push the twin when the device
        // connects, first reconciles with the twin that the connection got as it connected, or gets the twin if there is none.
        void ITwinTransportHandler.HandleConnectionReady()
        {
            if (_isDisposed || !_transport.IsReady)
            {
                return;
            }

            if (_transport.AnnouncesTwinVersions)
            {
                // The connection client may have received this connection's birth ack before passing it on.
                Guid? connectionNonce = _transport.ConnectionNonce;
                BirthAckVersions? latestBirthAck = _transport.LatestBirthAck;
                bool isBirthAckUnprocessed;
                lock (_reportedPatchLock)
                {
                    isBirthAckUnprocessed = connectionNonce != null
                        && latestBirthAck?.ConnectionNonce == connectionNonce
                        && _birthAckConnectionNonce != connectionNonce;
                }

                if (isBirthAckUnprocessed)
                {
                    _ = ((ITwinTransportHandler)this).HandleBirthAckAsync(connectionNonce, latestBirthAck!.DesiredVersion, latestBirthAck.ReportedVersion);
                    return;
                }
            }
            else
            {
                TwinSnapshot? connectTwin = _transport.ConnectTwinSnapshot;
                if (connectTwin != null)
                {
                    // Like a birth ack and twin push, this reconciles the twin and then sends the held reported patches.
                    _ = ApplyConnectTwinSnapshotAsync(_transport.ConnectionNonce, connectTwin);
                    return;
                }
            }

            ReportedPatchAttempt? nextAttempt;
            lock (_reportedPatchLock)
            {
                nextAttempt = TryStartReportedPatchAttempt();
            }

            SendReportedPatchAttempt(nextAttempt);

            if (!_transport.AnnouncesTwinVersions)
            {
                _ = StartTwinResyncAsync(_transport.ConnectionNonce);
            }
        }

        private void AbandonGetTwinExchanges(Func<Exception> createException)
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
        }

        // The transport calls these handlers as it receives each message, one at a time and in the order received, so that this
        // engine's state machines see the messages in that order. Each raises the twin events that are due once it's done.

        async Task ITwinTransportHandler.HandleResponseAsync(Guid? connectionNonce, Guid requestId, TwinResponse response)
        {
            try
            {
                if (response.ParseSnapshot != null)
                {
                    if (TryHandleGetTwinResponse(connectionNonce, requestId, response)
                        || await TryHandleDesiredResyncGetResponseAsync(connectionNonce, requestId, response).ConfigureAwait(false))
                    {
                        return;
                    }
                }

                if (response.PatchResponse != null
                    && await TryHandleReportedPatchResponseAsync(connectionNonce, requestId, response).ConfigureAwait(false))
                {
                    return;
                }

                Trace.TraceInformation("Dropping a twin response that does not match any outstanding twin request.");
            }
            finally
            {
                RaiseTwinEvents();
            }
        }

        async Task ITwinTransportHandler.HandleDesiredPatchAsync(Guid? connectionNonce, ReceivedDesiredPatch patch)
        {
            try
            {
                RecordAuthoritativeVersions(connectionNonce, patch.Version, 0);

                DesiredStateOutcome outcome = new();
                lock (_desiredStateLock)
                {
                    DiscardDesiredResyncFromPreviousConnection(connectionNonce);
                    ProcessDesiredPatch(patch, connectionNonce, outcome);
                }

                await CompleteAsync(outcome).ConfigureAwait(false);
            }
            finally
            {
                RaiseTwinEvents();
            }
        }

        async Task ITwinTransportHandler.HandleTwinPushAsync(Guid? connectionNonce, TwinPushSection? desired, TwinPushSection? reported)
        {
            try
            {
                RecordAuthoritativeVersions(connectionNonce, desired?.PropertiesVersion ?? 0, reported?.PropertiesVersion ?? 0);
                TwinPushReceivedEventArgs twinPushArgs = new();
                if (reported != null && SetLocalReportedVersionFromSnapshot(connectionNonce, reported.PropertiesVersion, reported.Properties))
                {
                    twinPushArgs.Reported = reported;
                }

                DesiredStateOutcome outcome = new();
                lock (_desiredStateLock)
                {
                    DiscardDesiredResyncFromPreviousConnection(connectionNonce);

                    if (desired != null)
                    {
                        twinPushArgs.Desired = TryApplyDesiredSnapshot(desired.PropertiesVersion, desired.Properties);
                        if (twinPushArgs.Desired == null)
                        {
                            Trace.TraceInformation("Dropping the desired section of a twin push with version {0} because this device has already observed version {1}.", desired.PropertiesVersion, _localDesiredVersion);
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

                await CompleteAsync(outcome).ConfigureAwait(false);
            }
            finally
            {
                RaiseTwinEvents();
            }
        }

        async Task ITwinTransportHandler.HandleBirthAckAsync(Guid? connectionNonce, ulong desiredVersion, ulong reportedVersion)
        {
            try
            {
                DesiredStateOutcome outcome = ReconcileHeldVersions(connectionNonce, desiredVersion, reportedVersion);

                // IoT hub's version of the reported properties is now known (if it has one), so conditional patches can be sent.
                ReportedPatchAttempt? nextAttempt;
                lock (_reportedPatchLock)
                {
                    _birthAckConnectionNonce = connectionNonce;
                    nextAttempt = TryStartReportedPatchAttempt();
                }

                SendReportedPatchAttempt(nextAttempt);

                await CompleteAsync(outcome).ConfigureAwait(false);
            }
            finally
            {
                RaiseTwinEvents();
            }
        }

        // Checks the versions that this device holds against the versions in the given connection's birth ack, and returns
        // the desired properties resync to start (if any).
        private DesiredStateOutcome ReconcileHeldVersions(Guid? connectionNonce, ulong birthAckDesiredVersion, ulong birthAckReportedVersion)
        {
            RecordAuthoritativeVersions(connectionNonce, birthAckDesiredVersion, birthAckReportedVersion);

            // IoT hub's versions never decrease, so if this device holds a newer version than IoT hub, the version it holds is
            // not valid (for example, the application restored the wrong version, or the twin was recreated).
            if (birthAckReportedVersion != 0)
            {
                lock (_reportedStateLock)
                {
                    if (_localReportedVersion > birthAckReportedVersion)
                    {
                        Trace.TraceWarning("This device held version {0} of the reported properties, but IoT hub only has version {1}. This device no longer considers itself to hold the reported properties.", _localReportedVersion, birthAckReportedVersion);
                        SetLocalReportedState(0, null);
                        _transport.ReplaceReportedPropertiesVersion(0);
                    }
                }
            }

            DesiredStateOutcome outcome = new();
            if (birthAckDesiredVersion != 0)
            {
                lock (_desiredStateLock)
                {
                    DiscardDesiredResyncFromPreviousConnection(connectionNonce);
                    if (_localDesiredVersion > birthAckDesiredVersion)
                    {
                        Trace.TraceWarning("This device held version {0} of the desired properties, but IoT hub only has version {1}. Resyncing the desired properties.", _localDesiredVersion, birthAckDesiredVersion);
                        lock (_twinStateLock)
                        {
                            _localDesiredVersion = 0;
                            _isDesiredPropertiesComplete = false;
                            _isTwinStateChanged = true;
                        }

                        _transport.ReplaceDesiredPropertiesVersion(0);
                        if (_desiredResync == null)
                        {
                            StartDesiredResync(connectionNonce, null, outcome);
                        }
                    }
                }
            }

            return outcome;
        }

        // The device is now connecting as a different twin (for example, after being re-provisioned to another IoT hub or
        // device ID), whose versions aren't comparable with the ones this device holds, so discard its copy of the twin.
        // Its pending reported patches are kept, and sent to the new twin.
        void ITwinTransportHandler.HandleTwinIdentityChanged()
        {
            lock (_desiredStateLock)
            {
                CancelDesiredResync();
                _desiredVersionAtConnectionStart = 0;
                _reportedVersionAtConnectionStart = 0;
                lock (_twinStateLock)
                {
                    _localDesiredVersion = 0;
                    _desiredProperties = new JsonObject();
                    _isDesiredPropertiesComplete = true;
                    _isTwinStateChanged = true;
                }
            }

            lock (_reportedStateLock)
            {
                SetLocalReportedState(0, new JsonObject());
            }

            RaiseTwinEvents();
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
            // This engine may have been created after the current connection's birth ack arrived.
            Guid? connectionNonce = _transport.ConnectionNonce;
            BirthAckVersions? latestBirthAck = _transport.LatestBirthAck;
            if (connectionNonce != null && latestBirthAck?.ConnectionNonce == connectionNonce)
            {
                RecordAuthoritativeVersions(connectionNonce, latestBirthAck.DesiredVersion, latestBirthAck.ReportedVersion);
            }

            lock (_authoritativeVersionsLock)
            {
                if (_authoritativeVersions == null || _authoritativeVersions.ConnectionNonce != connectionNonce)
                {
                    return null;
                }

                ulong version = desired ? _authoritativeVersions.Desired : _authoritativeVersions.Reported;

                // IoT hub's versions are never 0, so 0 means IoT hub has not revealed this section's version yet.
                return version == 0 ? null : version;
            }
        }

        // Must be called while holding _desiredStateLock.
        private void ProcessDesiredPatch(ReceivedDesiredPatch patch, Guid? connectionNonce, DesiredStateOutcome outcome)
        {
            if (_desiredResync != null)
            {
                if (patch.Version <= _localDesiredVersion)
                {
                    return;
                }

                // Version probes are buffered too, since IoT hub may have answered the outstanding resync GET with an older
                // version. If so, replaying the probe resyncs again.

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

            if (patch.Properties == null)
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

            // Each delivery gets its own copy, since the application may change it.
            JsonObject desiredProperties = patch.Properties.DeepClone().AsObject();
            AdvanceLocalDesiredVersion(patch.Version, desiredProperties, isPatch: true);
            outcome.Deliveries.Add(new DesiredPatchReceivedEventArgs()
            {
                DesiredProperties = desiredProperties,
                DesiredPropertiesVersion = patch.Version,
            });
        }

        // Must be called while holding _desiredStateLock. Returns null if the snapshot is not newer than this device's view.
        private TwinPushSection? TryApplyDesiredSnapshot(ulong version, JsonObject properties)
        {
            if (version <= _localDesiredVersion)
            {
                if (version == _localDesiredVersion && !_isDesiredPropertiesComplete)
                {
                    AdvanceLocalDesiredVersion(version, properties, isPatch: false);
                }

                return null;
            }

            AdvanceLocalDesiredVersion(version, properties, isPatch: false);
            return new TwinPushSection()
            {
                Properties = properties,
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
                        // The patch takes the previous version to this one, so the properties are complete only if they were before.
                        ApplyMergePatch(_desiredProperties, properties);
                    }
                    else
                    {
                        _desiredProperties = properties.DeepClone().AsObject();
                        _isDesiredPropertiesComplete = true;
                    }

                    _isTwinStateChanged = true;
                }

                _transport.ObserveDesiredPropertiesVersion(version);
            }
            else if (!isPatch && version == _localDesiredVersion && !_isDesiredPropertiesComplete)
            {
                // This device only knew the number of this version. Now it holds the desired properties in full at it.
                lock (_twinStateLock)
                {
                    _desiredProperties = properties.DeepClone().AsObject();
                    _isDesiredPropertiesComplete = true;
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
                    _transport.ObserveReportedPropertiesVersion(version);
                }
                else if (version != 0 && version == _localReportedVersion)
                {
                    if (properties != null && !_isReportedPropertiesComplete)
                    {
                        // This device only knew the number of this version. Now it holds the reported properties in full at it.
                        SetLocalReportedState(version, properties);
                    }

                    // IoT hub revealed that it has this version, so this device's claim to it is valid.
                    _localReportedVersionConfirmedNonce = _transport.ConnectionNonce;
                }
            }
        }

        // A twin push on the current connection carries IoT hub's reported properties in full as of when it was sent, so this
        // device now holds exactly that version, even if it previously claimed to hold a newer (and therefore invalid) one.
        // Returns false if the push is stale instead: IoT hub already revealed a newer version on this connection (for example,
        // in a get twin response), which an older push doesn't invalidate.
        private bool SetLocalReportedVersionFromSnapshot(Guid? connectionNonce, ulong version, JsonObject properties)
        {
            lock (_reportedStateLock)
            {
                if (version >= _localReportedVersion)
                {
                    AdvanceLocalReportedVersion(version, properties);
                    return true;
                }

                if (connectionNonce != null && _localReportedVersionConfirmedNonce == connectionNonce)
                {
                    Trace.TraceInformation("Dropping the reported section of a twin push with version {0} because IoT hub already revealed version {1} on this connection.", version, _localReportedVersion);
                    return false;
                }

                Trace.TraceWarning("This device held version {0} of the reported properties, but IoT hub pushed version {1}. Adopting version {1}.", _localReportedVersion, version);
                SetLocalReportedState(version, properties);
                _transport.ReplaceReportedPropertiesVersion(version);
                return true;
            }
        }

        // Must be called while holding _desiredStateLock (so that the resync can't change). Reconciles this device's copy of the
        // reported properties with the full reported properties from a resync GET response. Returns null if it didn't change.
        private TwinPushSection? ReconcileReportedSnapshot(ulong version, JsonObject properties, ulong localVersionWhenSent)
        {
            if (version == 0)
            {
                return null;
            }

            lock (_reportedStateLock)
            {
                if (version == _localReportedVersion && _isReportedPropertiesComplete)
                {
                    return null;
                }

                if (version < _localReportedVersion)
                {
                    if (_localReportedVersion != localVersionWhenSent)
                    {
                        // This device's own patch was applied after IoT hub responded to the GET, so its copy is newer.
                        return null;
                    }

                    // IoT hub's versions never decrease, so the version this device held is not valid (for example, the
                    // application restored the wrong version, or the twin was recreated).
                    Trace.TraceWarning("This device held version {0} of the reported properties, but IoT hub only has version {1}. Adopting version {1}.", _localReportedVersion, version);
                    SetLocalReportedState(version, properties);
                    _transport.ReplaceReportedPropertiesVersion(version);
                }
                else
                {
                    AdvanceLocalReportedVersion(version, properties);
                }
            }

            return new TwinPushSection()
            {
                Properties = properties,
                PropertiesVersion = version,
            };
        }

        // Must be called while holding _reportedStateLock. The properties, if any, are the reported properties in full at this
        // version. Otherwise, the caller already updated them, and they are complete only if they were before (and the version isn't 0).
        private void SetLocalReportedState(ulong version, JsonObject? properties)
        {
            // Every version other than 0 that this engine sets is one that IoT hub revealed on the current connection.
            _localReportedVersionConfirmedNonce = version == 0 ? null : _transport.ConnectionNonce;
            lock (_twinStateLock)
            {
                _localReportedVersion = version;
                if (properties != null)
                {
                    _reportedProperties = properties.DeepClone().AsObject();
                    _isReportedPropertiesComplete = true;
                }
                else if (version == 0)
                {
                    _isReportedPropertiesComplete = false;
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
                DesiredVersion = _isDesiredPropertiesComplete ? _localDesiredVersion : 0,
                Desired = _desiredProperties.DeepClone().AsObject(),
                ReportedVersion = _isReportedPropertiesComplete ? _localReportedVersion : 0,
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

        // Raises the twin events that are due, one at a time and in order. Must be called without holding any of this engine's
        // other locks, so that handlers can call back into it. If called from a handler, the outer call raises them, and then
        // sends the reported patches that the handlers made.
        private void RaiseTwinEvents()
        {
            if (Monitor.IsEntered(_twinEventsLock))
            {
                return;
            }

            ReportedPatchAttempt? attempt = null;
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

                        if (completion == null && state == null)
                        {
                            // Every pending patch was in a snapshot that was raised, so the ones made by handlers can be sent.
                            foreach (PendingReportedPatch pendingPatch in _pendingReportedPatches)
                            {
                                pendingPatch.IsReleased = true;
                            }

                            attempt = TryStartReportedPatchAttempt();
                            break;
                        }
                    }

                    try
                    {
                        if (completion != null)
                        {
                            ReportedPatchCompleted?.Invoke(completion);
                        }
                        else
                        {
                            TwinStateChanged?.Invoke(state!);
                        }
                    }
                    catch (Exception e)
                    {
                        Trace.TraceWarning("A twin event handler threw an exception: {0}", e);
                    }
                }
            }

            SendReportedPatchAttempt(attempt);
        }

        // Gets the twin to resync it with IoT hub, unless that is already in progress.
        private async Task StartTwinResyncAsync(Guid? connectionNonce)
        {
            try
            {
                DesiredStateOutcome outcome = new();
                lock (_desiredStateLock)
                {
                    if (_isDisposed)
                    {
                        return;
                    }

                    DiscardDesiredResyncFromPreviousConnection(connectionNonce);
                    if (_desiredResync == null)
                    {
                        StartDesiredResync(connectionNonce, null, outcome);
                    }
                }

                await CompleteAsync(outcome).ConfigureAwait(false);
            }
            finally
            {
                RaiseTwinEvents();
            }
        }

        // Must be called while holding _desiredStateLock.
        private void StartDesiredResync(Guid? connectionNonce, ReceivedDesiredPatch? firstBufferedPatch, DesiredStateOutcome outcome)
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
            foreach (ReceivedDesiredPatch bufferedPatch in resync.Buffer.OrderBy(patch => patch.Version).ThenBy(patch => patch.Properties != null ? 0 : 1))
            {
                ProcessDesiredPatch(bufferedPatch, connectionNonce, outcome);
            }
        }

        // Must be called while holding _desiredStateLock.
        private void SendNewDesiredResyncGet(DesiredResync resync, DesiredStateOutcome outcome)
        {
            Guid getId = Guid.NewGuid();
            resync.GetId = getId;
            resync.DesiredVersionWhenSent = _localDesiredVersion;
            lock (_reportedStateLock)
            {
                resync.ReportedVersionWhenSent = _localReportedVersion;
            }

            resync.Timer?.Dispose();
            resync.Timer = new System.Threading.Timer(
                _ => HandleDesiredResyncTimeout(resync, getId),
                null,
                DesiredResyncTimeoutSchedule(resync.Attempt),
                Timeout.InfiniteTimeSpan);
            outcome.ResyncGetId = getId;
            outcome.ResyncCancellationToken = resync.Cancellation.Token;
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

                if (resync.ConnectionNonce != _transport.ConnectionNonce)
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

        // Returns false if the response isn't for the latest GET of the resync in progress.
        private async Task<bool> TryHandleDesiredResyncGetResponseAsync(Guid? connectionNonce, Guid getId, TwinResponse response)
        {
            DesiredStateOutcome outcome = new();
            lock (_desiredStateLock)
            {
                DiscardDesiredResyncFromPreviousConnection(connectionNonce);

                DesiredResync? resync = _desiredResync;
                if (resync == null || resync.GetId != getId)
                {
                    return false;
                }

                if (response.Status != TwinResponseStatus.Success)
                {
                    // Keep the resync in progress, so that its GET is sent again once its defensive timeout elapses.
                    Trace.TraceWarning("IoT hub responded to the GET request for resyncing the desired properties with status {0}. Sending it again later.", response.StatusCode);
                    if (response.Status == TwinResponseStatus.Transient)
                    {
                        resync.Timer?.Change(DesiredResyncTimeoutSchedule(resync.Attempt), Timeout.InfiniteTimeSpan);
                    }

                    return true;
                }

                TwinSnapshot snapshot;
                try
                {
                    snapshot = response.ParseSnapshot!();
                }
                catch (Exception e)
                {
                    Trace.TraceWarning("Received a malformed get twin response while resyncing the desired properties. Sending it again later: {0}", e);
                    return true;
                }

                ApplyTwinSnapshot(snapshot, resync.DesiredVersionWhenSent, resync.ReportedVersionWhenSent, connectionNonce, outcome);
            }

            await CompleteAsync(outcome).ConfigureAwait(false);
            return true;
        }

        // Reconciles with the twin that the connection got from IoT hub as it connected, exactly like the response to a resync
        // GET, delivers the resulting events, and then sends the reported patches held while the device wasn't connected.
        private async Task ApplyConnectTwinSnapshotAsync(Guid? connectionNonce, TwinSnapshot connectTwin)
        {
            try
            {
                // The connection's twin is shared with any other feature client of the connection, so this engine works on its own copy.
                TwinSnapshot snapshot = new(
                    connectTwin.Desired?.DeepClone().AsObject(),
                    connectTwin.DesiredVersion,
                    connectTwin.Reported?.DeepClone().AsObject(),
                    connectTwin.ReportedVersion);

                DesiredStateOutcome outcome = new();
                lock (_desiredStateLock)
                {
                    if (_isDisposed || connectionNonce != _transport.ConnectionNonce || _connectTwinAppliedNonce == connectionNonce)
                    {
                        return;
                    }

                    _connectTwinAppliedNonce = connectionNonce;
                    DiscardDesiredResyncFromPreviousConnection(connectionNonce);

                    // The connection got this twin after it started, so a section whose version changed since then was changed by
                    // IoT hub on this connection (for example, by a desired patch that arrived before the connection was ready),
                    // and an older version in the snapshot doesn't invalidate it.
                    ApplyTwinSnapshot(snapshot, _desiredVersionAtConnectionStart, _reportedVersionAtConnectionStart, connectionNonce, outcome);
                }

                // The application sees the reconciled twin before any held reported patch is sent, like with a GET on ready.
                await CompleteAsync(outcome).ConfigureAwait(false);

                // IoT hub's reported properties are now reconciled, so the held reported patches can be sent.
                ReportedPatchAttempt? nextAttempt;
                lock (_reportedPatchLock)
                {
                    _connectTwinReconciledNonce = connectionNonce;
                    nextAttempt = TryStartReportedPatchAttempt();
                }

                SendReportedPatchAttempt(nextAttempt);
            }
            finally
            {
                RaiseTwinEvents();
            }
        }

        // Must be called while holding _desiredStateLock. Reconciles this device's copy of its twin with a full twin from IoT hub
        // (the response to a GET), given this device's versions of the twin sections when that GET was sent, then ends the
        // resync (if any).
        private void ApplyTwinSnapshot(TwinSnapshot snapshot, ulong desiredVersionWhenSent, ulong reportedVersionWhenSent, Guid? connectionNonce, DesiredStateOutcome outcome)
        {
            RecordAuthoritativeVersions(connectionNonce, snapshot.DesiredVersion, snapshot.ReportedVersion);

            TwinPushReceivedEventArgs twinPushArgs = new()
            {
                // Only a transport that doesn't push the twin gets the reported properties when resyncing.
                Reported = snapshot.Reported != null && !_transport.AnnouncesTwinVersions
                    ? ReconcileReportedSnapshot(snapshot.ReportedVersion, snapshot.Reported, reportedVersionWhenSent)
                    : null,
            };

            if (snapshot.Desired != null && (snapshot.DesiredVersion != 0 || _transport.AnnouncesTwinVersions))
            {
                if (!_transport.AnnouncesTwinVersions
                    && snapshot.DesiredVersion != 0
                    && snapshot.DesiredVersion < _localDesiredVersion
                    && _localDesiredVersion == desiredVersionWhenSent)
                {
                    // IoT hub's versions never decrease, so the version this device held is not valid. A transport that
                    // announces twin versions finds this out from the birth ack instead.
                    Trace.TraceWarning("This device held version {0} of the desired properties, but IoT hub only has version {1}. Adopting version {1}.", _localDesiredVersion, snapshot.DesiredVersion);
                    lock (_twinStateLock)
                    {
                        _localDesiredVersion = 0;
                        _isDesiredPropertiesComplete = false;
                        _isTwinStateChanged = true;
                    }

                    _transport.ReplaceDesiredPropertiesVersion(0);
                }

                twinPushArgs.Desired = TryApplyDesiredSnapshot(snapshot.DesiredVersion, snapshot.Desired);
            }

            if (twinPushArgs.Desired != null || twinPushArgs.Reported != null)
            {
                outcome.Deliveries.Add(twinPushArgs);
            }

            ResolveDesiredResync(connectionNonce, outcome);
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

            if (outcome.ResyncGetId is Guid getId)
            {
                await SendDesiredResyncGetAsync(getId, outcome.ResyncCancellationToken).ConfigureAwait(false);
            }
        }

        private async Task SendDesiredResyncGetAsync(Guid getId, CancellationToken cancellationToken)
        {
            try
            {
                // A transport that doesn't push the twin also reconciles the reported properties when resyncing.
                Trace.TraceInformation("Sending a get twin request to resync the desired properties");
                await _transport.PublishGetAsync(getId, getDesired: true, getReported: !_transport.AnnouncesTwinVersions, ifNotMatchDesired: 0, ifNotMatchReported: 0, cancellationToken).ConfigureAwait(false);
            }
            catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
            {
                // The resync already ended (for example, the connection was lost), so its request must not be sent.
            }
            catch (Exception e)
            {
                // The defensive timeout sends this request again later.
                Trace.TraceWarning("Failed to publish the GET request for resyncing the desired properties: {0}", e);
            }
        }

        private sealed class DesiredResync(Guid? connectionNonce)
        {
            public Guid? ConnectionNonce { get; } = connectionNonce;

            public Guid GetId { get; set; }

            public int Attempt { get; set; } = 1;

            public List<ReceivedDesiredPatch> Buffer { get; } = new();

            // This device's versions when the latest GET was sent, which tell whether they changed while it was outstanding.
            public ulong DesiredVersionWhenSent { get; set; }

            public ulong ReportedVersionWhenSent { get; set; }

            public System.Threading.Timer? Timer { get; set; }

            // Cancelled when the resync ends, so that a request still waiting to be sent is never sent.
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

            public CancellationToken ResyncCancellationToken { get; set; }
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

        // A reported properties patch that the application made, which is pending until IoT hub responds to an attempt carrying it.
        private sealed class PendingReportedPatch(JsonObject patch, bool hasWaiter, bool isReleased)
        {
            public JsonObject Patch { get; } = patch;

            // Guarded by _reportedPatchLock. False until a TwinStateChanged snapshot containing this patch has been raised (unless
            // it was restored from one). It isn't sent until then.
            public bool IsReleased { get; set; } = isReleased;

            // Guarded by _reportedPatchLock. Whether an attempt carrying this patch was ever started, so IoT hub may apply it.
            public bool HasBeenSent { get; set; }

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

        // Stops this engine: fails the requests that are still waiting and stops listening to the transport. Doesn't dispose
        // the connection.
        public void Dispose()
        {
            _isDisposed = true;
            _transport.Detach();
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

            string ownerName = _owner.GetType().Name;
            foreach (PendingReportedPatch pendingPatch in pendingPatches)
            {
                pendingPatch.Response?.TrySetException(new ObjectDisposedException(ownerName));
            }

            AbandonGetTwinExchanges(() => new ObjectDisposedException(ownerName));
        }
    }
}
