using Microsoft.Azure.Iot.Device.IntegrationTests.Models;
using Microsoft.Azure.Iot.Device.Models.Telemetry;
using Microsoft.Azure.Iot.Device.Unified.Connection;
using Microsoft.Azure.Iot.Device.Unified.Telemetry;
using System.Text.Json;
using System.Text.Json.Serialization;
using Xunit;
using Microsoft.Azure.Devices;

namespace Microsoft.Azure.Iot.Device.IntegrationTests.Unified
{
    public class TelemetryClientIntegrationTests
    {
        [Theory(Timeout = Setup.TestTimeoutMilliseconds)]
        [InlineData(true)]
        [InlineData(false)]
        public async Task TestDeviceToCloudTelemetry(bool testAgainstClassicHub)
        {
            UnifiedDeviceTestContext testDeviceContext = await Setup.CreateConnectedUnifiedConnectionClientAsync(testAgainstClassicHub, null, TestContext.Current.CancellationToken);

            TelemetryClient telemetryClient = new TelemetryClient(testDeviceContext.ConnectionClient);

            DeviceToCloudTelemetry outgoingTelemetryMessage = new()
            {
                Payload = new byte[10],
            };

            await telemetryClient.SendTelemetryAsync(outgoingTelemetryMessage, TestContext.Current.CancellationToken);

            await testDeviceContext.DisposeAsync(); // Dispose this before any feature clients so that the test device identity can be cleaned up and the MQTT client disconnected gracefully
        }

        [Theory(Timeout = Setup.TestTimeoutMilliseconds)]
        [InlineData(true)]
        [InlineData(false)]
        public async Task TestDeviceToCloudTelemetryWithAllUserProperties(bool testAgainstClassicHub)
        {
            UnifiedDeviceTestContext testDeviceContext = await Setup.CreateConnectedUnifiedConnectionClientAsync(testAgainstClassicHub, null, TestContext.Current.CancellationToken);

            using TelemetryClient telemetryClient = new TelemetryClient(testDeviceContext.ConnectionClient);

            DeviceToCloudTelemetry outgoingTelemetryMessage = new()
            {
                Payload = JsonSerializer.SerializeToUtf8Bytes(new SimpleTelemetryObject() { SomeString = "SomeValue"}),
                ContentEncoding = "utf-8",
                ContentType = "application/json",
                MessageId = Guid.NewGuid().ToString(),
                CorrelationId = Guid.NewGuid().ToString(),
            };

            outgoingTelemetryMessage.UserProperties.Add("SomeUserPropertyKey", "SomeUserPropertyValue");

            await telemetryClient.SendTelemetryAsync(outgoingTelemetryMessage, TestContext.Current.CancellationToken);
        }

        [Theory(Timeout = Setup.TestTimeoutMilliseconds)]
        [InlineData(true)]
        [InlineData(false)]
        public async Task TelemetryClientEncodesUserProperties(bool testAgainstClassicHub)
        {
            UnifiedDeviceTestContext testDeviceContext = await Setup.CreateConnectedUnifiedConnectionClientAsync(testAgainstClassicHub, null, TestContext.Current.CancellationToken);

            using TelemetryClient telemetryClient = new TelemetryClient(testDeviceContext.ConnectionClient);

            DeviceToCloudTelemetry message = new()
            {
                MessageId = Guid.NewGuid().ToString(),
                CorrelationId = Guid.NewGuid().ToString(),
            };
            message.UserProperties.Add("!@#$%^&*()", "!@#$%%^&*()");

            await telemetryClient.SendTelemetryAsync(message, TestContext.Current.CancellationToken);

            await testDeviceContext.DisposeAsync(); // Dispose this before any feature clients so that the test device identity can be cleaned up and the MQTT client disconnected gracefully
        }

        [Theory(Timeout = Setup.TestTimeoutMilliseconds)]
        [InlineData(true)]
        [InlineData(false)]
        public async Task TestCloudToDeviceMessages(bool testAgainstClassicHub)
        {
            UnifiedDeviceTestContext testDeviceContext = await Setup.CreateConnectedUnifiedConnectionClientAsync(testAgainstClassicHub, null, TestContext.Current.CancellationToken);
            ConnectionClient connectionClient = testDeviceContext.ConnectionClient;

            ServiceClient serviceClient = Setup.GetGen1IotHubServiceClient();
            using TelemetryClient telemetryClient = new TelemetryClient(connectionClient);

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

            CloudToDeviceTelemetry receivedC2dMessage = await c2dMessageReceived.Task.WaitAsync(TestContext.Current.CancellationToken);

            Assert.Equal(expectedMessageId, receivedC2dMessage.MessageId);
            Assert.Equal(expectedCorrelationId, receivedC2dMessage.CorrelationId);
            Assert.True(Enumerable.SequenceEqual(expectedPayload, receivedC2dMessage.Payload));
            Assert.Equal(expectedContentType, receivedC2dMessage.ContentType);
            Assert.Equal(expectedContentEncoding, receivedC2dMessage.ContentEncoding);

            await testDeviceContext.DisposeAsync(); // Dispose this before any feature clients so that the test device identity can be cleaned up and the MQTT client disconnected gracefully
        }
    }
}
