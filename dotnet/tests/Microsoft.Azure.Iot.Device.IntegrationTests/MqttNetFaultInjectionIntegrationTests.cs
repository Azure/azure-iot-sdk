using Microsoft.Azure.Iot.Device.Mqtt;
using Microsoft.Azure.Iot.Device.MQTTnetAdapter;
using Microsoft.Azure.Iot.Device.Unified.Connection;
using System;
using System.Collections.Generic;
using System.Text;
using Xunit;

namespace Microsoft.Azure.Iot.Device.IntegrationTests 
{
    // This integration test suite has the MQTTnet client adapter connect to a faultable MQTT broker (sourced from the AIO SDK repo here:https://github.com/Azure/iot-operations-sdks/tree/main/eng/test/faultablemqttbroker/src/Azure.Iot.Operations.FaultableMqttBroker).
    //
    // This allows us to test scenarios where the MQTT connection drops unexpectedly.
    //
    // Note that these scenarios cannot be tested against IoT hub directly as AEG Hub does not support fault injection messages and not all versions of classic hub do either.
    // Additionally, we cannot use the top level ConnectionClient to test these scenarios as that client deliberately does not allow the injection of user properties into
    // MQTT-level packets.
    public class MqttNetFaultInjectionIntegrationTests
    {
        private MqttConnect CreateConnectPacket()
        {
            return new()
            {
                HostName = "localhost",
                ProtocolVersion = MqttProtocolVersion.V500, //TODO Do we want to try both MQTTv5 and mqttv3 brokers? mqttv3 doesn't support user properties, so this is difficult
                TcpPort = 1884,
                CleanSession = true,
                CleanStart = true,
                ClientId = Guid.NewGuid().ToString(),
            };
        }

        //TODO were these tests even applicable with the new kind of layering around handling disconnects? May need a stub IoT Hub running on the fault injection broker so that I can test the Connection client + feature client instead
        // If we do try to test via connection client directly, then mock broker needs to run on the same ports as real hub but on localhost. Need to skip DPS as well, probably

        // Maybe the smart thing is to just mock the MQTT client directly instead. Doesn't help C side, unfortunately
    }
}
