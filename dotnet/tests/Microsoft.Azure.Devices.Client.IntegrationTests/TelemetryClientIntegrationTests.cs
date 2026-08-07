using Microsoft.Azure.Devices.Client.Models;
using Microsoft.Azure.Devices.Client.Unified.Connection;
using Microsoft.Azure.Devices.Client.Unified.Telemetry;
using System.Text.Json;
using System.Text.Json.Serialization;
using Xunit;

namespace Microsoft.Azure.Devices.Client.IntegrationTests
{
    public class TelemetryClientIntegrationTests
    {
        [Theory(Timeout = Setup.TestTimeoutMilliseconds)]
        [InlineData(true)]
        [InlineData(false)]
        public async Task TestDeviceToCloudTelemetry(bool testAgainstClassicHub)
        {
            using CancellationTokenSource cts = new();
            cts.CancelAfter(Setup.TestTimeoutMilliseconds - 1000);

            await using TestConnectionClient testDeviceContext = await Setup.CreateConnectedUnifiedConnectionClientAsync(testAgainstClassicHub, cts.Token);

            TelemetryClient telemetryClient = new TelemetryClient(testDeviceContext.ConnectionClient);

            OutgoingTelemetryMessage outgoingTelemetryMessage = new()
            {
                Payload = new byte[10],
            };

            await telemetryClient.SendTelemetryAsync(outgoingTelemetryMessage, cts.Token);
        }

        [Theory(Timeout = Setup.TestTimeoutMilliseconds)]
        [InlineData(true)]
        [InlineData(false)]
        public async Task TestDeviceToCloudTelemetryWithAllUserProperties(bool testAgainstClassicHub)
        {
            using CancellationTokenSource cts = new();
            cts.CancelAfter(Setup.TestTimeoutMilliseconds - 1000);

            await using TestConnectionClient testDeviceContext = await Setup.CreateConnectedUnifiedConnectionClientAsync(testAgainstClassicHub, cts.Token);

            TelemetryClient telemetryClient = new TelemetryClient(testDeviceContext.ConnectionClient);

            OutgoingTelemetryMessage outgoingTelemetryMessage = new()
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

        [Theory(Timeout = Setup.TestTimeoutMilliseconds)]
        [InlineData(true)]
        [InlineData(false)]
        public async Task TelemetryClientEncodesUserProperties(bool testAgainstClassicHub)
        {
            using CancellationTokenSource cts = new();
            cts.CancelAfter(Setup.TestTimeoutMilliseconds - 1000);

            await using TestConnectionClient testDeviceContext = await Setup.CreateConnectedUnifiedConnectionClientAsync(testAgainstClassicHub, cts.Token);

            TelemetryClient telemetryClient = new TelemetryClient(testDeviceContext.ConnectionClient);

            OutgoingTelemetryMessage message = new()
            {
                MessageId = Guid.NewGuid().ToString(),
                CorrelationId = Guid.NewGuid().ToString(),
            };
            message.UserProperties.Add("!@#$%^&*()", "!@#$%%^&*()");

            await telemetryClient.SendTelemetryAsync(message, TestContext.Current.CancellationToken);
        }

        [Theory(Timeout = Setup.TestTimeoutMilliseconds)]
        [InlineData(true)]
        [InlineData(false)]
        public async Task TestCloudToDeviceMessages(bool testAgainstClassicHub)
        {
            using CancellationTokenSource cts = new();
            cts.CancelAfter(Setup.TestTimeoutMilliseconds - 1000);

            await using TestConnectionClient testDeviceContext = await Setup.CreateConnectedUnifiedConnectionClientAsync(testAgainstClassicHub, cts.Token);
            ConnectionClient connectionClient = testDeviceContext.ConnectionClient;

            ServiceClient serviceClient = Setup.GetIotHubServiceClient();
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
