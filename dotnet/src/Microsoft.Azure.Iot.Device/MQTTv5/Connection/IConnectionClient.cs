// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Mqtt;

namespace Microsoft.Azure.Iot.Device.MQTTv5.Connection
{
    public interface IConnectionClient : IDisposable //TODO what is the disposal pattern like with feature clients + this client? Mimic HTTP pattern of "disposing" flag?
    {
        /// <summary>
        /// The stand-in for this device's ID in any topic passed to <see cref="PublishAsync"/>,
        /// <see cref="SubscribeAsync"/> or <see cref="UnsubscribeAsync"/>.
        /// </summary>
        /// <remarks>
        /// A device's ID can change while an operation is pending (for example, when the device is re-provisioned and
        /// assigned a different device ID), so feature clients must not bake the device ID into a topic themselves.
        /// Instead, topics use this placeholder (for example, "ih/{deviceId}/srv/telemetry") and the connection
        /// client substitutes the device's current ID each time it actually sends the operation.
        /// </remarks>
        public const string DeviceIdPlaceholder = Unified.Connection.IConnectionClient.DeviceIdPlaceholder;

        /// <summary>
        /// Get the current connection context.
        /// </summary>
        /// <returns>Null if this connection client has not been connected yet. Otherwise, it returns the current connection context.</returns>
        ConnectionContext? GetCurrentConnectionContext();

        /// <summary>
        /// Whether this client has selected a connection endpoint. This remains true while it is automatically
        /// reconnecting and becomes false after an explicit disconnect.
        /// </summary>
        bool IsConnectionTargetSet => true;

        /// <summary>
        /// The connection nonce sent in the birth message of the current (or most recent) connection attempt to an
        /// MQTTv5 IoT hub, or null if it is not known.
        /// </summary>
        /// <remarks>
        /// IoT hub tags connection-scoped messages (such as twin pushes and desired property patches) with this nonce.
        /// Connections to MQTTv3 IoT hubs have no birth message, so for those this is an ID that the connection client
        /// generates for each connection, which IoT hub never sees.
        /// </remarks>
        internal Guid? ConnectionNonce => null;

        /// <summary>
        /// Raised whenever the device presence flow completes on a connection to an MQTTv3 IoT hub, which means that the
        /// twin topics are subscribed to, so that IoT hub delivers the responses to twin requests.
        /// </summary>
        /// <remarks>
        /// MQTTv3 IoT hubs neither acknowledge a device's presence nor push its twin upon connecting, so the twin client
        /// uses this to fetch the twin instead. Never raised for MQTTv5 IoT hubs, whose birth ack serves this purpose.
        /// </remarks>
        internal event Action? ClassicDevicePresenceEstablished { add { } remove { } }

        /// <summary>
        /// Whether the device presence flow has completed on the current connection to an MQTTv3 IoT hub. See
        /// <see cref="ClassicDevicePresenceEstablished"/>.
        /// </summary>
        internal bool IsClassicDevicePresenceEstablished => false;

        /// <summary>
        /// The twin versions from the latest birth ack that IoT hub sent this connection client, or null if there hasn't
        /// been one (or this connection client doesn't track them).
        /// </summary>
        /// <remarks>
        /// Feature clients constructed after the device connected use this to learn the versions they missed.
        /// </remarks>
        internal BirthAckVersions? LatestBirthAck => null;

        /// <summary>
        /// Raised whenever the connection to IoT hub is lost or closed, before any reconnection starts.
        /// </summary>
        /// <remarks>
        /// Twin exchanges are bound to the connection that they were sent on, so feature clients use this to abandon them.
        /// </remarks>
        internal event Action? ConnectionLost { add { } remove { } }

        /// <summary>
        /// Raised when the device connects to IoT hub as a different identity (IoT hub or device ID) than on its previous
        /// connection, for example after re-provisioning, before the device announces its presence with that identity.
        /// </summary>
        /// <remarks>
        /// Twin versions are only meaningful for the identity they came from, so by the time this is raised the connection
        /// client has forgotten the twin versions it recorded, and feature clients discard their copies of the twin.
        /// </remarks>
        internal event Action? HubIdentityChanged { add { } remove { } }

