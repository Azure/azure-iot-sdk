// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

namespace Microsoft.Azure.Iot.Device.Mqtt
{
    public interface IMqttClient : IDisposable
    {
        event Func<MqttPublishReceivedEventArgs, Task> PublishReceivedAsync;

        event Func<MqttClientConnectedEventArgs, Task> ConnectedAsync;

        event Func<MqttConnect, CancellationToken, Task<MqttConnect>> ConnectingAsync;

        event Func<MqttClientDisconnectedEventArgs, Task> DisconnectedAsync;

        Task<MqttConnectAck> ConnectAsync(MqttConnect connect, CancellationToken cancellationToken = default);

        Task DisconnectAsync(MqttDisconnect disconnect, CancellationToken cancellationToken = default);

        /// <remarks>
        /// Throw <see cref="MqttClientNotConnectedException"/> if the client is not connected, or if the connection is lost
        /// before a QoS 1 PUBLISH is acknowledged; the connection client then sends it again once reconnected.
        /// </remarks>
        Task<MqttPublishAck> PublishAsync(MqttPublish publish, CancellationToken cancellationToken = default);

        /// <remarks>
        /// Throw <see cref="MqttClientNotConnectedException"/> if the client is not connected, or if the connection is lost
        /// before the SUBACK arrives; the connection client then sends it again once reconnected.
        /// </remarks>
        Task<MqttSubscribeAck> SubscribeAsync(MqttSubscribe subscribe, CancellationToken cancellationToken = default);

        /// <remarks>
        /// Throw <see cref="MqttClientNotConnectedException"/> if the client is not connected, or if the connection is lost
        /// before the UNSUBACK arrives; the connection client then sends it again once reconnected.
        /// </remarks>
        Task<MqttUnsubscribeAck> UnsubscribeAsync(MqttUnsubscribe unsubscribe, CancellationToken cancellationToken = default);

        public bool IsConnected();
    }
}
