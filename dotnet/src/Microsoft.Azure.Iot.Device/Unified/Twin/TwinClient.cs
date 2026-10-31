// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Exceptions;
using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Models.Twin;
using Microsoft.Azure.Iot.Device.Unified.Connection;
using Microsoft.Azure.Iot.Device.Unified.Models;
using System.Text.Json.Nodes;

namespace Microsoft.Azure.Iot.Device.Unified.Twin
{
    /// <summary>
    /// The feature client for interacting with a device's twin properties, whether the device is connected to an MQTTv5 IoT
    /// hub or an MQTTv3 one.
    /// </summary>
    /// <remarks>
    /// <para>
    /// This client keeps a copy of the twin, coalesces reported properties patches, holds them while the device is
    /// disconnected, and retries them after reconnecting, the same way as <see cref="MQTTv5.Twin.TwinClient"/> does. The
    /// application doesn't need to coordinate its twin operations with the connection's state.
    /// </para>
    /// <para>
    /// MQTTv3 IoT hubs lack some of the features that this client relies on for MQTTv5 IoT hubs, so it behaves differently
    /// when connected to one:
    /// </para>
    /// <list type="bullet">
    /// <item><description>
    /// Reported properties patches are applied at least once, rather than at most once, since MQTTv3 IoT hubs can't make a
    /// patch conditional on their version of the reported properties. A patch whose response is lost is applied again when it
    /// is resent.
    /// </description></item>
    /// <item><description>
    /// MQTTv3 IoT hubs don't push the twin when the device connects, so this client gets the full twin upon each connection
    /// instead, and raises <see cref="TwinPushReceived"/> with the sections that changed. Reported properties patches are
    /// only sent once that request completes.
    /// </description></item>
    /// <item><description>
    /// MQTTv3 IoT hubs respond with HTTP-like status codes, which this client maps onto <see cref="Result"/>: 2xx onto
    /// <see cref="Result.Ok"/>, 400 onto <see cref="Result.PayloadInvalid"/>, 413 onto <see cref="Result.PayloadTooLarge"/>
    /// and anything else (including throttling) onto <see cref="Result.InternalError"/>.
    /// </description></item>
    /// </list>
    /// </remarks>
    public class TwinClient : IDisposable
    {
        // The device presence flow subscribes to these topics on MQTTv3 IoT hubs.
        internal const string ClassicTwinResponseTopic = "$iothub/twin/res/";
        internal const string ClassicTwinDesiredPropertiesPatchTopic = "$iothub/twin/PATCH/properties/desired/";

        private readonly TwinEngine _twinClient;

        /// <summary>
        /// Construct a new <see cref="TwinClient"/> instance.
        /// </summary>
        /// <param name="connection">The connection client this feature client will use.</param>
        /// <remarks>
        /// The provided connection client does not need to be connected before this constructor is called. However, the provided
        /// connection client must be connected prior to getting the twin. Reported properties patches are held until the device
        /// connects.
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
        /// Its pending reported properties patches are sent, in order, before any made afterwards. Their outcomes are raised by
        /// <see cref="ReportedPatchCompleted"/>. MQTTv5 IoT hubs only push the twin sections that changed since this snapshot.
        /// MQTTv3 IoT hubs always return the full twin, but <see cref="TwinPushReceived"/> is still only raised with the
        /// sections that changed.
        /// </remarks>
        /// <exception cref="InvalidOperationException">Thrown if <paramref name="connection"/> opted out of twins (see <see cref="ConnectionClientOptions.EnableTwin"/>).</exception>
        public TwinClient(IConnectionClient connection, TwinState? initialState)
        {
            if (!connection.IsTwinEnabled)
            {
                throw new InvalidOperationException($"The provided connection client opted out of twins. Set {nameof(ConnectionClientOptions)}.{nameof(ConnectionClientOptions.EnableTwin)} to true to use a {nameof(TwinClient)}.");
            }

            _twinClient = new TwinEngine(new Stub(connection), initialState);
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
            add => _twinClient.DesiredPatchReceived += value;
            remove => _twinClient.DesiredPatchReceived -= value;
        }

