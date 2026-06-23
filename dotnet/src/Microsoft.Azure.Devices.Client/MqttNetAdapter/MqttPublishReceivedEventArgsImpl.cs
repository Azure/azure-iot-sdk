using Microsoft.Azure.Devices.Client.Mqtt;
using MQTTnet;

namespace Microsoft.Azure.Devices.Client.MQTTnetAdapter
{
    internal class MqttPublishReceivedEventArgsImpl : MqttPublishReceivedEventArgs //TODO naming "Impl"
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
