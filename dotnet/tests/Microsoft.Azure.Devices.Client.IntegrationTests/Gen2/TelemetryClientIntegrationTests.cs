using Microsoft.Azure.Devices.Client.IntegrationTests.Gen2;
using Microsoft.Azure.Devices.Client.Models.Telemetry;
using Microsoft.Azure.Devices.Client.Gen2.Connection;
using Microsoft.Azure.Devices.Client.Gen2.Telemetry;
using System.Text.Json;
using System.Text.Json.Serialization;
using Xunit;

namespace Microsoft.Azure.Devices.Client.IntegrationTests
{
    public class TelemetryClientIntegrationTests
    {
        [Fact(Timeout = Setup.TestTimeoutMilliseconds)]
        public async Task TestDeviceToCloudTelemetry()
        {
            using CancellationTokenSource cts = new();
            cts.CancelAfter(Setup.TestTimeoutMilliseconds - 1000);

            await using Gen2DeviceTestContext testDeviceContext = await Setup.CreateConnectedGen2ConnectionClientAsync(null, cts.Token);

            TelemetryClient telemetryClient = new TelemetryClient(testDeviceContext.ConnectionClient);

            DeviceToCloudTelemetry outgoingTelemetryMessage = new()
            {
                Payload = new byte[10],
            };

            await telemetryClient.SendTelemetryAsync(outgoingTelemetryMessage, cts.Token);
        }

        [Fact(Timeout = Setup.TestTimeoutMilliseconds)]
        public async Task TestDeviceToCloudTelemetryWithAllUserProperties()
        {
            using CancellationTokenSource cts = new();
            cts.CancelAfter(Setup.TestTimeoutMilliseconds - 1000);

            await using Gen2DeviceTestContext testDeviceContext = await Setup.CreateConnectedGen2ConnectionClientAsync(null, cts.Token);

            TelemetryClient telemetryClient = new TelemetryClient(testDeviceContext.ConnectionClient);

            DeviceToCloudTelemetry outgoingTelemetryMessage = new()
            {
                Payload = JsonSerializer.SerializeToUtf8Bytes(new TestObject() { SomeString = "SomeValue"}),
                ContentEncoding = "utf-8",
                ContentType = "application/json",
                MessageId = Guid.NewGuid().ToString(),
                CorrelationId = Guid.NewGuid().ToString(),
            };

            outgoingTelemetryMessage.UserProperties.Add("SomeUserPropertyKey", "SomeUserPropertyValue");

            await telemetryClient.SendTelemetryAsync(outgoingTelemetryMessage, cts.Token);
        }

        [Fact(Timeout = Setup.TestTimeoutMilliseconds)]
        public async Task TestCloudToDeviceMessages()
        {
            using CancellationTokenSource cts = new();
            cts.CancelAfter(Setup.TestTimeoutMilliseconds - 1000);

            await using Gen2DeviceTestContext testDeviceContext = await Setup.CreateConnectedGen2ConnectionClientAsync(null, cts.Token);
            ConnectionClient connectionClient = testDeviceContext.ConnectionClient;

            ServiceClient serviceClient = Setup.GetGen1IotHubServiceClient();
            TelemetryClient telemetryClient = new TelemetryClient(connectionClient);

            TaskCompletionSource<CloudToDeviceTelemetry> c2dMessageReceived = new();

            telemetryClient.CloudToDeviceTelemetryReceivedAsync += (args) =>
            {
                c2dMessageReceived.TrySetResult(args);
                return Task.CompletedTask;
            };

            byte[] expectedPayload = Guid.NewGuid().ToByteArray();
            string expectedCorrelationId = Guid.NewGuid().ToString();
            string expectedMessageId = Guid.NewGuid().ToString();
            string expectedContentType = "SomeFakeContentType";
            string expectedContentEncoding = "SomeFakeContentEncoding";
            Message cloudToDeviceMessageToSend = new(expectedPayload)
            {
                MessageId = expectedMessageId,
                CorrelationId = expectedCorrelationId,
                ContentType = expectedContentType,
                ContentEncoding = expectedContentEncoding
            };

            await serviceClient.SendAsync(testDeviceContext.ConnectionContext.DeviceId, cloudToDeviceMessageToSend);

            CloudToDeviceTelemetry receivedC2dMessage = await c2dMessageReceived.Task.WaitAsync(cts.Token);

            Assert.Equal(expectedMessageId, receivedC2dMessage.MessageId);
            Assert.Equal(expectedCorrelationId, receivedC2dMessage.CorrelationId);
            Assert.True(Enumerable.SequenceEqual(expectedPayload, receivedC2dMessage.Payload));
            Assert.Equal(expectedContentType, receivedC2dMessage.ContentType);
            Assert.Equal(expectedContentEncoding, receivedC2dMessage.ContentEncoding);
        }


        public class TestObject
        {
            [JsonPropertyName("SomeString")]
            public string? SomeString { get; set; }
        }
    }
}
