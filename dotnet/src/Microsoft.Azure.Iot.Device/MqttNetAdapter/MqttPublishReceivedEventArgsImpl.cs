using Microsoft.Azure.Iot.Device.Mqtt;
using MQTTnet;

namespace Microsoft.Azure.Iot.Device.MQTTnetAdapter
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
