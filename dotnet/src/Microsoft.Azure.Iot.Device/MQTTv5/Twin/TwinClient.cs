// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Exceptions;
using Microsoft.Azure.Iot.Device.MQTTv5.Connection;
using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Models.Twin;
using Microsoft.Azure.Iot.Device.Twin;
using System.Text.Json.Nodes;

namespace Microsoft.Azure.Iot.Device.MQTTv5.Twin
{
    /// <summary>
    /// The feature client that keeps this device's copy of its twin in sync with an IoT hub that uses MQTTv5, as the device
    /// twin specification recommends. Safe to use from multiple threads.
    /// </summary>
    /// <remarks>
    /// <para>
    /// This client tracks the versions of the twin that this device holds and advertises them to IoT hub upon each
    /// connection, so that IoT hub only pushes the twin sections that changed. It delivers desired properties updates in
    /// version order (fetching the desired properties when it misses one), and queues reported properties patches, sending
    /// them one at a time, conditional on IoT hub's version of the reported properties, across reconnects, until IoT hub
    /// responds. It also keeps a copy of the twin that the application can persist (see <see cref="TwinStateChanged"/>).
    /// </para>
    /// <para>
    /// Unlike <see cref="Unified.Twin.TwinClient"/>, which also works with IoT hubs that use MQTTv3, this client makes its
    /// reported properties patches conditional (so that IoT hub applies each change at most once) and supports conditional
    /// get twin requests.
    /// </para>
    /// </remarks>
    public class TwinClient : IDisposable
    {
        private readonly IConnectionClient _connection;

        private readonly TwinEngine _engine;

        // The maximum number of desired patches buffered while resyncing the desired properties.
        internal const int DesiredResyncBufferCapacity = TwinEngine.DesiredResyncBufferCapacity;

        /// <summary>
        /// How long to wait for the response to the given attempt (starting at 1) of a desired properties resync GET
        /// request before sending it again.
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
        /// last observed. If a patch is missed, this client fetches the full desired properties from IoT hub and raises
        /// <see cref="TwinPushReceived"/> with them instead, followed by any newer patches.
        /// </remarks>
        public event Action<DesiredPatchReceivedEventArgs>? DesiredPatchReceived
        {
            add => _engine.DesiredPatchReceived += value;
            remove => _engine.DesiredPatchReceived -= value;
        }

        /// <summary>
        /// An event that executes whenever this client receives a Twin push message from IoT hub.
        /// </summary>
        /// <remarks>
        /// This feature is only supported by IoT hubs using MQTTv5. Older IoT hubs will never encounter this event.
        /// The desired section is only included if it is newer than the desired properties this client last observed. This
        /// event is also raised with just the desired section when this client resynchronizes its desired properties
        /// with IoT hub (for example, after missing a desired properties patch).
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
        /// that handler returns. Reported properties patches (including those made by such a handler) are only sent once a
        /// snapshot containing them has been raised and its handlers have returned. The snapshot belongs to the handler, which
        /// may keep it.
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
        /// IoT hub reveals its version of the desired properties when it acknowledges this device's presence upon each
        /// (re)connection, and in every desired properties patch, twin push and get twin response. This value is reset
        /// whenever the device reconnects.
        /// </remarks>
        public ulong? AuthoritativeDesiredPropertiesVersion => _engine.AuthoritativeDesiredPropertiesVersion;

        /// <summary>
        /// The latest version of the reported properties that IoT hub has revealed on the current connection, or null if it
        /// hasn't revealed one yet.
        /// </summary>
        /// <remarks>
        /// IoT hub reveals its version of the reported properties when it acknowledges this device's presence upon each
        /// (re)connection, and in every twin push, get twin response and reported properties update response. This client
        /// makes each reported properties patch conditional on this version. This value is reset whenever the device reconnects.
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
        /// The versions in <paramref name="initialState"/> replace any initial versions in the connection client's options, so
        /// that IoT hub only pushes the twin sections that changed. Its pending reported properties patches are sent, in order,
        /// before any made afterwards. Their outcomes are raised by <see cref="ReportedPatchCompleted"/>.
        /// </remarks>
        public TwinClient(IConnectionClient connection, TwinState? initialState)
        {
            ArgumentNullException.ThrowIfNull(connection);
            _connection = connection;
            _engine = new TwinEngine(
                new Mqttv5TwinTransport(connection, handlesConnectionEvents: true),
                this,
                new TwinEngineOptions { ConditionalReportedPatches = true },
                initialState);
        }

        /// <summary>
        /// Get a snapshot of this device's copy of its twin, as last raised by <see cref="TwinStateChanged"/> (or changed since).
        /// </summary>
        public TwinState GetTwinState() => _engine.GetTwinState();

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
        public Task<DeviceTwin> GetTwinAsync(bool getReported = true, bool getDesired = true, ulong ifNotMatchReported = 0, ulong ifNotMatchDesired = 0, CancellationToken cancellationToken = default)
        {
            return _engine.GetTwinAsync(getReported, getDesired, ifNotMatchReported, ifNotMatchDesired, cancellationToken);
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
        /// reported properties, or the reason why IoT hub rejected the changes.
        /// </returns>
        /// <remarks>
        /// <para>
        /// This client sends one reported properties patch at a time, and makes it conditional on IoT hub's current version
        /// of the reported properties so that IoT hub applies each change at most once. Patches made while another patch is
        /// in flight (or while the device is reconnecting) are combined, in order, into the next patch that this client sends,
        /// so they share its response. Patches that can't be combined into one JSON merge patch (such as one that removes a
        /// property followed by one that sets some of that property's children) are sent one after another.
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
        /// Patches are only discarded if <paramref name="cancellationToken"/> is cancelled before they are sent, or if this
        /// client is disposed.
        /// </para>
        /// </remarks>
        /// <exception cref="DeviceException">Thrown if the patch could not be sent.</exception>
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
