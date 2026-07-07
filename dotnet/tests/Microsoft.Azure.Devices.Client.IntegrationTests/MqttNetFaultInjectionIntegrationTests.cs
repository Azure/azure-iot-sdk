using Microsoft.Azure.Devices.Client.Mqtt;
using Microsoft.Azure.Devices.Client.MQTTnetAdapter;
using Microsoft.Azure.Devices.Client.MQTTnetAdapter.Session;
using System;
using System.Collections.Generic;
using System.Text;
using Xunit;

namespace Microsoft.Azure.Devices.Client.IntegrationTests
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
        [Fact(Timeout = Setup.TestTimeoutMilliseconds)]
        public async Task Foo()
        {
            MqttSessionClient mqttClient = new();
            MqttConnect connectPacket = new()
            {
                HostName = "localhost",
                TcpPort = 1884,
                CleanSession = true,
                CleanStart = true,
                ClientId = Guid.NewGuid().ToString(),
            };
            var connAck = await mqttClient.ConnectAsync(connectPacket);
            Assert.Equal(MqttConnectResultCode.Success, connAck.ResultCode);
        }
    }
}
