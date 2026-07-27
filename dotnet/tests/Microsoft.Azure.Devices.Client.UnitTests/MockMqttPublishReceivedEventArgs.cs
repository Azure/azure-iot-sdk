using Microsoft.Azure.Devices.Client.Mqtt;
using MQTTnet;
using System;
using System.Collections.Generic;
using System.Text;

namespace Microsoft.Azure.Devices.Client.UnitTests
{
    internal class MockMqttPublishReceivedEventArgs : MqttPublishReceivedEventArgs
    {
        public MockMqttPublishReceivedEventArgs()
        {
        }

        public override async Task AcknowledgeAsync(CancellationToken cancellationToken)
        {
            // Do nothing. No test needs to check this currently
        }
    }
}
