// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Mqtt;

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
        /// Send a PUBLISH to IoT hub. Any <see cref="DeviceIdPlaceholder"/> in the topic is replaced with the device's
        /// current ID when it is sent.
        /// </summary>
        Task<MqttPublishAck> PublishAsync(MqttPublish publish, CancellationToken cancellationToken = default);

        /// <summary>
        /// Send a SUBSCRIBE to IoT hub. Any <see cref="DeviceIdPlaceholder"/> in a topic filter is replaced with the
        /// device's current ID when it is sent.
        /// </summary>
        Task<MqttSubscribeAck> SubscribeAsync(MqttSubscribe subscribe, CancellationToken cancellationToken = default);

        /// <summary>
        /// Send an UNSUBSCRIBE to IoT hub. Any <see cref="DeviceIdPlaceholder"/> in a topic filter is replaced with the
        /// device's current ID when it is sent.
        /// </summary>
        Task<MqttUnsubscribeAck> UnsubscribeAsync(MqttUnsubscribe unsubscribe, CancellationToken cancellationToken = default);

        event Func<MqttPublishReceivedEventArgs, Task> PublishReceivedAsync;
    }
}
