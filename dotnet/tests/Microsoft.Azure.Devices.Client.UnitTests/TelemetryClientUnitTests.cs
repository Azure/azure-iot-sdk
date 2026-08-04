using Microsoft.Azure.Devices.Client.Connection.Models;
using Microsoft.Azure.Devices.Client.Telemetry.Models;
using Microsoft.Azure.Devices.Client.Telemetry.Unified;
using Moq;
using System;
using System.Collections.Generic;
using System.Text;
using Xunit;

namespace Microsoft.Azure.Devices.Client.UnitTests
{
    public class TelemetryClientUnitTests
    {
        /*
        [Theory]
        [InlineData(true)]
        [InlineData(false)]
        public async Task TelemetryClientThrowsIfMessageTooLarge_Unified(bool testUnifiedClient)
        {
            MockConnectionClient mockConnectionClient = new();
            mockConnectionClient.SetCurrentConnectionContext(new ConnectionContext()
            {
                DeviceId = "SomeDeviceId",
                IsAzureEventGrid = false,
                IotHubHostName = "SomeHostName",
            });
            TelemetryClient telemetryClient = new(mockConnectionClient);

            OutgoingTelemetryMessage outgoingTelemetryMessage = new()
            {
                Payload = new byte[256000]
            };

            await Assert.ThrowsAsync<MessageTooLargeException>(async () => await telemetryClient.SendTelemetryAsync(outgoingTelemetryMessage, TestContext.Current.CancellationToken));
        }

        private Connection.Unified.ConnectionClient GetUnifiedClient()
        { 
        
        }

        private Connection.Gen2.ConnectionClient GetGen2Client()
        {

        }
        */ //TODO
    }
}
