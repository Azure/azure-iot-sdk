// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Exceptions;
using Microsoft.Azure.Iot.Device.Mqtt;
using Microsoft.Azure.Iot.Device.MQTTv5.Connection;

namespace Microsoft.Azure.Iot.Device.MQTTv5.CustomTopics
{
    /// <summary>
    /// A feature client for publishing to custom (application-defined) MQTT topics.
    /// </summary>
    /// <remarks>
    /// <para>
    /// Any topic passed to this client may contain the <see cref="IConnectionClient.DeviceIdPlaceholder"/>
    /// ("{deviceId}") placeholder, for example "foo/{deviceId}/bar". The connection client replaces the placeholder with
    /// this device's current ID each time it actually sends the operation, so an operation that is pending while the device
    /// is re-provisioned and assigned a different device ID is sent with the newly assigned ID.
    /// </para>
    /// <para>
    /// To keep using a specific device ID even if the device is later assigned a different one, put that ID in the topic instead of the placeholder.
    /// </para>
    /// <para>
    /// MQTT-level results are returned as-is. A PUBACK whose reason code reports a failure is returned to the caller
    /// rather than thrown as an exception, and the operation is not retried. Connection-level failures are still handled
    /// by the connection client: an operation that cannot be sent because the device is not connected waits for the
    /// connection to be re-established and is then sent. A QoS 1 PUBLISH interrupted before its acknowledgement arrives
    /// is re-sent after reconnection. See the remarks on <see cref="PublishAsync"/> for how it behaves in exceptional cases.
    /// </para>
    /// </remarks>
    public class CustomTopicsClient : IDisposable
    {
        private volatile bool _isDisposed = false;
        private readonly object _disposeLock = new();
        private readonly IConnectionClient _connection;

        /// <summary>
        /// Construct a new <see cref="CustomTopicsClient"/> instance.
        /// </summary>
        /// <param name="connection">The connection client this feature client will use.</param>
        /// <remarks>
        /// The provided connection client does not need to be connected before this constructor is called. However, the provided connection client must be connected prior
        /// to using this feature client.
        /// </remarks>
        public CustomTopicsClient(IConnectionClient connection)
        {
            _connection = connection;
        }

