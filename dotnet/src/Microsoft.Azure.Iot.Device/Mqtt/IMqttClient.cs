// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

namespace Microsoft.Azure.Iot.Device.Mqtt
{
    public interface IMqttClient : IDisposable
    {
        event Func<MqttPublishReceivedEventArgs, Task> PublishReceivedAsync;

        event Func<MqttClientConnectedEventArgs, Task> ConnectedAsync;

        event Func<MqttConnect, CancellationToken, Task<MqttConnect>> ConnectingAsync;

        /// <remarks>
        /// Raise this whenever a connection or connect attempt ends. It may be raised for a connect attempt that never
        /// connected, more than once, or late: the connection client only treats it as a connection loss if a connection
        /// was reported through <see cref="ConnectedAsync"/> and <see cref="IsConnected"/> is <c>false</c>.
        /// </remarks>
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
