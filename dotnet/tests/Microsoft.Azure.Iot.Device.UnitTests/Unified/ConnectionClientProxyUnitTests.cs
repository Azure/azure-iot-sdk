// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Authentication;
using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.MqttNetAdapter;
using Microsoft.Azure.Iot.Device.MQTTnetAdapter;
using Microsoft.Azure.Iot.Device.Retry;
using Microsoft.Azure.Iot.Device.Unified.Connection;
using Microsoft.Azure.Iot.Device.Unified.Models;
using System.Net;
using System.Text;
using Xunit;

namespace Microsoft.Azure.Iot.Device.UnitTests.Unified
{
    public class ConnectionClientProxyUnitTests
    {
        private const string GlobalDeviceEndpoint = "global.azure-devices-provisioning.net";
        private const string IdScope = "0ne00000000";
        private const string RegistrationId = "someRegistrationId";

        private static readonly TimeSpan s_testTimeout = TimeSpan.FromSeconds(30);

        [Theory]
        [InlineData(false)]
        [InlineData(true)]
        public async Task MqttConnectionsGoThroughTheConfiguredProxy(bool useWebsocket)
        {
            using LoopbackConnectProxy proxy = new();
            using ConnectionClient connectionClient = new(new ConnectionClientOptions
            {
                Proxy = new WebProxy(proxy.Address),
                UseWebsocket = useWebsocket, // A proxy implies websockets, since only they can be tunnelled through it
                ConnectionRetryPolicy = new NoRetry(),
            });

            await ProvisionExpectingFailureAsync(connectionClient);

            // MQTT reaches the proxy as a websocket tunnelled to port 443.
            Assert.Contains($"CONNECT {GlobalDeviceEndpoint}:443 HTTP/1.1", proxy.RequestLines);
        }

        [Fact]
        public async Task SuppliedMqttClientsProxyTakesPrecedenceForMqttConnections()
        {
            using LoopbackConnectProxy connectionClientProxy = new();
            using LoopbackConnectProxy mqttClientProxy = new();
            using ConnectionClient connectionClient = new(new ConnectionClientOptions
            {
                Proxy = new WebProxy(connectionClientProxy.Address),
                ConnectionRetryPolicy = new NoRetry(),
                MqttClient = new MqttNetClient(new MqttNetClientOptions { UseWebsocket = true, Proxy = new WebProxy(mqttClientProxy.Address) }),
            });

            await ProvisionExpectingFailureAsync(connectionClient);

            Assert.Contains($"CONNECT {GlobalDeviceEndpoint}:443 HTTP/1.1", mqttClientProxy.RequestLines);
            Assert.Empty(connectionClientProxy.RequestLines);
        }

        private static async Task ProvisionExpectingFailureAsync(ConnectionClient connectionClient)
        {
            await Assert.ThrowsAnyAsync<Exception>(
                async () => await connectionClient
                    .ProvisionAndConnectAsync(
                        new ProvisioningSettings(IdScope) { RegistrationId = RegistrationId, GlobalEndpointAddress = GlobalDeviceEndpoint },
                        new SymmetricKeyAuthenticationProvider(RegistrationId, IdScope, Encoding.UTF8.GetBytes("some-key"), null),
                        cancellationToken: TestContext.Current.CancellationToken)
                    .WaitAsync(s_testTimeout, TestContext.Current.CancellationToken));
        }
    }
}
