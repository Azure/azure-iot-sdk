// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Mqtt;
using Microsoft.Azure.Iot.Device.Unified.Models;

namespace Microsoft.Azure.Iot.Device.Unified.Connection
{
    public interface IConnectionClient : IDisposable //TODO the MQTTv5 and unified connection client interfaces are now the same. It may be worth keeping them separate though just so that users don't try to plug in MQTTv5 connections into unified feature clients?
    {
        /// <summary>
        /// The stand-in for this device's ID in any topic passed to <see cref="PublishAsync"/>,
        /// <see cref="SubscribeAsync"/> or <see cref="UnsubscribeAsync"/>.
        /// </summary>
        /// <remarks>
        /// A device's ID can change while an operation is pending (for example, when the device is re-provisioned and
        /// assigned a different device ID), so feature clients must not bake the device ID into a topic themselves.
        /// Instead, topics use this placeholder (for example, "devices/{deviceId}/messages/events/") and the connection
        /// client substitutes the device's current ID each time it actually sends the operation.
        /// </remarks>
        public const string DeviceIdPlaceholder = "{deviceId}";

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
        /// An ID that the connection client generates for each connection to IoT hub, which IoT hub never sees, or null if it
        /// is not known.
        /// </summary>
        /// <remarks>
        /// The twin client uses this to tell responses and held reported properties patches of one connection apart from
        /// those of the next.
        /// </remarks>
        internal Guid? ConnectionNonce => null;

        /// <summary>
        /// Raised whenever the connection to IoT hub is lost or closed, before any reconnection starts.
        /// </summary>
        internal event Action? ConnectionLost { add { } remove { } }

        /// <summary>
        /// Raised whenever the device presence flow completes on a connection to IoT hub, which means that the twin topics
        /// are subscribed to, so that IoT hub delivers the responses to twin requests.
        /// </summary>
        /// <remarks>
        /// IoT hub neither acknowledges a device's presence nor pushes its twin upon connecting, so the twin client uses this
        /// to fetch the twin instead.
        /// </remarks>
        internal event Action? ClassicDevicePresenceEstablished { add { } remove { } }

        /// <summary>
        /// Whether the device presence flow has completed on the current connection to IoT hub. See
        /// <see cref="ClassicDevicePresenceEstablished"/>.
        /// </summary>
        internal bool IsClassicDevicePresenceEstablished => false;

        /// <summary>
        /// Whether this connection client subscribes to twin topics. See <see cref="ConnectionClientOptions.EnableTwin"/>.
        /// </summary>
        bool IsTwinEnabled => true;

        /// <summary>
        /// Whether this connection client subscribes to direct method topics. See
        /// <see cref="ConnectionClientOptions.EnableDirectMethods"/>.
        /// </summary>
        bool AreDirectMethodsEnabled => true;

        /// <summary>
        /// Whether this connection client subscribes to cloud-to-device message topics. See
        /// <see cref="ConnectionClientOptions.EnableCloudToDeviceMessages"/>.
        /// </summary>
        bool AreCloudToDeviceMessagesEnabled => true;

        /// <summary>
        /// Send a PUBLISH to IoT hub. Any <see cref="DeviceIdPlaceholder"/> in the topic is replaced with the device's
        /// current ID when it is sent.
        /// </summary>
        /// <remarks>
        /// If the connection is lost before a QoS 1 PUBLISH is acknowledged, it is sent again once the device is
        /// reconnected to IoT hub (including after re-provisioning, in which case it is sent to the newly assigned hub
        /// with the newly assigned device ID), so IoT hub may receive it more than once.
        /// </remarks>
        /// <param name="publish">The publish to send.</param>
        /// <param name="cancellationToken">The cancellation token.</param>
        /// <returns>
        /// The PUBACK as received from IoT hub. This method does not throw if the PUBACK reports an error, so check its
        /// reason code(s) to see whether IoT hub accepted the request.
        /// </returns>
        Task<MqttPublishAck> PublishAsync(MqttPublish publish, CancellationToken cancellationToken = default);

        /// <summary>
        /// Send a SUBSCRIBE to IoT hub. Any <see cref="DeviceIdPlaceholder"/> in a topic filter is replaced with the
        /// device's current ID when it is sent.
        /// </summary>
        /// <remarks>
        /// If the connection is lost before the SUBACK arrives, the SUBSCRIBE is sent again once the device is reconnected
        /// to IoT hub, including after re-provisioning.
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

        event Func<MqttPublishReceivedEventArgs, Task> PublishReceivedAsync;
    }
}
