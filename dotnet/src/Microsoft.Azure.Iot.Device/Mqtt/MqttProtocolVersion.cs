using System;
using System.Collections.Generic;
using System.Text;

namespace Microsoft.Azure.Iot.Device.Mqtt
{
    public enum MqttProtocolVersion
    {
        V311, // These are the only MQTT versions that IoT hub + DPS may communicate over
        V500
    }
}
