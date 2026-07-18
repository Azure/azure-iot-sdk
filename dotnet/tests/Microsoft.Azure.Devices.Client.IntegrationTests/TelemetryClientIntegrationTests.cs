using Microsoft.Azure.Devices.Client.Telemetry;
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

            await using TestConnectionClient testDeviceContext = await Setup.CreateConnectedConnectionClientAsync(testAgainstClassicHub, cts.Token);

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

            await using TestConnectionClient testDeviceContext = await Setup.CreateConnectedConnectionClientAsync(testAgainstClassicHub, cts.Token);

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

            await using TestConnectionClient testDeviceContext = await Setup.CreateConnectedConnectionClientAsync(testAgainstClassicHub, cts.Token);

            TelemetryClient telemetryClient = new TelemetryClient(testDeviceContext.ConnectionClient);

            OutgoingTelemetryMessage message = new()
            {
                MessageId = Guid.NewGuid().ToString(),
                CorrelationId = Guid.NewGuid().ToString(),
            };
            message.UserProperties.Add("!@#$%^&*()", "!@#$%%^&*()");

            await telemetryClient.SendTelemetryAsync(message, TestContext.Current.CancellationToken);
        }

        public class TestObject
        {
            [JsonPropertyName("SomeString")]
            public string? SomeString { get; set; }
        }
    }
}
