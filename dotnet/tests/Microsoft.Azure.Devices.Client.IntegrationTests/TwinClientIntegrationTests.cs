using Microsoft.Azure.Devices.Client.Twin;
using Microsoft.Azure.Devices.Client.Twin.LegacyTwinObjects;
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
            ConnectionClient connectionClient = testDeviceContext.ConnectionClient;
            string deviceId = testDeviceContext.ConnectionContext.DeviceId;

            RegistryManager registryManager = Setup.GetIotHubRegistryManager();
            TwinClient twinClient = new TwinClient(connectionClient);

            TaskCompletionSource<DesiredPropertyUpdateReceivedEventArgs> onDesiredPropertiesUpdateReceived = new();
            twinClient.DesiredPropertyUpdateReceived += (args) =>
            {
                onDesiredPropertiesUpdateReceived.TrySetResult(args);
            };

            var getTwinResponse = await twinClient.GetTwinAsync(cancellationToken: cts.Token);
            Assert.Empty(getTwinResponse.DesiredProperties);
            Assert.Empty(getTwinResponse.ReportedProperties);
            Assert.NotNull(getTwinResponse.DesiredPropertiesVersion);
            Assert.NotNull(getTwinResponse.ReportedPropertiesVersion);

            string expectedDesiredPropertyKey = Guid.NewGuid().ToString();
            string expectedDesiredPropertyValue = Guid.NewGuid().ToString();

            var twin = await registryManager.GetTwinAsync(deviceId, cts.Token);

            twin.Properties.Desired[expectedDesiredPropertyKey] = expectedDesiredPropertyValue;
            await registryManager.UpdateTwinAsync(deviceId, twin, twin.ETag, cts.Token);

            DesiredPropertyUpdateReceivedEventArgs receivedDesiredPropertyUpdate = await onDesiredPropertiesUpdateReceived.Task.WaitAsync(cts.Token);
            Assert.True(receivedDesiredPropertyUpdate.DesiredProperties.ContainsKey(expectedDesiredPropertyKey));
            Assert.Equal(expectedDesiredPropertyValue, (string) receivedDesiredPropertyUpdate.DesiredProperties[expectedDesiredPropertyKey]);

            // Get the twin again from the device side, this time looking for the updated desired property
            getTwinResponse = await twinClient.GetTwinAsync(cancellationToken: cts.Token);
            Assert.True(getTwinResponse.DesiredProperties.ContainsKey(expectedDesiredPropertyKey));
            Assert.Equal(expectedDesiredPropertyValue, (string) getTwinResponse.DesiredProperties[expectedDesiredPropertyKey]);

            string expectedReportedPropertyKey = Guid.NewGuid().ToString();
            string expectedReportedPropertyValue = Guid.NewGuid().ToString();

            getTwinResponse.ReportedProperties[expectedReportedPropertyKey] = expectedReportedPropertyValue;

            ReportedPatchRequest reportedPatch = new()
            {
                ReportedProperties = getTwinResponse.ReportedProperties,
            };

            reportedPatch.ReportedProperties[expectedReportedPropertyKey] = expectedReportedPropertyValue;

            var updateReportedPropertiesResponse = await twinClient.UpdateReportedPropertiesAsync(reportedPatch, cts.Token);
            Assert.Equal(Result.Ok, updateReportedPropertiesResponse.Result);

            /*
            twin = await registryManager.GetTwinAsync(deviceId);
            Assert.True(twin.Properties.Reported.Contains(expectedReportedPropertyKey));
            Assert.Equal(expectedReportedPropertyValue, twin.Properties.Reported[expectedReportedPropertyKey]);
            */ // TODO even worth checking?
        }
    }
}
