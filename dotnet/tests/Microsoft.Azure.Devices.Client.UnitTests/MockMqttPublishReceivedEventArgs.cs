using Microsoft.Azure.Devices.Client.Mqtt;
using System;
using System.Collections.Generic;
using System.Text;

namespace Microsoft.Azure.Devices.Client.UnitTests
{
    internal class MockMqttPublishReceivedEventArgs : MqttPublishReceivedEventArgs
    {
        bool IsAcknowledged { get; set; } = false;

        public override Task AcknowledgeAsync(CancellationToken cancellationToken)
        {
            IsAcknowledged = true;
            return Task.CompletedTask;
        }
    }
}
