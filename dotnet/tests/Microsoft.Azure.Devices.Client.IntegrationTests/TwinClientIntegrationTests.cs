using Microsoft.Azure.Devices.Client.Twin;
using Microsoft.Azure.Devices.Client.Twin.Models;
using Microsoft.Azure.Devices.Client.Twin.Unified;
using System.Text.Json.Nodes;
using Xunit;

namespace Microsoft.Azure.Devices.Client.IntegrationTests
{
    public class TwinClientIntegrationTests
    {
        [Theory(Timeout = Setup.TestTimeoutMilliseconds)]
        [InlineData(true)]
        [InlineData(false)]
        public async Task TestTwin(bool testAgainstClassicHub)
        {
            using CancellationTokenSource cts = new();
            cts.CancelAfter(Setup.TestTimeoutMilliseconds - 1000);
            await using TestConnectionClient testDeviceContext = await Setup.CreateConnectedConnectionClientAsync(testAgainstClassicHub, cts.Token);
            string deviceId = testDeviceContext.ConnectionContext.DeviceId;

            RegistryManager registryManager = Setup.GetIotHubRegistryManager();
            using TwinClient twinClient = new TwinClient(testDeviceContext.ConnectionClient);
            TaskCompletionSource<DesiredPatchReceivedEventArgs> onDesiredPropertiesUpdateReceived = new();
            int desiredPatchesReceived = 0;
            twinClient.DesiredPatchReceived += (args) =>
            {
                desiredPatchesReceived++;
                onDesiredPropertiesUpdateReceived.TrySetResult(args);
            };

            var getTwinResponse = await twinClient.GetTwinAsync(cancellationToken: cts.Token);
            Assert.NotNull(getTwinResponse.Desired);
            Assert.NotNull(getTwinResponse.Reported);
            Assert.Empty(getTwinResponse.Desired);
            Assert.Empty(getTwinResponse.Reported);

            string expectedDesiredPropertyKey = Guid.NewGuid().ToString();
            string expectedDesiredPropertyValue = Guid.NewGuid().ToString();

            var twin = await registryManager.GetTwinAsync(deviceId, cts.Token);

            twin.Properties.Desired[expectedDesiredPropertyKey] = expectedDesiredPropertyValue;
            await registryManager.UpdateTwinAsync(deviceId, twin, twin.ETag, cts.Token);

            DesiredPatchReceivedEventArgs receivedDesiredPropertyUpdate = await onDesiredPropertiesUpdateReceived.Task.WaitAsync(cts.Token);
            Assert.True(receivedDesiredPropertyUpdate.DesiredProperties.ContainsKey(expectedDesiredPropertyKey));
            Assert.Equal(expectedDesiredPropertyValue, (string) receivedDesiredPropertyUpdate.DesiredProperties[expectedDesiredPropertyKey]!);

            // Get the twin again from the device side, this time looking for the updated desired property
            getTwinResponse = await twinClient.GetTwinAsync(cancellationToken: cts.Token);
            Assert.True(getTwinResponse.Desired!.ContainsKey(expectedDesiredPropertyKey));
            Assert.Equal(expectedDesiredPropertyValue, (string) getTwinResponse.Desired[expectedDesiredPropertyKey]!);

            string expectedReportedPropertyKey = Guid.NewGuid().ToString();
            string expectedReportedPropertyValue = Guid.NewGuid().ToString();

            getTwinResponse.Reported![expectedReportedPropertyKey] = expectedReportedPropertyValue;
            var reportedProperties = new JsonObject();

            reportedProperties[expectedReportedPropertyKey] = expectedReportedPropertyValue;

            var updateReportedPropertiesResponse = await twinClient.UpdateReportedPropertiesAsync(reportedProperties, cts.Token);
            Assert.Equal(Result.Ok, updateReportedPropertiesResponse.Result);
            Assert.Equal((ulong) 2, updateReportedPropertiesResponse.Version);

            // Check that only one desired property patch was received during this test
            Assert.Equal(1, desiredPatchesReceived);

            // Check from the service side that the reported properties were received
            twin = await registryManager.GetTwinAsync(deviceId, TestContext.Current.CancellationToken);
            Assert.True(twin.Properties.Reported.Contains(expectedReportedPropertyKey));
            Assert.Equal(expectedReportedPropertyValue, (string) twin.Properties.Reported[expectedReportedPropertyKey]);
        }
    }
}
