using System;
using System.Collections.Generic;
using System.Text;

namespace Microsoft.Azure.Devices.Client.Mqtt
{
    public class MqttUserProperty
    {
        public string Name { get; set; }

        public ReadOnlyMemory<byte> Value { get; set; }

        public MqttUserProperty(string name, byte[] value)
        {
            Name = name;
            Value = value;
        }

        public MqttUserProperty(string name, ReadOnlyMemory<byte> value)
        {
            Name = name;
            Value = value;
        }

        public MqttUserProperty(string name, string utf8Value)
        {
            Name = name;
            Value = Encoding.UTF8.GetBytes(utf8Value);
        }
    }
}
