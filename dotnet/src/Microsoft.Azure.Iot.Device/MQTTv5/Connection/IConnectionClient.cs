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
        /// Whether twin subscriptions and automatic twin pushes are enabled. See <see cref="ConnectionClientOptions.EnableTwin"/>.
        /// </summary>
        bool IsTwinEnabled => true;

        /// <summary>
        /// Whether direct method subscriptions are enabled. See <see cref="ConnectionClientOptions.EnableDirectMethods"/>.
        /// </summary>
        bool AreDirectMethodsEnabled => true;

        /// <summary>
        /// Whether cloud-to-device messages are enabled. MQTTv5 cloud-to-device receiving is not currently supported.
        /// See <see cref="ConnectionClientOptions.EnableCloudToDeviceMessages"/>.
        /// </summary>
        bool AreCloudToDeviceMessagesEnabled => true;

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
        /// <param name="throwOnErrorReasonCode">
        /// True to throw a <see cref="DeviceException"/> if the PUBACK reports an error. False to return the PUBACK as-is
        /// whatever its reason code. Either way, a QoS 1 PUBLISH interrupted by a connection-level failure is re-sent once
        /// the device is connected again.
        /// </param>
        /// <param name="cancellationToken">The cancellation token.</param>
        Task<MqttPublishAck> PublishAsync(MqttPublish publish, bool throwOnErrorReasonCode, CancellationToken cancellationToken = default);

        /// <summary>
        /// Send a SUBSCRIBE to IoT hub. Any <see cref="DeviceIdPlaceholder"/> in a topic filter is replaced with the
        /// device's current ID when it is sent.
        /// </summary>
        /// <remarks>
        /// If the connection is lost before the SUBACK arrives, the SUBSCRIBE is sent again once the device is
        /// reconnected to IoT hub, including after re-provisioning.
        /// </remarks>
        /// <param name="subscribe">The subscribe to send.</param>
        /// <param name="throwOnErrorReasonCode">
        /// True to throw a <see cref="DeviceException"/> if the SUBACK rejects a topic filter. False to return the SUBACK
        /// as-is whatever its reason codes. Either way, an operation interrupted by a connection-level failure is re-sent
        /// once the device is connected again.
        /// </param>
        /// <param name="cancellationToken">The cancellation token.</param>
        Task<MqttSubscribeAck> SubscribeAsync(MqttSubscribe subscribe, bool throwOnErrorReasonCode, CancellationToken cancellationToken = default);

        /// <summary>
        /// Send an UNSUBSCRIBE to IoT hub. Any <see cref="DeviceIdPlaceholder"/> in a topic filter is replaced with the
        /// device's current ID when it is sent.
        /// </summary>
        /// <remarks>
        /// If the connection is lost before the UNSUBACK arrives, the UNSUBSCRIBE is sent again once the device is
        /// reconnected to IoT hub, including after re-provisioning.
        /// </remarks>
        /// <param name="unsubscribe">The unsubscribe to send.</param>
        /// <param name="throwOnErrorReasonCode">
        /// True to throw a <see cref="DeviceException"/> if the UNSUBACK rejects a topic filter. False to return the
        /// UNSUBACK as-is whatever its reason codes. Either way, an operation interrupted by a connection-level failure is
        /// re-sent once the device is connected again.
        /// </param>
        /// <param name="cancellationToken">The cancellation token.</param>
        Task<MqttUnsubscribeAck> UnsubscribeAsync(MqttUnsubscribe unsubscribe, bool throwOnErrorReasonCode, CancellationToken cancellationToken = default);

        /// <remarks>
        /// Every handler is invoked and awaited in the order it was added, even if an earlier one throws.
        /// </remarks>
        event Func<MqttPublishReceivedEventArgs, Task> PublishReceivedAsync;
    }
}
