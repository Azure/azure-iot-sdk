// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Mqtt;
using System;
using System.Collections.Generic;
using System.Text;

namespace Microsoft.Azure.Iot.Device.Unified.Connection
{
    // A stub class that is for converting Unified connection client traffic into Gen2 client traffic
    internal class Stub : Gen2.Connection.IConnectionClient
    {
        public event Func<MqttPublishReceivedEventArgs, Task> PublishReceivedAsync;

        private readonly Unified.Connection.IConnectionClient _unifiedConnectionClient;

        internal Stub(Unified.Connection.IConnectionClient unifiedConnectionClient)
        {
            _unifiedConnectionClient = unifiedConnectionClient;
            _unifiedConnectionClient.PublishReceivedAsync += ForwardPublishReceivedAsync;
        }

        // Forward received publishes through a stable method reference. Subscribing the 'PublishReceivedAsync' event
        // field directly would capture its (null) backing delegate at construction time, so later subscribers (the
        // underlying gen2 feature clients) would never be invoked.
        private Task ForwardPublishReceivedAsync(MqttPublishReceivedEventArgs args)
        {
            return PublishReceivedAsync?.Invoke(args) ?? Task.CompletedTask;
        }

        public void Dispose()
        {
            _unifiedConnectionClient.PublishReceivedAsync -= ForwardPublishReceivedAsync;
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
