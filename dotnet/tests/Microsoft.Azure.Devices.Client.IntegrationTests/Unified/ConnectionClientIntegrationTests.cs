using CaptureProxy;
using Microsoft.Azure.Devices.Client.Models;
using Microsoft.Azure.Devices.Client.MqttNetAdapter;
using Microsoft.Azure.Devices.Client.MQTTnetAdapter;
using System;
using System.Collections.Generic;
using System.Net;
using System.Text;
using Xunit;

namespace Microsoft.Azure.Devices.Client.IntegrationTests.Unified
{
    public class ConnectionClientIntegrationTests
    {
        [Theory(Timeout = Setup.TestTimeoutMilliseconds)]
        [InlineData(true)]
        [InlineData(false)]
        public async Task CanConnectOverWebsocket(bool isClassicHub)
        {
            MqttNetClientOptions mqttClientOptions = new()
            {
                UseWebsocket = true,
            };

            ConnectionClientOptions connectionClientOptions = new()
            {
                MqttClient = new MqttNetClient(mqttClientOptions)
            };

            UnifiedDeviceTestContext testDeviceContext = await Setup.CreateConnectedUnifiedConnectionClientAsync(isClassicHub, connectionClientOptions, TestContext.Current.CancellationToken);

            await testDeviceContext.DisposeAsync(); // Dispose this before any feature clients so that the test device identity can be cleaned up and the MQTT client disconnected gracefully
        }

        [Theory(Timeout = Setup.TestTimeoutMilliseconds)]
        [InlineData(true)]
        [InlineData(false)]
        public async Task CanConnectOverWebsocketAndHttpProxy(bool isClassicHub)
        {
            int proxyPort = isClassicHub ? 8877 : 8878; // Use a different port per test so they don't collide if run in parallel
            var httpProxy = new HttpProxy(proxyPort);

            // Start the proxy server
            httpProxy.Start();


            MqttNetClientOptions mqttClientOptions = new()
            {
                UseWebsocket = true,
                Proxy = new WebProxy("localhost", proxyPort)
            };

            ConnectionClientOptions connectionClientOptions = new()
            {
                MqttClient = new MqttNetClient(mqttClientOptions)
            };

            UnifiedDeviceTestContext testDeviceContext = await Setup.CreateConnectedUnifiedConnectionClientAsync(isClassicHub, connectionClientOptions, TestContext.Current.CancellationToken);

            await testDeviceContext.DisposeAsync(); // Dispose this before any feature clients so that the test device identity can be cleaned up and the MQTT client disconnected gracefully


            // Stop the proxy server
            httpProxy.Stop();
            httpProxy.Dispose();
        }
    }
}
