using Microsoft.Azure.Devices.Client.Gen2.Twin;
using Microsoft.Azure.Devices.Client.IntegrationTests.Gen2;
using Microsoft.Azure.Devices.Client.Models.Twin;
using System.Text.Json.Nodes;
using Xunit;

namespace Microsoft.Azure.Devices.Client.IntegrationTests
{
    public class TwinClientIntegrationTests
    {
        [Fact(Timeout = Setup.TestTimeoutMilliseconds)]
        public async Task TestTwin()
        {
            using CancellationTokenSource cts = new();
            cts.CancelAfter(Setup.TestTimeoutMilliseconds - 1000);
            DeviceTwin initialTwin = new()
            {
                Desired = new JsonObject(),
                DesiredVersion = 5,
                Reported = new JsonObject(),
                ReportedVersion = 4,
            };

            initialTwin.Desired["someInitialDesiredPropertyKey"] = "someInitialDesiredPropertyValue";
            initialTwin.Reported["someInitialReportedPropertyKey"] = "someInitialReportedPropertyValue";

            // Want to defer connecting until TwinClient is set up to consume TwinPush
            await using Gen2DeviceTestContext testDeviceContext = await Setup.CreateConnectedGen2ConnectionClientAsync(initialTwin, cts.Token);
            
            //TODO
        }
    }
}
