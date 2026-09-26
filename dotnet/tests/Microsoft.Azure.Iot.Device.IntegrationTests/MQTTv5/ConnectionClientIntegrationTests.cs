// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using CaptureProxy;
using Microsoft.Azure.Iot.Device.Gen2.Connection;
using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.MqttNetAdapter;
using Microsoft.Azure.Iot.Device.MQTTnetAdapter;
using System;
using System.Collections.Generic;
using System.Net;
using System.Text;
using Xunit;

namespace Microsoft.Azure.Iot.Device.IntegrationTests.MQTTv5
{
    public class ConnectionClientIntegrationTests
    {
        [Fact(Timeout = Setup.TestTimeoutMilliseconds)]
        public async Task CanConnectOverWebsocket()
        {
            MqttNetClientOptions mqttClientOptions = new()
            {
                UseWebsocket = true,
            };

            ConnectionClientOptions connectionClientOptions = new()
            {
                MqttClient = new MqttNetClient(mqttClientOptions)
            };

            MQTTv5DeviceTestContext testDeviceContext = await Setup.CreateConnectedMQTTv5ConnectionClientAsync(null, connectionClientOptions, TestContext.Current.CancellationToken);

            await testDeviceContext.DisposeAsync(); // Dispose this before any feature clients so that the test device identity can be cleaned up and the MQTT client disconnected gracefully
        }

        [Fact(Timeout = Setup.TestTimeoutMilliseconds)]
        public async Task CanConnectOverWebsocketAndHttpProxy()
        {
            int proxyPort = 8879; // Use a port that won't collide with any other proxy test
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

            MQTTv5DeviceTestContext testDeviceContext = await Setup.CreateConnectedMQTTv5ConnectionClientAsync(null, connectionClientOptions, TestContext.Current.CancellationToken);

            await testDeviceContext.DisposeAsync(); // Dispose this before any feature clients so that the test device identity can be cleaned up and the MQTT client disconnected gracefully

            // Stop the proxy server
            httpProxy.Stop();
            httpProxy.Dispose();
        }
    }
}
