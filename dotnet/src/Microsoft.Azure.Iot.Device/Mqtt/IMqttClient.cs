// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

namespace Microsoft.Azure.Iot.Device.Mqtt
{
    public interface IMqttClient : IDisposable
    {
        event Func<MqttPublishReceivedEventArgs, Task> PublishReceivedAsync;

        event Func<MqttClientConnectedEventArgs, Task> ConnectedAsync;

        event Func<MqttConnect, Task<MqttConnect>> ConnectingAsync;

        event Func<MqttClientDisconnectedEventArgs, Task> DisconnectedAsync;

        Task<MqttConnectAck> ConnectAsync(MqttConnect connect, CancellationToken cancellationToken = default);

        Task DisconnectAsync(MqttDisconnect disconnect, CancellationToken cancellationToken = default);

        Task<MqttPublishAck> PublishAsync(MqttPublish publish, CancellationToken cancellationToken = default);

        Task<MqttSubscribeAck> SubscribeAsync(MqttSubscribe subscribe, CancellationToken cancellationToken = default);

        Task<MqttUnsubscribeAck> UnsubscribeAsync(MqttUnsubscribe unsubscribe, CancellationToken cancellationToken = default);

        public bool IsConnected();
    }
}
