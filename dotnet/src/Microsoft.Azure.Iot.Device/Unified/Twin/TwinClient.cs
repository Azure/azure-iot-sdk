// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Exceptions;
using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Models.Twin;
using Microsoft.Azure.Iot.Device.Twin;
using Microsoft.Azure.Iot.Device.Unified.Connection;
using Microsoft.Azure.Iot.Device.Unified.Models;
using System.Text.Json.Nodes;

namespace Microsoft.Azure.Iot.Device.Unified.Twin
{
    /// <summary>
    /// The feature client that keeps this device's copy of its twin in sync with IoT hub, whether that IoT hub uses MQTTv3 or
    /// MQTTv5, as the device twin specification recommends. Safe to use from multiple threads.
    /// </summary>
    /// <remarks>
    /// <para>
    /// This client tracks the versions of the twin that this device holds. It delivers desired properties updates in version
    /// order (fetching the twin when it misses one), and queues reported properties patches, sending them one at a time,
    /// across reconnects, until IoT hub responds. It also keeps a copy of the twin that the application can persist (see
    /// <see cref="TwinStateChanged"/>).
    /// </para>
    /// <para>
    /// IoT hubs that use MQTTv3 don't push the twin to the device when it connects, so on those this client gets the twin
    /// each time the device (re)connects, and raises <see cref="TwinPushReceived"/> with the sections of it that changed.
    /// IoT hubs that use MQTTv5 learn the versions of the twin that this device holds when it connects, and push the sections
    /// that changed themselves.
    /// </para>
    /// <para>
    /// This client behaves the same way on both kinds of IoT hub, including when re-provisioning moves the device from one
    /// kind to the other: the device's copy of its twin and its pending reported properties patches carry over. Its reported
    /// properties patches are always unconditional. See <see cref="MQTTv5.Twin.TwinClient"/> for a client of IoT hubs that use
    /// MQTTv5 that makes them conditional.
    /// </para>
    /// </remarks>
    public class TwinClient : IDisposable
    {
        private readonly IConnectionClient _connection;

        private readonly TwinEngine _engine;

        // Response topics to subscribe to
        internal const string ClassicTwinResponseTopic = ClassicTwinTransport.ResponseTopic;
        internal const string ClassicTwinDesiredPropertiesPatchTopic = ClassicTwinTransport.DesiredPropertiesPatchTopic;

        internal const string VersionKey = ClassicTwinTransport.VersionKey;

        // The maximum number of desired patches buffered while resyncing the desired properties.
        internal const int DesiredResyncBufferCapacity = TwinEngine.DesiredResyncBufferCapacity;

        /// <summary>
        /// How long to wait for the response to the given attempt (starting at 1) of a get twin request that this client
        /// sends to resync the twin before sending it again.
        /// </summary>
        internal Func<int, TimeSpan> DesiredResyncTimeoutSchedule
        {
            get => _engine.DesiredResyncTimeoutSchedule;
            set => _engine.DesiredResyncTimeoutSchedule = value;
        }

        /// <summary>
        /// How long to wait for the response to the given attempt (starting at 1) of an application's get twin request
        /// before sending it again.
        /// </summary>
        internal Func<int, TimeSpan> GetTwinTimeoutSchedule
        {
            get => _engine.GetTwinTimeoutSchedule;
            set => _engine.GetTwinTimeoutSchedule = value;
        }

        /// <summary>
        /// How long to wait for the response to the given consecutive attempt (starting at 1) of a reported properties patch
        /// before sending it again.
        /// </summary>
        internal Func<int, TimeSpan> ReportedPatchTimeoutSchedule
        {
            get => _engine.ReportedPatchTimeoutSchedule;
            set => _engine.ReportedPatchTimeoutSchedule = value;
        }

