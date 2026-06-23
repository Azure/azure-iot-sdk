using Microsoft.Azure.Devices.Client.Telemetry;
using Xunit;

namespace Microsoft.Azure.Devices.Client.IntegrationTests
{
    public class TelemetryClientIntegrationTests
    {
        [Theory(Timeout = Setup.TestTimeoutMilliseconds)]
        [InlineData(true)]
        [InlineData(false)]
        public async Task TestCloudToDeviceMessages(bool testAgainstClassicHub)
        {
            using CancellationTokenSource cts = new();
            cts.CancelAfter(Setup.TestTimeoutMilliseconds - 1000);

            await using TestConnectionClient testDeviceContext = await Setup.CreateConnectedConnectionClientAsync(testAgainstClassicHub, cts.Token);
            ConnectionClient connectionClient = testDeviceContext.ConnectionClient;

            ServiceClient serviceClient = Setup.GetIotHubServiceClient();
            TelemetryClient telemetryClient = new TelemetryClient(connectionClient);

            TaskCompletionSource<CloudToDeviceMessage> c2dMessageReceived = new();

            telemetryClient.CloudToDeviceMessageReceivedAsync += (args) =>
            {
                c2dMessageReceived.TrySetResult(args);
                return Task.FromResult(CompletionType.Complete);
            };

            Message cloudToDeviceMessageToSend = new()
            {
                MessageId = Guid.NewGuid().ToString(),
            };

            await serviceClient.SendAsync(testDeviceContext.ConnectionContext.DeviceId, cloudToDeviceMessageToSend);

            CloudToDeviceMessage receivedC2dMessage = await c2dMessageReceived.Task.WaitAsync(cts.Token);

            Assert.Equal(cloudToDeviceMessageToSend.MessageId, receivedC2dMessage.MessageId);
        }

        [Theory(Timeout = Setup.TestTimeoutMilliseconds)]
        [InlineData(true)]
        [InlineData(false)]
        public async Task TestDeviceToCloudTelemetry(bool testAgainstClassicHub)
        {
            using CancellationTokenSource cts = new();
            cts.CancelAfter(Setup.TestTimeoutMilliseconds - 1000);

            await using TestConnectionClient testDeviceContext = await Setup.CreateConnectedConnectionClientAsync(testAgainstClassicHub, cts.Token);
            ConnectionClient connectionClient = testDeviceContext.ConnectionClient;

            TelemetryClient telemetryClient = new TelemetryClient(connectionClient);

            OutgoingTelemetryMessage outgoingTelemetryMessage = new()
            {
                Payload = new byte[10],
            };

            await telemetryClient.SendTelemetryAsync(outgoingTelemetryMessage, cts.Token);
        }
    }
}
