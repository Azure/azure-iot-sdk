// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Mqtt;
using MQTTnet;

namespace Microsoft.Azure.Iot.Device.MQTTnetAdapter
{
    internal class MqttPublishReceivedEventArgsImpl : MqttPublishReceivedEventArgs
    {
        private MqttApplicationMessageReceivedEventArgs _underlyingEventArgs;
        public MqttPublishReceivedEventArgsImpl(MqttApplicationMessageReceivedEventArgs UnderlyingEventArgs)
        {
            _underlyingEventArgs = UnderlyingEventArgs;
        }

        public override async Task AcknowledgeAsync(CancellationToken cancellationToken)
        {
            await _underlyingEventArgs.AcknowledgeAsync(cancellationToken);
        }
    }
}