        /// <summary>
        /// An event that executes whenever this client receives a desired properties update from IoT hub.
        /// </summary>
        /// <remarks>
        /// Patches are only raised in version order, each exactly one version newer than the desired properties this client
        /// last observed. If a patch is missed, this client gets the twin from IoT hub and raises
        /// <see cref="TwinPushReceived"/> with the desired properties instead, followed by any newer patches.
        /// </remarks>
        public event Action<DesiredPatchReceivedEventArgs>? DesiredPatchReceived
        {
            add => _engine.DesiredPatchReceived += value;
            remove => _engine.DesiredPatchReceived -= value;
        }

        /// <summary>
        /// An event that executes whenever this client gets a copy of the twin from IoT hub that changes this device's copy.
        /// </summary>
        /// <remarks>
        /// IoT hubs that use MQTTv5 push the twin sections that changed when the device (re)connects. IoT hubs that use MQTTv3
        /// don't push the twin, so on those this client gets the twin each time the device (re)connects, and whenever it misses
        /// a desired properties patch or learns that its copy of the reported properties is out of date. This event is also
        /// raised with the desired section whenever this client resyncs its desired properties with IoT hub. The desired section is only included if it is newer than the desired properties this client last observed. The
        /// reported section is only included if it differs from the version of the reported properties this client holds.
        /// </remarks>
        public event Action<TwinPushReceivedEventArgs>? TwinPushReceived
        {
            add => _engine.TwinPushReceived += value;
            remove => _engine.TwinPushReceived -= value;
        }

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
        public event Action<TwinState>? TwinStateChanged
        {
            add => _engine.TwinStateChanged += value;
            remove => _engine.TwinStateChanged -= value;
        }

        /// <summary>
        /// An event that executes whenever IoT hub responds to a reported properties patch that this client sent, or the patch
        /// couldn't be sent. Raised in the order the patches were sent.
        /// </summary>
        /// <remarks>
        /// Unlike awaiting <see cref="UpdateReportedPropertiesAsync"/>, this is raised for every patch, including those made by
        /// <see cref="EnqueueReportedPropertiesUpdate"/> or restored from a <see cref="TwinState"/>.
        /// </remarks>
        public event Action<ReportedPatchCompletedEventArgs>? ReportedPatchCompleted
        {
            add => _engine.ReportedPatchCompleted += value;
            remove => _engine.ReportedPatchCompleted -= value;
        }

        /// <summary>
        /// The latest version of the desired properties that IoT hub has revealed on the current connection, or null if it
        /// hasn't revealed one yet.
        /// </summary>
        /// <remarks>
        /// IoT hub reveals its version of the desired properties in every desired properties patch, twin push and get twin
        /// response (including the one this client gets after each connection to an IoT hub that uses MQTTv3), and IoT hubs
        /// that use MQTTv5 also reveal it when they acknowledge this device's presence upon each connection. This value is
        /// reset whenever the connection is lost.
        /// </remarks>
        public ulong? AuthoritativeDesiredPropertiesVersion => _engine.AuthoritativeDesiredPropertiesVersion;

        /// <summary>
        /// The latest version of the reported properties that IoT hub has revealed on the current connection, or null if it
        /// hasn't revealed one yet.
        /// </summary>
        /// <remarks>
        /// IoT hub reveals its version of the reported properties in every twin push and get twin response (including the one
        /// this client gets after each connection to an IoT hub that uses MQTTv3) and every successful reported properties
        /// update response, and IoT hubs that use MQTTv5 also reveal it when they acknowledge this device's presence upon each
        /// connection. This value is reset whenever the connection is lost.
        /// </remarks>
        public ulong? AuthoritativeReportedPropertiesVersion => _engine.AuthoritativeReportedPropertiesVersion;

        /// <summary>
        /// Construct a new <see cref="TwinClient"/> instance.
        /// </summary>
        /// <param name="connection">The connection client this feature client will use.</param>
        /// <remarks>
        /// The provided connection client does not need to be connected before this constructor is called. However, the provided connection client must be connected prior
        /// to using this feature client to send/receive any twin messages, except for reported properties patches, which are held until the device connects.
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
        /// The twin that this client gets (or IoT hub pushes) once the device connects is compared against
        /// <paramref name="initialState"/>, so that <see cref="TwinPushReceived"/> only includes the twin sections that changed. Its pending reported properties
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
            _engine = new TwinEngine(
                new UnifiedTwinTransport(connection),
                this,
                new TwinEngineOptions { KeepAttemptOnRetryablePublishFailure = true },
                initialState);
        }