        /// <summary>
        /// Publish to a custom topic.
        /// </summary>
        /// <param name="publish">
        /// The publish to send. Any <see cref="IConnectionClient.DeviceIdPlaceholder"/> in its topic is replaced with this
        /// device's current ID when it is sent.
        /// </param>
        /// <param name="cancellationToken">
        /// The cancellation token. This also bounds how long this method waits for a lost connection to be re-established.
        /// </param>
        /// <returns>
        /// The PUBACK, whatever its reason code. For a QoS 0 publish, which IoT hub does not acknowledge, this is a
        /// successful PUBACK created once the PUBLISH has been sent.
        /// </returns>
        /// <remarks>
        /// <para>How this method behaves in exceptional cases:</para>
        /// <list type="bullet">
        /// <item><description>
        /// <b>A temporary disconnection.</b> If this device is not connected to IoT hub when the PUBLISH is about to be
        /// sent (for example, because the connection dropped and is being re-established, or because the device is being
        /// re-provisioned), this method waits until the connection client has re-established this device's presence on IoT
        /// hub, then sends the PUBLISH. Any <see cref="IConnectionClient.DeviceIdPlaceholder"/> is replaced with the
        /// device ID at that time. If the connection drops after a QoS 1 PUBLISH was sent but before its PUBACK arrived,
        /// the PUBLISH is re-sent after reconnection, including after re-provisioning. IoT hub may receive it more than
        /// once. A QoS 0 PUBLISH is not re-sent if the connection drops while it is being sent.
        /// </description></item>
        /// <item><description>
        /// <b>A fatal disconnection.</b> If the connection client hits a fault that it will not recover from (which it
        /// reports through <see cref="AbstractConnectionClient.ConnectionFaultedAsync"/>) while this method waits for the
        /// connection, this method stops waiting. It throws an <see cref="OperationCanceledException"/> whose
        /// <see cref="Exception.InnerException"/> is the <see cref="DeviceException"/> that describes the fault. Calls made
        /// after such a fault fail without waiting, until the application connects the connection client again.
        /// </description></item>
        /// <item><description>
        /// <b>A PUBACK with a failure reason code.</b> The PUBACK is returned as-is. This method does not throw, and the
        /// PUBLISH is not retried. Check <see cref="MqttPublishAck.ReasonCode"/> to find out whether IoT hub accepted it.
        /// </description></item>
        /// <item><description>
        /// <b>A deliberate disconnection by the application.</b> If <see cref="AbstractConnectionClient.DisconnectAsync"/>
        /// has been called before this method is called, this method throws a <see cref="NotSupportedException"/> without
        /// sending anything. If it interrupts a QoS 1 PUBLISH awaiting its PUBACK, or is called while this method waits
        /// for a lost connection to be re-established, this method throws an <see cref="OperationCanceledException"/>.
        /// </description></item>
        /// </list>
        /// </remarks>
        /// <exception cref="NotSupportedException">
        /// Thrown if the connection client is disconnected and is not trying to reconnect, for example because
        /// <see cref="AbstractConnectionClient.DisconnectAsync"/> was called.
        /// </exception>
        /// <exception cref="MqttClientNotConnectedException">
        /// Thrown if <see cref="AbstractConnectionClient.DisconnectAsync"/> is called just as this operation finds the
        /// connection gone, before it has started waiting for the connection to be re-established.
        /// </exception>
        /// <exception cref="OperationCanceledException">
        /// Thrown if <paramref name="cancellationToken"/> is canceled, the application deliberately disconnects, or the
        /// connection client hits a fault that it will not recover from while this operation waits for the connection. In the latter case, the
        /// <see cref="Exception.InnerException"/> is the <see cref="DeviceException"/> describing that fault.
        /// </exception>
        /// <exception cref="DeviceException">
        /// Thrown if this operation fails for a reason other than an MQTT reason code or a recoverable connection loss.
        /// </exception>
        /// <exception cref="ObjectDisposedException">Thrown if this client has been disposed.</exception>
        public async Task<MqttPublishAck> PublishAsync(MqttPublish publish, CancellationToken cancellationToken = default)
        {
            ObjectDisposedException.ThrowIf(_isDisposed, this);
            cancellationToken.ThrowIfCancellationRequested();
            EnsureConnected();

            // Unlike other feature clients, this client hands MQTT-level results to the user as-is
            return await _connection.PublishAsync(publish, throwOnErrorReasonCode: false, cancellationToken);
        }

        // The connection context survives a disconnect, so IsConnectionTargetSet is what says whether the connection client
        // is still connected or reconnecting. Without it, operations after an explicit disconnect would wait indefinitely.
        private void EnsureConnected()
        {
            if (_connection.GetCurrentConnectionContext() == null || !_connection.IsConnectionTargetSet)
            {
                throw new NotSupportedException("Must be connected before calling this method.");
            }
        }

        /// <summary>
        /// Releases the unmanaged resources used by this client and optionally disposes of the managed resources.
        /// </summary>
        /// <param name="disposing">true to release both managed and unmanaged resources; false to releases only unmanaged resources.</param>
        public void Dispose(bool disposing)
        {
            lock (_disposeLock)
            {
                if (_isDisposed)
                {
                    return;
                }

                _isDisposed = true;
            }

            if (disposing)
            {
                _connection.Dispose();
            }
        }

        /// <summary>
        /// Releases the unmanaged resources and disposes of the managed resources used by this client.
        /// </summary>
        public void Dispose()
        {
            Dispose(true);
            GC.SuppressFinalize(this);
        }
    }
}
