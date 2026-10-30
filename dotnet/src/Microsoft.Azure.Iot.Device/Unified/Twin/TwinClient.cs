// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Exceptions;
using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Models.Twin;
using Microsoft.Azure.Iot.Device.Mqtt;
using Microsoft.Azure.Iot.Device.Unified.Connection;
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
    /// The feature client that keeps this device's copy of its twin in sync with an IoT hub that uses MQTTv3, as the device
    /// twin specification recommends. Safe to use from multiple threads.
    /// </summary>
    /// <remarks>
    /// <para>
    /// This client tracks the versions of the twin that this device holds. It delivers desired properties updates in version
    /// order (fetching the twin when it misses one), and queues reported properties patches, sending them one at a time,
    /// across reconnects, until IoT hub responds. It also keeps a copy of the twin that the application can persist (see
    /// <see cref="TwinStateChanged"/>).
    /// </para>
    /// <para>
    /// IoT hubs that use MQTTv3 don't push the twin to the device when it connects, so the connection client gets the twin as
    /// part of each (re)connection, before the connection is ready. This client reconciles its copy with that twin (getting the
    /// twin itself if the connection client couldn't) before sending any held reported properties patches, and raises
    /// <see cref="TwinPushReceived"/> with the sections of it that changed.
    /// </para>
    /// <para>
    /// This client only works with IoT hubs that use MQTTv3. It ignores the twin messages of connections to IoT hubs that use MQTTv5.
    /// </para>
    /// </remarks>
    public class TwinClient : IDisposable
    {
        private volatile bool _isDisposed;

        private readonly IConnectionClient _connection;

        // Response topics to subscribe to
        internal const string ClassicTwinResponseTopic = "$iothub/twin/res/";
        internal const string ClassicTwinDesiredPropertiesPatchTopic = "$iothub/twin/PATCH/properties/desired/";
        private const string ClassicTwinTopicPrefix = "$iothub/twin/";
        private const string ClassicTwinResponseTopicPattern = @"\$iothub/twin/res/(\d+)/(\?.+)";
        private static readonly Regex s_twinResponseTopicRegex = new(ClassicTwinResponseTopicPattern, RegexOptions.Compiled);

        // Topics to publish to
        internal const string ClassicTwinGetTopicFormat = "$iothub/twin/GET/?$rid={0}";
        private const string ClassicTwinReportedPropertiesPatchTopicFormat = "$iothub/twin/PATCH/properties/reported/?$rid={0}";

        private const string RequestIdTopicKey = "$rid";
        internal const string VersionKey = "$version";

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

        // Incremented each time the connection to IoT hub is lost. IoT hubs that use MQTTv3 don't identify their connections,
        // so this tells the state of one connection from that of the next.
        private long _connectionEpoch;

        // The maximum number of desired patches buffered while resyncing the desired properties.
        internal const int DesiredResyncBufferCapacity = 8;

        // Guards the device's view of the desired properties (the desired section state machine).
        private readonly object _desiredStateLock = new();

        // The version of the desired properties that this device has most recently observed. 0 means the device has not
        // observed the desired properties yet. Unlike the resync state, this survives disconnects.
        private ulong _localDesiredVersion;

        // Guarded by _desiredStateLock. This device's versions of the twin sections when the current connection epoch started,
        // which is before the connection client got the twin on that connection (see ApplyConnectTwinSnapshotAsync).
        private ulong _desiredVersionAtConnectionStart;
        private ulong _reportedVersionAtConnectionStart;

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

        // Whether _reportedProperties are the reported properties in full at _localReportedVersion.
        private bool _isReportedPropertiesComplete = true;

        // Whether this device's copy of its twin (including its pending reported patches) changed since TwinStateChanged was last raised.
        private bool _isTwinStateChanged;

        // Guarded by _reportedPatchLock. The ReportedPatchCompleted events that are yet to be raised, in order.
        private readonly Queue<ReportedPatchCompletedEventArgs> _reportedPatchCompletions = new();

        // Held while raising TwinStateChanged and ReportedPatchCompleted, so that they are raised one at a time, in order.
        private readonly object _twinEventsLock = new();

        // Non-null while this client is getting the twin to resync its desired properties (and reconcile its reported
        // properties) with IoT hub.
        private DesiredResync? _desiredResync;

        // Guards _authoritativeVersions.
        private readonly object _authoritativeVersionsLock = new();

        // The latest versions of the twin sections that IoT hub has revealed on the current connection.
        private AuthoritativeVersions? _authoritativeVersions;

        /// <summary>
        /// How long to wait for the response to the given attempt (starting at 1) of a get twin request that this client
        /// sends to resync the twin before sending it again.
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
        /// last observed. If a patch is missed, this client gets the twin from IoT hub and raises
        /// <see cref="TwinPushReceived"/> with the desired properties instead, followed by any newer patches.
        /// </remarks>
        public event Action<DesiredPatchReceivedEventArgs>? DesiredPatchReceived;

        /// <summary>
        /// An event that executes whenever this client gets a copy of the twin from IoT hub that changes this device's copy.
        /// </summary>
        /// <remarks>
        /// IoT hubs that use MQTTv3 don't push the twin, so the twin is fetched each time the device (re)connects, and whenever
        /// this client misses a desired properties patch or learns that its copy of the reported properties is out of date.
        /// The desired section is only included if it is newer than the desired properties this client last observed. The
        /// reported section is only included if it differs from the version of the reported properties this client holds.
        /// </remarks>
        public event Action<TwinPushReceivedEventArgs>? TwinPushReceived;

        /// <summary>
        /// An event that executes whenever this device's copy of its twin changes, with a snapshot of it to persist.
        /// </summary>
        /// <remarks>
        /// <para>
        /// The SDK doesn't persist the twin. An application that persists each snapshot, and passes the latest one to
        /// <see cref="TwinClient(IConnectionClient, TwinState?)"/> when it starts again, only receives the twin sections that
        /// changed while it was stopped, and its reported properties patches that IoT hub hadn't applied are sent again.
        /// </para>
        /// <para>
        /// Snapshots are raised one at a time, in the order the changes were made, so the latest one raised is always the
        /// latest state. Changes made by a handler of this event (or of <see cref="ReportedPatchCompleted"/>) are raised once
        /// that handler returns, and reported properties patches made by such a handler are only sent once a snapshot containing
        /// them has been raised. The snapshot belongs to the handler, which may keep it.
        /// </para>
        /// </remarks>
        public event Action<TwinState>? TwinStateChanged;

        /// <summary>
        /// An event that executes whenever IoT hub responds to a reported properties patch that this client sent, or the patch
        /// couldn't be sent. Raised in the order the patches were sent.
        /// </summary>
        /// <remarks>
        /// Unlike awaiting <see cref="UpdateReportedPropertiesAsync"/>, this is raised for every patch, including those made by
        /// <see cref="EnqueueReportedPropertiesUpdate"/> or restored from a <see cref="TwinState"/>.
        /// </remarks>
        public event Action<ReportedPatchCompletedEventArgs>? ReportedPatchCompleted;

        /// <summary>
        /// The latest version of the desired properties that IoT hub has revealed on the current connection, or null if it
        /// hasn't revealed one yet.
        /// </summary>
        /// <remarks>
        /// IoT hub reveals its version of the desired properties in every desired properties patch and get twin response
        /// (including the one this client gets after each connection). This value is reset whenever the connection is lost.
        /// </remarks>
        public ulong? AuthoritativeDesiredPropertiesVersion => GetAuthoritativeVersion(desired: true);

        /// <summary>
        /// The latest version of the reported properties that IoT hub has revealed on the current connection, or null if it
        /// hasn't revealed one yet.
        /// </summary>
        /// <remarks>
        /// IoT hub reveals its version of the reported properties in every get twin response (including the one this client
        /// gets after each connection) and every successful reported properties update response. This value is reset whenever
        /// the connection is lost.
        /// </remarks>
        public ulong? AuthoritativeReportedPropertiesVersion => GetAuthoritativeVersion(desired: false);

        /// <summary>
        /// Construct a new <see cref="TwinClient"/> instance.
        /// </summary>
        /// <param name="connection">The connection client this feature client will use.</param>
        /// <remarks>
        /// The provided connection client does not need to be connected before this constructor is called. However, the provided connection client must be connected prior
        /// to using this feature client to send/receive any twin messages, except for reported properties patches, which are held until the device connects.
        /// If the connection is already ready when this client is constructed and the connection client got the twin when it
        /// connected, this client applies that twin right away, before any event handler can be added; use <see cref="GetTwinState"/>
        /// to read it.
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
        /// <exception cref="InvalidOperationException">Thrown if <paramref name="connection"/> opted out of twins (see <see cref="ConnectionClientOptions.EnableTwin"/>).</exception>
        public TwinClient(IConnectionClient connection)
            : this(connection, null)
        {
        }

        /// <summary>
        /// Construct a new <see cref="TwinClient"/> instance that starts from a copy of the twin that the application persisted.
        /// </summary>
        /// <param name="connection">The connection client this feature client will use. It must not be connected yet.</param>
        /// <param name="initialState">
        /// The latest snapshot raised by <see cref="TwinStateChanged"/> before the application last stopped, or null if the
        /// device holds no copy of its twin.
        /// </param>
        /// <remarks>
        /// The twin that this client gets once the device connects is compared against <paramref name="initialState"/>, so
        /// that <see cref="TwinPushReceived"/> only includes the twin sections that changed. Its pending reported properties
        /// patches are sent, in order, before any made afterwards. Their outcomes are raised by <see cref="ReportedPatchCompleted"/>.
        /// </remarks>
        /// <exception cref="InvalidOperationException">Thrown if <paramref name="connection"/> opted out of twins (see <see cref="ConnectionClientOptions.EnableTwin"/>).</exception>
        public TwinClient(IConnectionClient connection, TwinState? initialState)
        {
            ArgumentNullException.ThrowIfNull(connection);
            if (!connection.IsTwinEnabled)
            {
                throw new InvalidOperationException($"The provided connection client opted out of twins. Set {nameof(ConnectionClientOptions)}.{nameof(ConnectionClientOptions.EnableTwin)} to true to use a {nameof(TwinClient)}.");
            }

            _connection = connection;

            if (initialState != null)
            {
                _localDesiredVersion = initialState.DesiredVersion;
                _desiredProperties = initialState.Desired.DeepClone().AsObject();
                _localReportedVersion = initialState.ReportedVersion;
                _reportedProperties = initialState.Reported.DeepClone().AsObject();
                _isReportedPropertiesComplete = true;
                foreach (JsonObject patch in initialState.PendingReportedPatches)
                {
                    _pendingReportedPatches.Add(new PendingReportedPatch(patch.DeepClone().AsObject(), hasWaiter: false, isReleased: true));
                }
            }

            _desiredVersionAtConnectionStart = _localDesiredVersion;
            _reportedVersionAtConnectionStart = _localReportedVersion;

            _connection.PublishReceivedAsync += HandleReceivedMqttPublishAsync;
            _connection.ConnectionLost += HandleConnectionLost;
            _connection.ConnectionReady += HandleConnectionReady;

            if (IsReadyForTwinMessages() && TryGetConnectTwinSnapshot() != null)
            {
                // The connection became ready before this client was created, and the connection client got the twin then, so
                // this client applies it now (before any handler can be added; see GetTwinState) rather than waiting for the
                // next connection.
                HandleConnectionReady();
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
        /// Get the full twin, or one of its sections.
        /// </summary>
        /// <param name="getReported">Include the reported properties in the returned twin.</param>
        /// <param name="getDesired">Include the desired properties in the returned twin.</param>
        /// <param name="cancellationToken">The cancellation token.</param>
        /// <returns>The returned twin. Both versions are always set.</returns>
        /// <remarks>
        /// <para>
        /// IoT hubs that use MQTTv3 always return the full twin and don't support conditional get twin requests, so the
        /// sections that weren't requested are removed by this client.
        /// </para>
        /// <para>
        /// Concurrent calls with identical arguments share a single request to IoT hub and all receive its response. If IoT hub
        /// doesn't respond within a defensive timeout (5 to 15 minutes, growing with each attempt), or responds that it is
        /// throttling or failing (status 429 or 5xx), the request is sent again once that timeout elapses.
        /// </para>
        /// </remarks>
        /// <exception cref="ArgumentException">Thrown when neither desired nor reported properties are requested.</exception>
        /// <exception cref="NotSupportedException">Thrown if the device isn't connected to an IoT hub that uses MQTTv3.</exception>
        /// <exception cref="DeviceException">
        /// Thrown if IoT hub rejected this get twin request, or with <see cref="ErrorRetryability.Retryable"/> if the
        /// connection to IoT hub was lost before the response arrived. The request may be retried once reconnected.
        /// </exception>
        public async Task<DeviceTwin> GetTwinAsync(bool getReported = true, bool getDesired = true, CancellationToken cancellationToken = default)
        {
            ObjectDisposedException.ThrowIf(_isDisposed, this);
            if (!getReported && !getDesired)
            {
                throw new ArgumentException("At least one twin section must be requested.");
            }

            EnsureConnected();

            GetTwinRequest request = new(getDesired, getReported);
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
        /// <param name="cancellationToken">
        /// The cancellation token. Cancelling stops waiting for the response. The patch is discarded if it hasn't been sent
        /// yet. Otherwise, IoT hub may still apply it.
        /// </param>
        /// <returns>
        /// IoT hub's response to the patch that carried these changes: <see cref="Result.Ok"/> with the new version of the
        /// reported properties, or the reason why IoT hub rejected the changes (<see cref="Result.PayloadInvalid"/> for status
        /// 400, <see cref="Result.PayloadTooLarge"/> for status 413 and <see cref="Result.InternalError"/> for any other status
        /// that isn't retried).
        /// </returns>
        /// <remarks>
        /// <para>
        /// This client sends one reported properties patch at a time. IoT hubs that use MQTTv3 don't support conditional
        /// patches, so each patch is applied unconditionally. Patches made while another patch is in flight (or while the device
        /// is reconnecting) are combined, in order, into the next patch that this client sends, so they share its response.
        /// Patches that can't be combined into one JSON merge patch (such as one that removes a property followed by one that
        /// sets some of that property's children) are sent one after another.
        /// </para>
        /// <para>
        /// If IoT hub doesn't respond within a defensive timeout (5 to 15 minutes, growing with each attempt), or responds that
        /// it is throttling or failing (status 429 or 5xx), the patch is sent again once that timeout elapses. If the connection
        /// is lost, the patch is sent again once the device has reconnected. Since these patches are unconditional, IoT hub may
        /// apply a patch that is sent again more than once.
        /// </para>
        /// <para>
        /// The device doesn't need to be connected to call this method. Patches made before the device connects (or while it is
        /// disconnected) are held, in order, and sent once the device has connected. Patches are only discarded if
        /// <paramref name="cancellationToken"/> is cancelled before they are sent, or if this client is disposed.
        /// </para>
        /// </remarks>
        /// <exception cref="ObjectDisposedException">Thrown if this client is disposed before IoT hub responds.</exception>
        public Task<ReportedPatchResponse> UpdateReportedPropertiesAsync(ReportedPatchRequest patch, CancellationToken cancellationToken = default)
        {
            ArgumentNullException.ThrowIfNull(patch);
            return UpdateReportedPropertiesAsync(patch.ReportedProperties, cancellationToken);
        }

        private async Task<ReportedPatchResponse> UpdateReportedPropertiesAsync(JsonObject reportedProperties, CancellationToken cancellationToken)
        {
            ObjectDisposedException.ThrowIf(_isDisposed, this);
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

        /// <summary>
        /// Queue an update to this device's reported properties, without waiting for IoT hub to apply it.
        /// </summary>
        /// <param name="reportedProperties">The patch of the reported properties to send to IoT hub.</param>
        /// <remarks>
        /// <para>
        /// The patch is sent exactly as by <see cref="UpdateReportedPropertiesAsync"/>, including when the device isn't
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
            ObjectDisposedException.ThrowIf(_isDisposed, this);
            ArgumentNullException.ThrowIfNull(reportedProperties);
            AddPendingReportedPatch(reportedProperties, hasWaiter: false);
        }

        private PendingReportedPatch AddPendingReportedPatch(JsonObject reportedProperties, bool hasWaiter)
        {
            // A patch made by a twin event handler is only sent once the outer call to RaiseTwinEvents has raised a snapshot
            // containing it, so that it is persisted before being sent.
            bool isReentrant = Monitor.IsEntered(_twinEventsLock);

            // Copy the patch so that later changes the application makes to it don't affect what is sent.
            PendingReportedPatch pendingPatch = new(reportedProperties.DeepClone().AsObject(), hasWaiter, isReleased: !isReentrant);
            ReportedPatchAttempt? attempt;
            lock (_reportedPatchLock)
            {
                ObjectDisposedException.ThrowIf(_isDisposed, this);
                _pendingReportedPatches.Add(pendingPatch);
                MarkTwinStateChanged();
                attempt = TryStartReportedPatchAttempt();
            }

            RaiseTwinEvents();
            SendReportedPatchAttempt(attempt);
            return pendingPatch;
        }

        // Must be called while holding _reportedPatchLock. Starts the next reported patch attempt from the pending patches, unless
        // one is already in flight, there are none, or the device isn't connected to an IoT hub that uses MQTTv3. Patches held
        // while the device isn't connected are sent once the connection is ready.
        private ReportedPatchAttempt? TryStartReportedPatchAttempt()
        {
            if (_isDisposed
                || _reportedPatchAttempt != null
                || _pendingReportedPatches.Count == 0
                || !_pendingReportedPatches[0].IsReleased
                || !IsReadyForTwinMessages())
            {
                return null;
            }

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

            ReportedPatchAttempt attempt = new(Guid.NewGuid(), patches, JsonSerializer.SerializeToUtf8Bytes(payload));
            attempt.Timer = new System.Threading.Timer(
                _ => HandleReportedPatchTimeout(attempt),
                null,
                ReportedPatchTimeoutSchedule(_reportedPatchTimeoutAttempt),
                Timeout.InfiniteTimeSpan);
            _reportedPatchAttempt = attempt;
            return attempt;
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
                MqttPublish publish = new()
                {
                    Topic = string.Format(CultureInfo.InvariantCulture, ClassicTwinReportedPropertiesPatchTopicFormat, attempt.PatchId),
                    QualityOfServiceLevel = MqttQualityOfServiceLevel.AtLeastOnce,
                    Payload = attempt.Payload,
                };

                Trace.TraceInformation("Publishing 'PatchReported' request on topic " + publish.Topic);
                await _connection.PublishAsync(publish, attempt.Cancellation.Token).ConfigureAwait(false);
            }
            catch (OperationCanceledException) when (attempt.Cancellation.IsCancellationRequested)
            {
                // The attempt already ended (for example, the connection was lost), so it must not be sent.
            }
            catch (DeviceException e) when (e.Retryability == ErrorRetryability.Retryable)
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
        private async Task<bool> TryHandleReportedPatchResponseAsync(Guid patchId, int status, ulong version, long connectionEpoch)
        {
            ReportedPatchAttempt? nextAttempt;
            ReportedPatchResponse response;
            List<PendingReportedPatch> resolvedPatches;
            bool isReportedStateStale = false;
            lock (_reportedPatchLock)
            {
                ReportedPatchAttempt? attempt = _reportedPatchAttempt;
                if (attempt == null || attempt.PatchId != patchId)
                {
                    return false;
                }

                if (IsTransientStatus(status))
                {
                    // Keep the attempt in flight, so that it is sent again once its defensive timeout elapses.
                    Trace.TraceWarning("IoT hub responded to a reported properties patch with status {0}. Sending it again later.", status);
                    attempt.Timer?.Change(ReportedPatchTimeoutSchedule(_reportedPatchTimeoutAttempt), Timeout.InfiniteTimeSpan);
                    return true;
                }

                response = new ReportedPatchResponse()
                {
                    Result = MapReportedPatchStatus(status),
                    Version = version,
                };

                if (response.Result == Result.Ok)
                {
                    RecordAuthoritativeVersions(connectionEpoch, 0, version);
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

                        // If this device held the version before this patch, it now holds them at the patched version (in full, if it
                        // held them in full before). Otherwise, it still doesn't hold them in full.
                        if (_localReportedVersion != 0 && version == _localReportedVersion + 1)
                        {
                            AdvanceLocalReportedVersion(version);
                        }
                        else if (_localReportedVersion != 0 && version > _localReportedVersion + 1)
                        {
                            // Someone else changed the reported properties in between, so this device's copy lacks their changes.
                            Trace.TraceWarning("This device held version {0} of the reported properties, but IoT hub applied its patch at version {1}. This device no longer considers itself to hold the reported properties, and gets the twin again.", _localReportedVersion, version);
                            SetLocalReportedState(0, null);
                            isReportedStateStale = true;
                        }
                    }
                }
                else
                {
                    Trace.TraceWarning("IoT hub rejected a reported properties patch with status {0}.", status);
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

            if (isReportedStateStale)
            {
                await StartTwinResyncAsync(connectionEpoch).ConfigureAwait(false);
            }

            return true;
        }

        private static JsonObject ParseAttemptPatch(ReportedPatchAttempt attempt)
        {
            return JsonNode.Parse(attempt.Payload)!.AsObject();
        }

        internal static bool IsSuccessStatus(int status)
        {
            return status >= 200 && status < 300;
        }

        // IoT hub is throttling or failing, so the request may succeed if it is sent again later.
        private static bool IsTransientStatus(int status)
        {
            return status == 429 || status >= 500;
        }

        private static Result MapReportedPatchStatus(int status)
        {
            return status switch
            {
                >= 200 and < 300 => Result.Ok,
                400 => Result.PayloadInvalid,
                413 => Result.PayloadTooLarge,
                _ => Result.InternalError,
            };
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
            try
            {
                await PublishGetAsync(getId, exchange.Cancellation.Token).ConfigureAwait(false);
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

        private async Task PublishGetAsync(Guid getId, CancellationToken cancellationToken)
        {
            MqttPublish publish = new()
            {
                Topic = string.Format(CultureInfo.InvariantCulture, ClassicTwinGetTopicFormat, getId),
                QualityOfServiceLevel = MqttQualityOfServiceLevel.AtLeastOnce,
            };

            Trace.TraceInformation("Publishing 'GetTwin' request on topic " + publish.Topic);

            // Puback is checked for non-success cases under this layer, so no need to check it here as well
            await _connection.PublishAsync(publish, cancellationToken).ConfigureAwait(false);
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
        private bool TryHandleGetTwinResponse(Guid getId, int status, byte[] payload, long connectionEpoch)
        {
            GetTwinExchange? exchange;
            lock (_getTwinExchangesLock)
            {
                if (!_getTwinExchangesById.TryGetValue(getId, out exchange))
                {
                    return false;
                }

                if (IsTransientStatus(status))
                {
                    // Keep the exchange outstanding, so that it is sent again once its defensive timeout elapses.
                    Trace.TraceWarning("IoT hub responded to a get twin request with status {0}. Sending it again later.", status);
                    exchange.Timer?.Change(GetTwinTimeoutSchedule(exchange.Attempt), Timeout.InfiniteTimeSpan);
                    return true;
                }

                EndGetTwinExchange(exchange);
            }

            if (!IsSuccessStatus(status))
            {
                exchange.Response.TrySetException(new DeviceException($"IoT hub rejected the get twin request with status {status}.")
                {
                    Retryability = ErrorRetryability.Terminal,
                    IsContained = true,
                });
                return true;
            }

            TwinSnapshot snapshot;
            try
            {
                snapshot = ParseGetTwinResponsePayload(payload);
            }
            catch (Exception e)
            {
                exchange.Response.TrySetException(e);
                return true;
            }

            RecordAuthoritativeVersions(connectionEpoch, snapshot.DesiredVersion, snapshot.ReportedVersion);

            GetTwinRequest request = exchange.Request;
            DeviceTwin twin = new()
            {
                Desired = request.GetDesired ? snapshot.Desired : null,
                Reported = request.GetReported ? snapshot.Reported : null,
                DesiredVersion = snapshot.DesiredVersion,
                ReportedVersion = snapshot.ReportedVersion,
            };

            if (request.GetDesired && snapshot.DesiredVersion != 0)
            {
                lock (_desiredStateLock)
                {
                    // The application fetched the desired properties, so this device's view of them is now at least this version.
                    AdvanceLocalDesiredVersion(snapshot.DesiredVersion, snapshot.Desired, isPatch: false);
                }
            }

            if (request.GetReported && snapshot.ReportedVersion != 0)
            {
                AdvanceLocalReportedVersion(snapshot.ReportedVersion, snapshot.Reported);
            }

            exchange.Response.TrySetResult(twin);
            return true;
        }

        // Twin exchanges can't outlive the connection they were sent on, since IoT hub never sends their responses on a later one.
        private void HandleConnectionLost()
        {
            Interlocked.Increment(ref _connectionEpoch);

            AbandonGetTwinExchanges(() => new DeviceException("The connection to IoT hub was lost before the response to this twin request arrived. It may be retried once reconnected.")
            {
                Retryability = ErrorRetryability.Retryable,
                IsContained = true,
            });

            // Reported patches are kept and sent again once the connection is ready again. IoT hub may have applied the attempt in
            // flight, in which case it applies it again.
            lock (_reportedPatchLock)
            {
                if (_reportedPatchAttempt != null)
                {
                    EndReportedPatchAttempt(_reportedPatchAttempt);
                }

                _reportedPatchTimeoutAttempt = 1;
            }

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

        // IoT hubs that use MQTTv3 don't push the twin when the device connects, so the connection client gets it as part of the
        // device presence flow instead. Reconcile with it (or get it, if the connection client couldn't), then send the reported
        // patches held while the device wasn't connected, like the MQTTv5 birth flow.
        private void HandleConnectionReady()
        {
            if (_isDisposed || !IsReadyForTwinMessages())
            {
                return;
            }

            long connectionEpoch = Interlocked.Read(ref _connectionEpoch);
            ConnectTwinSnapshot? snapshot = TryGetConnectTwinSnapshot();
            if (snapshot != null)
            {
                // This raises the twin events, and sends the reported patches once they have been raised.
                _ = ApplyConnectTwinSnapshotAsync(snapshot, connectionEpoch);
            }

            ReportedPatchAttempt? nextAttempt;
            lock (_reportedPatchLock)
            {
                nextAttempt = TryStartReportedPatchAttempt();
            }

            SendReportedPatchAttempt(nextAttempt);

            if (snapshot == null)
            {
                _ = StartTwinResyncAsync(connectionEpoch);
            }
        }

        // Returns the twin that the connection client got on the current connection, or null if it didn't get one.
        private ConnectTwinSnapshot? TryGetConnectTwinSnapshot()
        {
            ConnectTwinSnapshot? snapshot = _connection.LatestConnectTwinSnapshot;
            if (snapshot == null)
            {
                return null;
            }

            if (snapshot.ConnectionId != _connection.HubConnectionId)
            {
                Trace.TraceInformation("Ignoring the twin that the connection client got on a previous connection.");
                return null;
            }

            return snapshot;
        }

        // Applies the twin that the connection client got on this connection, exactly like the response to a resync GET.
        private async Task ApplyConnectTwinSnapshotAsync(ConnectTwinSnapshot connectSnapshot, long connectionEpoch)
        {
            try
            {
                // The snapshot is shared with any other feature client of the connection, so this client works on its own copy.
                TwinSnapshot snapshot = new(
                    connectSnapshot.Desired.DeepClone().AsObject(),
                    connectSnapshot.DesiredVersion,
                    connectSnapshot.Reported.DeepClone().AsObject(),
                    connectSnapshot.ReportedVersion);

                DesiredStateOutcome outcome = new();
                lock (_desiredStateLock)
                {
                    if (_isDisposed || connectionEpoch != Interlocked.Read(ref _connectionEpoch))
                    {
                        return;
                    }

                    DiscardDesiredResyncFromPreviousConnection(connectionEpoch);

                    // The connection client sent its GET after this connection epoch started, so a section whose version
                    // changed since then was changed by IoT hub on this connection (for example, by a desired patch that
                    // arrived before the connection was ready), and an older version in the snapshot is not adopted.
                    ApplyTwinSnapshot(snapshot, _desiredVersionAtConnectionStart, _reportedVersionAtConnectionStart, connectionEpoch, outcome);
                }

                await CompleteAsync(outcome).ConfigureAwait(false);
            }
            finally
            {
                RaiseTwinEvents();
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

        private void EnsureConnected()
        {
            ConnectionContext? connectionContext = _connection.GetCurrentConnectionContext();
            if (connectionContext == null || !_connection.IsConnectionTargetSet)
            {
                throw new NotSupportedException("Must be connected before calling this method.");
            }

            if (connectionContext.ConnectionProfile == IotHubConnectionType.Mqttv5)
            {
                throw new NotSupportedException($"{nameof(TwinClient)} only supports IoT hubs that use MQTTv3.");
            }
        }

        private bool IsReadyForTwinMessages()
        {
            return _connection.IsConnectionReady
                && _connection.GetCurrentConnectionContext()?.ConnectionProfile != IotHubConnectionType.Mqttv5;
        }

        private async Task HandleReceivedMqttPublishAsync(MqttPublishReceivedEventArgs args)
        {
            string topic = args.Publish.Topic;
            if (!topic.StartsWith(ClassicTwinTopicPrefix, StringComparison.Ordinal))
            {
                // The publish is not relevant to this client, so ignore it. This check needs to happen prior to checking the deviceId within the topic b/c deviceId is
                // not available until after provisioning finishes and this client may be setup prior to provisioning. This allows this client to ignore DPS
                // publishes without needing to know the deviceId.
                return;
            }

            if (_isDisposed)
            {
                return;
            }

            ConnectionContext? connectionContext = _connection.GetCurrentConnectionContext();
            if (connectionContext == null)
            {
                Trace.TraceWarning("Cannot handle a received MQTT message while disconnected");
                return;
            }

            if (connectionContext.ConnectionProfile == IotHubConnectionType.Mqttv5)
            {
                // This client only handles the twin messages of IoT hubs that use MQTTv3.
                return;
            }

            long connectionEpoch = Interlocked.Read(ref _connectionEpoch);

            // Note that all twin response and desired property update messages are QoS 0, so no need to ack the MQTT message here
            try
            {
                if (topic.StartsWith(ClassicTwinResponseTopic, StringComparison.Ordinal))
                {
                    if (!TryParseResponseTopic(topic, out Guid requestId, out int status, out ulong version))
                    {
                        Trace.TraceWarning("Dropping a twin response with an unexpected topic: " + topic);
                        return;
                    }

                    Trace.TraceInformation("Received twin response message on topic " + topic);
                    if (!TryHandleGetTwinResponse(requestId, status, args.Publish.Payload, connectionEpoch)
                        && !await TryHandleDesiredResyncGetResponseAsync(requestId, status, args.Publish.Payload, connectionEpoch).ConfigureAwait(false)
                        && !await TryHandleReportedPatchResponseAsync(requestId, status, version, connectionEpoch).ConfigureAwait(false))
                    {
                        Trace.TraceInformation("Dropping a twin response that does not match any outstanding twin request.");
                    }
                }
                else if (topic.StartsWith(ClassicTwinDesiredPropertiesPatchTopic, StringComparison.Ordinal))
                {
                    await HandleDesiredPatchAsync(topic, args.Publish.Payload, connectionEpoch).ConfigureAwait(false);
                }
            }
            catch (Exception e) when (e is JsonException or InvalidOperationException or FormatException)
            {
                Trace.TraceWarning("Dropping a malformed twin message received on topic {0}: {1}", topic, e);
            }
            finally
            {
                RaiseTwinEvents();
            }
        }

        private async Task HandleDesiredPatchAsync(string topic, byte[] payload, long connectionEpoch)
        {
            JsonObject desiredProperties = payload.Length == 0 ? new JsonObject() : JsonNode.Parse(payload)!.AsObject();
            ulong version = TakeVersion(desiredProperties);
            if (version == 0)
            {
                version = ParseVersionFromTopic(topic);
            }

            if (version == 0)
            {
                Trace.TraceWarning("Dropping a desired property patch without a version.");
                return;
            }

            RecordAuthoritativeVersions(connectionEpoch, version, 0);

            DesiredStateOutcome outcome = new();
            lock (_desiredStateLock)
            {
                DiscardDesiredResyncFromPreviousConnection(connectionEpoch);
                ProcessDesiredPatch(new DesiredPatch(version, desiredProperties), connectionEpoch, outcome);
            }

            await CompleteAsync(outcome).ConfigureAwait(false);
        }

        private void RecordAuthoritativeVersions(long connectionEpoch, ulong desiredVersion, ulong reportedVersion)
        {
            lock (_authoritativeVersionsLock)
            {
                if (_authoritativeVersions == null || _authoritativeVersions.ConnectionEpoch != connectionEpoch)
                {
                    _authoritativeVersions = new AuthoritativeVersions(connectionEpoch);
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
                if (_authoritativeVersions == null || _authoritativeVersions.ConnectionEpoch != Interlocked.Read(ref _connectionEpoch))
                {
                    return null;
                }

                ulong version = desired ? _authoritativeVersions.Desired : _authoritativeVersions.Reported;

                // IoT hub's versions are never 0, so 0 means IoT hub has not revealed this section's version yet.
                return version == 0 ? null : version;
            }
        }

        // Must be called while holding _desiredStateLock.
        private void ProcessDesiredPatch(DesiredPatch patch, long connectionEpoch, DesiredStateOutcome outcome)
        {
            if (_desiredResync != null)
            {
                if (patch.Version <= _localDesiredVersion)
                {
                    return;
                }

                if (_desiredResync.Buffer.Count >= DesiredResyncBufferCapacity)
                {
                    // The response to a new GET will include every patch buffered so far (and this one), so they can be dropped.
                    Trace.TraceWarning("Too many desired property patches were received while resyncing the desired properties. Requesting the twin again.");
                    _desiredResync.Buffer.Clear();
                    SendNewDesiredResyncGet(_desiredResync, outcome);
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
                StartDesiredResync(connectionEpoch, patch, outcome);
                return;
            }

            AdvanceLocalDesiredVersion(patch.Version, patch.Properties, isPatch: true);
            outcome.Deliveries.Add(new DesiredPatchReceivedEventArgs()
            {
                DesiredProperties = patch.Properties,
                DesiredPropertiesVersion = patch.Version,
            });
        }

        // Must be called while holding _desiredStateLock. Returns null if the snapshot is not newer than this device's view.
        private TwinPushSection? TryApplyDesiredSnapshot(ulong version, JsonObject properties)
        {
            if (version <= _localDesiredVersion)
            {
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
            if (version <= _localDesiredVersion)
            {
                return;
            }

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

        // The properties, if any, are the reported properties in full at this version. Otherwise, the caller already updated them.
        private void AdvanceLocalReportedVersion(ulong version, JsonObject? properties = null)
        {
            lock (_reportedStateLock)
            {
                if (version > _localReportedVersion)
                {
                    SetLocalReportedState(version, properties);
                }
                else if (properties != null && version == _localReportedVersion && !_isReportedPropertiesComplete)
                {
                    // This device only knew the number of this version. Now it holds the reported properties in full at it.
                    SetLocalReportedState(version, properties);
                }
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
                }

                SetLocalReportedState(version, properties);
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
                DesiredVersion = _localDesiredVersion,
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

        // Raises the twin events that are due, one at a time and in order. Must be called without holding any of this client's
        // other locks, so that handlers can call back into this client. If called from a handler, the outer call raises them,
        // and then sends the reported patches that the handlers made.
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
        private async Task StartTwinResyncAsync(long connectionEpoch)
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

                    DiscardDesiredResyncFromPreviousConnection(connectionEpoch);
                    if (_desiredResync == null)
                    {
                        StartDesiredResync(connectionEpoch, null, outcome);
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
        private void StartDesiredResync(long connectionEpoch, DesiredPatch? firstBufferedPatch, DesiredStateOutcome outcome)
        {
            _desiredResync = new DesiredResync(connectionEpoch);
            if (firstBufferedPatch != null)
            {
                _desiredResync.Buffer.Add(firstBufferedPatch);
            }

            SendNewDesiredResyncGet(_desiredResync, outcome);
        }

        // Must be called while holding _desiredStateLock. Ends the resync (if any) and applies its buffered patches that are newer than this device's view.
        private void ResolveDesiredResync(long connectionEpoch, DesiredStateOutcome outcome)
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
                ProcessDesiredPatch(bufferedPatch, connectionEpoch, outcome);
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
            outcome.ResyncGet = new ResyncGet(getId, resync.Cancellation.Token);
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
        private void DiscardDesiredResyncFromPreviousConnection(long connectionEpoch)
        {
            if (_desiredResync != null && _desiredResync.ConnectionEpoch != connectionEpoch)
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

                if (resync.ConnectionEpoch != Interlocked.Read(ref _connectionEpoch))
                {
                    CancelDesiredResync();
                    return;
                }

                Trace.TraceWarning("Timed out waiting for the response to the get twin request for resyncing the twin. Sending it again.");
                resync.Attempt++;
                SendNewDesiredResyncGet(resync, outcome);
            }

            _ = CompleteAsync(outcome);
        }

        // Returns false if the response isn't for the latest GET of the resync in progress.
        private async Task<bool> TryHandleDesiredResyncGetResponseAsync(Guid getId, int status, byte[] payload, long connectionEpoch)
        {
            DesiredStateOutcome outcome = new();
            lock (_desiredStateLock)
            {
                DiscardDesiredResyncFromPreviousConnection(connectionEpoch);

                DesiredResync? resync = _desiredResync;
                if (resync == null || resync.GetId != getId)
                {
                    return false;
                }

                if (!IsSuccessStatus(status))
                {
                    // Keep the resync in progress, so that its GET is sent again once its defensive timeout elapses.
                    Trace.TraceWarning("IoT hub responded to the get twin request for resyncing the twin with status {0}. Sending it again later.", status);
                    if (IsTransientStatus(status))
                    {
                        resync.Timer?.Change(DesiredResyncTimeoutSchedule(resync.Attempt), Timeout.InfiniteTimeSpan);
                    }

                    return true;
                }

                TwinSnapshot snapshot;
                try
                {
                    snapshot = ParseGetTwinResponsePayload(payload);
                }
                catch (Exception e) when (e is JsonException or InvalidOperationException or FormatException)
                {
                    Trace.TraceWarning("Received a malformed get twin response while resyncing the twin. Sending it again later: {0}", e);
                    return true;
                }

                ApplyTwinSnapshot(snapshot, resync.DesiredVersionWhenSent, resync.ReportedVersionWhenSent, connectionEpoch, outcome);
            }

            await CompleteAsync(outcome).ConfigureAwait(false);
            return true;
        }

        // Must be called while holding _desiredStateLock. Reconciles this device's copy of its twin with the twin that IoT hub
        // responded to a GET with, given this device's versions of the twin sections when that GET was sent, then ends the
        // resync (if any).
        private void ApplyTwinSnapshot(TwinSnapshot snapshot, ulong desiredVersionWhenSent, ulong reportedVersionWhenSent, long connectionEpoch, DesiredStateOutcome outcome)
        {
            RecordAuthoritativeVersions(connectionEpoch, snapshot.DesiredVersion, snapshot.ReportedVersion);

            TwinPushReceivedEventArgs twinPushArgs = new()
            {
                Reported = ReconcileReportedSnapshot(snapshot.ReportedVersion, snapshot.Reported, reportedVersionWhenSent),
            };

            if (snapshot.DesiredVersion != 0)
            {
                if (snapshot.DesiredVersion < _localDesiredVersion && _localDesiredVersion == desiredVersionWhenSent)
                {
                    // IoT hub's versions never decrease, so the version this device held is not valid.
                    Trace.TraceWarning("This device held version {0} of the desired properties, but IoT hub only has version {1}. Adopting version {1}.", _localDesiredVersion, snapshot.DesiredVersion);
                    lock (_twinStateLock)
                    {
                        _localDesiredVersion = 0;
                        _isTwinStateChanged = true;
                    }
                }

                twinPushArgs.Desired = TryApplyDesiredSnapshot(snapshot.DesiredVersion, snapshot.Desired);
            }

            if (twinPushArgs.Desired != null || twinPushArgs.Reported != null)
            {
                outcome.Deliveries.Add(twinPushArgs);
            }

            ResolveDesiredResync(connectionEpoch, outcome);
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

            if (outcome.ResyncGet is ResyncGet resyncGet)
            {
                await SendDesiredResyncGetAsync(resyncGet).ConfigureAwait(false);
            }
        }

        private async Task SendDesiredResyncGetAsync(ResyncGet resyncGet)
        {
            try
            {
                Trace.TraceInformation("Sending a get twin request to resync the twin");
                await PublishGetAsync(resyncGet.GetId, resyncGet.CancellationToken).ConfigureAwait(false);
            }
            catch (OperationCanceledException) when (resyncGet.CancellationToken.IsCancellationRequested)
            {
                // The resync already ended.
            }
            catch (Exception e)
            {
                // The defensive timeout sends this request again later.
                Trace.TraceWarning("Failed to publish the get twin request for resyncing the twin: {0}", e);
            }
        }

        // The payload looks like {"desired":{...,"$version":n},"reported":{...,"$version":m}}.
        internal static TwinSnapshot ParseGetTwinResponsePayload(byte[] payload)
        {
            JsonObject twin = payload.Length == 0 ? new JsonObject() : JsonNode.Parse(payload)!.AsObject();
            JsonObject desired = DetachSection(twin, "desired", out ulong desiredVersion);
            JsonObject reported = DetachSection(twin, "reported", out ulong reportedVersion);
            return new TwinSnapshot(desired, desiredVersion, reported, reportedVersion);
        }

        private static JsonObject DetachSection(JsonObject twin, string sectionName, out ulong version)
        {
            JsonObject section = twin.TryGetPropertyValue(sectionName, out JsonNode? node) && node is JsonObject sectionObject
                ? sectionObject
                : new JsonObject();
            twin.Remove(sectionName);

            // Remove the "$version" entry so that the twin object more closely mimics how it would in MQTTv5 scenario
            version = TakeVersion(section);
            return section;
        }

        // Removes the "$version" entry from the properties, and returns its value (or 0 if there isn't one).
        private static ulong TakeVersion(JsonObject properties)
        {
            ulong version = 0;
            if (properties.TryGetPropertyValue(VersionKey, out JsonNode? versionNode) && versionNode is JsonValue versionValue)
            {
                version = (ulong)versionValue;
            }

            properties.Remove(VersionKey);
            return version;
        }

        private static ulong ParseVersionFromTopic(string topic)
        {
            int queryStart = topic.IndexOf('?', StringComparison.Ordinal);
            if (queryStart < 0)
            {
                return 0;
            }

            NameValueCollection query = HttpUtility.ParseQueryString(topic.Substring(queryStart));
            return ulong.TryParse(query.Get(VersionKey), NumberStyles.None, CultureInfo.InvariantCulture, out ulong version) ? version : 0;
        }

        internal static bool TryParseResponseTopic(string topicName, out Guid requestId, out int status, out ulong version)
        {
            requestId = Guid.Empty;
            status = 0;
            version = 0;

            // The topic here looks like
            // "$iothub/twin/res/204/?$rid=efc34c73-79ce-4054-9985-0cdf40a3c794&$version=2"
            // The regex matching splits it up into the status ("204") and the query string ("?$rid=...&$version=2"), which is
            // then parsed for key value pairs. Get twin responses carry the twin versions in the payload instead.
            Match match = s_twinResponseTopicRegex.Match(topicName);
            if (!match.Success || !int.TryParse(match.Groups[1].Value, NumberStyles.None, CultureInfo.InvariantCulture, out status))
            {
                return false;
            }

            NameValueCollection queryStringKeyValuePairs = HttpUtility.ParseQueryString(match.Groups[2].Value);
            if (!Guid.TryParse(queryStringKeyValuePairs.Get(RequestIdTopicKey), out requestId))
            {
                return false;
            }

            _ = ulong.TryParse(queryStringKeyValuePairs.Get(VersionKey), NumberStyles.None, CultureInfo.InvariantCulture, out version);
            return true;
        }

        private sealed record DesiredPatch(ulong Version, JsonObject Properties);

        internal sealed record TwinSnapshot(JsonObject Desired, ulong DesiredVersion, JsonObject Reported, ulong ReportedVersion);

        private readonly record struct ResyncGet(Guid GetId, CancellationToken CancellationToken);

        // Gets the twin to resync the desired properties (and reconcile the reported properties) with IoT hub.
        private sealed class DesiredResync(long connectionEpoch)
        {
            public long ConnectionEpoch { get; } = connectionEpoch;

            public Guid GetId { get; set; }

            public int Attempt { get; set; } = 1;

            public List<DesiredPatch> Buffer { get; } = new();

            // This device's versions of the twin sections when the latest GET was sent. A section that IoT hub responds with an
            // older version of is only adopted if this device's version didn't change in the meantime.
            public ulong DesiredVersionWhenSent { get; set; }

            public ulong ReportedVersionWhenSent { get; set; }

            // Cancelled when the resync ends, so that a GET still waiting to be sent is never sent.
            public CancellationTokenSource Cancellation { get; } = new();

            public System.Threading.Timer? Timer { get; set; }
        }

        private sealed class AuthoritativeVersions(long connectionEpoch)
        {
            public long ConnectionEpoch { get; } = connectionEpoch;

            public ulong Desired { get; set; }

            public ulong Reported { get; set; }
        }

        private sealed class DesiredStateOutcome
        {
            public List<EventArgs> Deliveries { get; } = new();

            public ResyncGet? ResyncGet { get; set; }
        }

        private readonly record struct GetTwinRequest(bool GetDesired, bool GetReported);

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

            // Guarded by _reportedPatchLock. False until a TwinStateChanged snapshot containing this patch has been raised, if
            // it was made by a twin event handler. It isn't sent until then.
            public bool IsReleased { get; set; } = isReleased;

            // Guarded by _reportedPatchLock. Whether an attempt carrying this patch was ever started, so IoT hub may apply it.
            public bool HasBeenSent { get; set; }

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
            _connection.PublishReceivedAsync -= HandleReceivedMqttPublishAsync;
            _connection.ConnectionLost -= HandleConnectionLost;
            _connection.ConnectionReady -= HandleConnectionReady;
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

            AbandonGetTwinExchanges(() => new ObjectDisposedException(nameof(TwinClient)));
        }

        /// <summary>
        /// Releases the unmanaged resources used by this client and optionally disposes of the managed resources.
        /// </summary>
        /// <param name="disposing">true to release both managed and unmanaged resources (including the connection client); false to releases only unmanaged resources.</param>
        /// <remarks>
        /// Any reported properties patches or get twin requests still waiting for IoT hub's response fail with an
        /// <see cref="ObjectDisposedException"/>.
        /// </remarks>
        public void Dispose(bool disposing)
        {
            ReleaseResources();
            if (disposing)
            {
                _connection.Dispose();
            }
        }

        /// <summary>
        /// Releases the unmanaged resources and disposes of the managed resources (including the connection client) used by this client.
        /// </summary>
        /// <remarks>
        /// Any reported properties patches or get twin requests still waiting for IoT hub's response fail with an
        /// <see cref="ObjectDisposedException"/>.
        /// </remarks>
        public void Dispose()
        {
            Dispose(true);
        }
    }
}
