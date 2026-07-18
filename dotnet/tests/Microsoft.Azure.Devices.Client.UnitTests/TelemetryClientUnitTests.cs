using Microsoft.Azure.Devices.Client.Telemetry;
using Moq;
using System;
using System.Collections.Generic;
using System.Text;
using Xunit;

namespace Microsoft.Azure.Devices.Client.UnitTests
{
    public class TelemetryClientUnitTests
    {
        [Fact]
        public async Task TelemetryClientThrowsIfMessageTooLarge()
        {
            Mock<ConnectionClient> mockConnectionClient = new();
            mockConnectionClient.Object.CurrentConnectionContext = new ConnectionContext()
            {
                DeviceId = Guid.NewGuid().ToString(),
                IsAzureEventGrid = false,
                IotHubHostName = "localhost"
            };

            TelemetryClient telemetryClient = new(mockConnectionClient.Object);

            OutgoingTelemetryMessage outgoingTelemetryMessage = new()
            {
                Payload = new byte[256000]
            };

            await Assert.ThrowsAsync<MessageTooLargeException>(async () => await telemetryClient.SendTelemetryAsync(outgoingTelemetryMessage, TestContext.Current.CancellationToken));
        }
    }
}