        /// <summary>
        /// Get a snapshot of this device's copy of its twin, as last raised by <see cref="TwinStateChanged"/> (or changed since).
        /// </summary>
        public TwinState GetTwinState() => _engine.GetTwinState();

        /// <summary>
        /// Get the full twin, or one of its sections.
        /// </summary>
        /// <param name="getReported">Include the reported properties in the returned twin.</param>
        /// <param name="getDesired">Include the desired properties in the returned twin.</param>
        /// <param name="cancellationToken">The cancellation token.</param>
        /// <returns>The returned twin. Both versions are always set.</returns>
        /// <remarks>
        /// <para>
        /// IoT hubs that use MQTTv3 always return the full twin, so the sections that weren't requested are removed by this
        /// client.
        /// </para>
        /// <para>
        /// Concurrent calls with identical arguments share a single request to IoT hub and all receive its response. If IoT hub
        /// doesn't respond within a defensive timeout (5 to 15 minutes, growing with each attempt), or responds that it is
        /// throttling or failing (status 429 or 5xx), the request is sent again once that timeout elapses.
        /// </para>
        /// </remarks>
        /// <exception cref="ArgumentException">Thrown when neither desired nor reported properties are requested.</exception>
        /// <exception cref="NotSupportedException">Thrown if the device isn't connected.</exception>
        /// <exception cref="DeviceException">
        /// Thrown if IoT hub rejected this get twin request, or with <see cref="ErrorRetryability.Retryable"/> if the
        /// connection to IoT hub was lost before the response arrived. The request may be retried once reconnected.
        /// </exception>
        public Task<DeviceTwin> GetTwinAsync(bool getReported = true, bool getDesired = true, CancellationToken cancellationToken = default)
        {
            return _engine.GetTwinAsync(getReported, getDesired, ifNotMatchReported: 0, ifNotMatchDesired: 0, cancellationToken);
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
        /// reported properties, or the reason why IoT hub rejected the changes. IoT hubs that use MQTTv3 respond with a status,
        /// which is mapped to <see cref="Result.PayloadInvalid"/> for status 400, <see cref="Result.PayloadTooLarge"/> for
        /// status 413 and <see cref="Result.InternalError"/> for any other status that isn't retried.
        /// </returns>
        /// <remarks>
        /// <para>
        /// This client sends one reported properties patch at a time, and each patch is applied unconditionally (IoT hubs that
        /// use MQTTv3 don't support conditional patches, and this client behaves the same way on IoT hubs that use MQTTv5). Patches made while another patch is in flight (or while the device
        /// is reconnecting) are combined, in order, into the next patch that this client sends, so they share its response.
        /// Patches that can't be combined into one JSON merge patch (such as one that removes a property followed by one that
        /// sets some of that property's children) are sent one after another.
        /// </para>
        /// <para>
        /// If IoT hub doesn't respond within a defensive timeout (5 to 15 minutes, growing with each attempt), or responds that
        /// it is throttling or failing (such as status 429 or 5xx), the patch is sent again once that timeout elapses. If the connection
        /// is lost, the patch is sent again once the device has reconnected (and, on IoT hubs that use MQTTv5, IoT hub has
        /// acknowledged its presence). Since these patches are unconditional, IoT hub may
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
            return _engine.UpdateReportedPropertiesAsync(patch.ReportedProperties, cancellationToken);
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
        public void EnqueueReportedPropertiesUpdate(JsonObject reportedProperties) => _engine.EnqueueReportedPropertiesUpdate(reportedProperties);

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
            _engine.Dispose();
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
