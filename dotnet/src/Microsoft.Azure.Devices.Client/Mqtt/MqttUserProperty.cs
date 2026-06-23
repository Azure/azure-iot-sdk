using System;
using System.Collections.Generic;
using System.Text;

namespace Microsoft.Azure.Devices.Client.Mqtt
{
    public class MqttUserProperty
    {
        public string Name { get; set; }

        public ReadOnlyMemory<byte> Value { get; set; }
    }
}