        /// <summary>
        /// Record that the device now holds this version of the desired properties, so that it is announced to IoT hub upon
        /// (re)connecting. Older versions than the one already recorded are ignored.
        /// </summary>
        internal void ObserveDesiredPropertiesVersion(ulong version) { }

        /// <summary>
        /// Record that the device now holds this version of the reported properties, so that it is announced to IoT hub upon
        /// (re)connecting. Older versions than the one already recorded are ignored.
        /// </summary>
        internal void ObserveReportedPropertiesVersion(ulong version) { }

        /// <summary>
        /// Replace the recorded version of the desired properties that the device holds, even with an older one. Used when
        /// IoT hub reveals that the recorded version is not valid (for example, it is newer than IoT hub's version).
        /// </summary>
        internal void ReplaceDesiredPropertiesVersion(ulong version) { }

        /// <summary>
        /// Replace the recorded version of the reported properties that the device holds, even with an older one. Used when
        /// IoT hub reveals that the recorded version is not valid (for example, it is newer than IoT hub's version).
        /// </summary>
        internal void ReplaceReportedPropertiesVersion(ulong version) { }

        /// <summary>
        /// Send a PUBLISH to IoT hub. Any <see cref="DeviceIdPlaceholder"/> in the topic is replaced with the device's
        /// current ID when it is sent.
        /// </summary>
        /// <remarks>
        /// If the connection is lost before a QoS 1 PUBLISH is acknowledged, it is sent again once the device is
        /// reconnected to IoT hub, including after re-provisioning with the newly assigned hub and device ID.
        /// IoT hub may receive it more than once. A QoS 0 PUBLISH is not re-sent if the connection drops while it is sent.
        /// </remarks>
        /// <param name="publish">The publish to send.</param>
        /// <param name="cancellationToken">The cancellation token.</param>
        /// <returns>
        /// For QoS 1, the PUBACK received from IoT hub. Error reason codes are returned without throwing and must
        /// be checked by the caller. For QoS 0, no PUBACK is received, and the returned result does not confirm
        /// that IoT hub accepted the request.
        /// </returns>
        Task<MqttPublishAck> PublishAsync(MqttPublish publish, CancellationToken cancellationToken = default);

        /// <summary>
        /// Send a SUBSCRIBE to IoT hub. Any <see cref="DeviceIdPlaceholder"/> in a topic filter is replaced with the
        /// device's current ID when it is sent.
        /// </summary>
        /// <remarks>
        /// If the connection is lost before the SUBACK arrives, the SUBSCRIBE is sent again once the device is
        /// reconnected to IoT hub, including after re-provisioning.
        /// </remarks>
        /// <param name="subscribe">The subscribe to send.</param>
        /// <param name="cancellationToken">The cancellation token.</param>
        /// <returns>
        /// The SUBACK as received from IoT hub. This method does not throw if the SUBACK reports an error, so check its
        /// reason code(s) to see whether IoT hub accepted the request.
        /// </returns>
        Task<MqttSubscribeAck> SubscribeAsync(MqttSubscribe subscribe, CancellationToken cancellationToken = default);

        /// <summary>
        /// Send an UNSUBSCRIBE to IoT hub. Any <see cref="DeviceIdPlaceholder"/> in a topic filter is replaced with the
        /// device's current ID when it is sent.
        /// </summary>
        /// <remarks>
        /// If the connection is lost before the UNSUBACK arrives, the UNSUBSCRIBE is sent again once the device is
        /// reconnected to IoT hub, including after re-provisioning.
        /// </remarks>
        /// <param name="unsubscribe">The unsubscribe to send.</param>
        /// <param name="cancellationToken">The cancellation token.</param>
        /// <returns>
        /// The UNSUBACK as received from IoT hub. This method does not throw if the UNSUBACK reports an error, so check its
        /// reason code(s) to see whether IoT hub accepted the request.
        /// </returns>
        Task<MqttUnsubscribeAck> UnsubscribeAsync(MqttUnsubscribe unsubscribe, CancellationToken cancellationToken = default);

        /// <remarks>
        /// Every handler is invoked and awaited in the order it was added, even if an earlier one throws.
        /// </remarks>
        event Func<MqttPublishReceivedEventArgs, Task> PublishReceivedAsync;
    }
}