        /// <summary>
        /// An event that executes whenever this client receives a snapshot of twin sections that changed since this device
        /// last observed them, without the application asking for it.
        /// </summary>
        /// <remarks>
        /// Raised whenever the device connects and its copy of the desired or reported properties is out of date. MQTTv5 IoT
        /// hubs push these sections. For MQTTv3 IoT hubs, this client gets the twin upon each connection instead. Also raised with
        /// just the desired section when this client resynchronizes its desired properties with IoT hub (for example, after
        /// missing a desired properties patch). The desired properties in the snapshot replace the ones this device held.
        /// </remarks>
        public event Action<TwinPushReceivedEventArgs>? TwinPushReceived
        {
            add => _twinClient.TwinPushReceived += value;
            remove => _twinClient.TwinPushReceived -= value;
        }

        /// <inheritdoc cref="MQTTv5.Twin.TwinClient.TwinStateChanged"/>
        public event Action<TwinState>? TwinStateChanged
        {
            add => _twinClient.TwinStateChanged += value;
            remove => _twinClient.TwinStateChanged -= value;
        }

        /// <inheritdoc cref="MQTTv5.Twin.TwinClient.ReportedPatchCompleted"/>
        public event Action<ReportedPatchCompletedEventArgs>? ReportedPatchCompleted
        {
            add => _twinClient.ReportedPatchCompleted += value;
            remove => _twinClient.ReportedPatchCompleted -= value;
        }

        /// <inheritdoc cref="MQTTv5.Twin.TwinClient.GetTwinState"/>
        public TwinState GetTwinState()
        {
            return _twinClient.GetTwinState();
        }

        /// <summary>
        /// Get the full twin.
        /// </summary>
        /// <param name="cancellationToken">The cancellation token.</param>
        /// <returns>The returned twin.</returns>
        /// <remarks>
        /// Concurrent calls share a single request to IoT hub and all receive its response. If IoT hub doesn't respond within a
        /// defensive timeout (5 to 15 minutes, growing with each attempt), the request is sent again.
        /// </remarks>
        /// <exception cref="DeviceException">
        /// Thrown if this get twin request could not be sent or IoT hub rejected it, or with
        /// <see cref="ErrorRetryability.Retryable"/> if the connection to IoT hub was lost before the response arrived. The
        /// request may be retried once reconnected.
        /// </exception>
        public Task<DeviceTwin> GetTwinAsync(CancellationToken cancellationToken = default)
        {
            return _twinClient.GetTwinAsync(getReported: true, getDesired: true, ifNotMatchReported: 0, ifNotMatchDesired: 0, cancellationToken);
        }

        /// <summary>
        /// Update this device's reported properties.
        /// </summary>
        /// <param name="reportedProperties">The patch of the reported properties to send to IoT hub.</param>
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
        /// This client sends one reported properties patch at a time. Patches made while another patch is in flight (or while the
        /// device is disconnected) are combined, in order, into the next patch that this client sends (up to 32 KiB), so they
        /// share its response. If IoT hub doesn't respond within a defensive timeout (5 to 15 minutes, growing with each attempt), or the
        /// connection is lost, the patch is sent again once the device has (re)connected.
        /// </para>
        /// <para>
        /// On MQTTv5 IoT hubs, each patch is conditional on IoT hub's version of the reported properties, so IoT hub applies it at
        /// most once. MQTTv3 IoT hubs don't support that, so a patch whose response was lost is applied again when it is resent.
        /// </para>
        /// </remarks>
        /// <exception cref="DeviceException">Thrown if the patch could not be sent.</exception>
        /// <exception cref="ObjectDisposedException">Thrown if this client is disposed before IoT hub responds.</exception>
        public Task<ReportedPatchResponse> UpdateReportedPropertiesAsync(JsonObject reportedProperties, CancellationToken cancellationToken = default)
        {
            return _twinClient.UpdateReportedPropertiesCoreAsync(reportedProperties, cancellationToken);
        }

        /// <inheritdoc cref="MQTTv5.Twin.TwinClient.EnqueueReportedPropertiesUpdate"/>
        public void EnqueueReportedPropertiesUpdate(JsonObject reportedProperties)
        {
            _twinClient.EnqueueReportedPropertiesUpdate(reportedProperties);
        }

        /// <summary>
        /// Releases the unmanaged resources used by this client and optionally disposes of the managed resources.
        /// </summary>
        /// <param name="disposing">true to release both managed and unmanaged resources; false to releases only unmanaged resources.</param>
        public void Dispose(bool disposing)
        {
            _twinClient.Dispose(disposing);
        }

        /// <summary>
        /// Releases the unmanaged resources and disposes of the managed resources used by this client 
        /// </summary>
        public void Dispose()
        {
            _twinClient.Dispose();
        }
    }
}
