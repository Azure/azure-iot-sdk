using Microsoft.Azure.Devices.Client.Models;
using Microsoft.Azure.Devices.Client.Mqtt;
using System;
using System.Collections.Generic;
using System.Text;

namespace Microsoft.Azure.Devices.Client.Unified.Connection
{
    // A stub class that is for converting Unified connection client traffic into Gen2 client traffic
    internal class Stub : Gen2.Connection.IConnectionClient
    {
        public event Func<MqttPublishReceivedEventArgs, Task> PublishReceivedAsync;

        private readonly Unified.Connection.IConnectionClient _unifiedConnectionClient;

        internal Stub(Unified.Connection.IConnectionClient unifiedConnectionClient)
        {
            _unifiedConnectionClient = unifiedConnectionClient;
            _unifiedConnectionClient.PublishReceivedAsync += PublishReceivedAsync;
        }

        public void Dispose()
        {
            _unifiedConnectionClient.PublishReceivedAsync -= PublishReceivedAsync;
            _unifiedConnectionClient.Dispose();
        }

        public ConnectionContext? GetCurrentConnectionContext()
        {
            return _unifiedConnectionClient.GetCurrentConnectionContext();
        }

        public Task<MqttPublishAck> PublishAsync(MqttPublish publish, CancellationToken cancellationToken = default)
        {
            return _unifiedConnectionClient.PublishAsync(publish, cancellationToken);
        }

        public Task<MqttSubscribeAck> SubscribeAsync(MqttSubscribe subscribe, CancellationToken cancellationToken = default)
        {
            return _unifiedConnectionClient.SubscribeAsync(subscribe, cancellationToken);
        }

        public Task<MqttUnsubscribeAck> UnsubscribeAsync(MqttUnsubscribe unsubscribe, CancellationToken cancellationToken = default)
        {
            return _unifiedConnectionClient.UnsubscribeAsync(unsubscribe, cancellationToken);
        }
    }
}
