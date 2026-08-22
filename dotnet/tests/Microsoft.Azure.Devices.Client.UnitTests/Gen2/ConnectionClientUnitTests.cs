using Microsoft.Azure.Devices.Client.Gen2.Connection;
using Microsoft.Azure.Devices.Client.Models;
using System;
using System.Collections.Generic;
using System.Text;
using Xunit;

namespace Microsoft.Azure.Devices.Client.UnitTests.Gen2
{
    public class ConnectionClientUnitTests
    {
        private static ConnectionContext GetMockConnectionContext()
        {
            return new ConnectionContext()
            {
                AuthenticationProvider = new X509AuthenticationProvider(new System.Security.Cryptography.X509Certificates.X509Certificate2()),
                DeviceId = "someDeviceId",
                IotHubHostName = "someHostName",
                IsGen2Hub = true
            };
        }


        [Fact]
        public async Task ConnectionClientReannouncesBirthBeforeContinuingPublish()
        {
            MockMqttClient mockMqttClient = new(true);
            ConnectionClient connectionClient = new(new() 
            { 
                MqttClient = mockMqttClient
            });

            await connectionClient.ConnectAsync(GetMockConnectionContext(), cancellationToken: TestContext.Current.CancellationToken);
        }
    }
}
