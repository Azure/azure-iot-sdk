using System;
using System.Collections.Generic;
using System.Text;

namespace Microsoft.Azure.Devices.Client.Mqtt
{
    public class MqttClientNotConnectedException : Exception
    {
        public MqttClientNotConnectedException(string message, Exception e) : base(message, e)
        { 
        
        }
    }
}
