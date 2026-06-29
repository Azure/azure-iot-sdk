using Microsoft.Azure.Devices.Client.Telemetry;
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
    }
}
