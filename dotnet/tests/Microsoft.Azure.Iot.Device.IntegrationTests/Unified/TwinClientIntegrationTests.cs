// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Models.Twin;
using Microsoft.Azure.Iot.Device.Unified.Twin;
using System.Text.Json.Nodes;
using Xunit;
using Microsoft.Azure.Devices;

namespace Microsoft.Azure.Iot.Device.IntegrationTests.Unified
{
    public class TwinClientIntegrationTests
    {
        [Theory(Timeout = Setup.TestTimeoutMilliseconds)]
        [InlineData(true)]
        [InlineData(false)]
        public async Task TestTwin(bool testAgainstClassicHub)
        {
            UnifiedDeviceTestContext testDeviceContext = await Setup.CreateConnectedUnifiedConnectionClientAsync(testAgainstClassicHub, null, TestContext.Current.CancellationToken);
            string deviceId = testDeviceContext.ConnectionContext.DeviceId;

            RegistryManager registryManager = testAgainstClassicHub
                ? Setup.GetMQTTv3IotHubRegistryManager()
                : Setup.GetMQTTv5IotHubRegistryManager();
            using TwinClient twinClient = new TwinClient(testDeviceContext.ConnectionClient);
            TaskCompletionSource<DesiredPatchReceivedEventArgs> onDesiredPropertiesUpdateReceived = new();
            int desiredPatchesReceived = 0;
            twinClient.DesiredPatchReceived += (args) =>
            {
                desiredPatchesReceived++;
                onDesiredPropertiesUpdateReceived.TrySetResult(args);
            };

            var getTwinResponse = await twinClient.GetTwinAsync(cancellationToken: TestContext.Current.CancellationToken);
            Assert.NotNull(getTwinResponse.Desired);
            Assert.NotNull(getTwinResponse.Reported);
            Assert.Empty(getTwinResponse.Desired);
            Assert.Empty(getTwinResponse.Reported);

            string expectedDesiredPropertyKey = Guid.NewGuid().ToString();
            string expectedDesiredPropertyValue = Guid.NewGuid().ToString();

            var twin = await registryManager.GetTwinAsync(deviceId, TestContext.Current.CancellationToken);

            twin.Properties.Desired[expectedDesiredPropertyKey] = expectedDesiredPropertyValue;
            await registryManager.UpdateTwinAsync(deviceId, twin, twin.ETag, TestContext.Current.CancellationToken);

            DesiredPatchReceivedEventArgs receivedDesiredPropertyUpdate = await onDesiredPropertiesUpdateReceived.Task.WaitAsync(TestContext.Current.CancellationToken);
            Assert.True(receivedDesiredPropertyUpdate.DesiredProperties.ContainsKey(expectedDesiredPropertyKey));
            Assert.Equal(expectedDesiredPropertyValue, (string)receivedDesiredPropertyUpdate.DesiredProperties[expectedDesiredPropertyKey]!);

            // Get the twin again from the device side, this time looking for the updated desired property
            getTwinResponse = await twinClient.GetTwinAsync(cancellationToken: TestContext.Current.CancellationToken);
            Assert.True(getTwinResponse.Desired!.ContainsKey(expectedDesiredPropertyKey));
            Assert.Equal(expectedDesiredPropertyValue, (string)getTwinResponse.Desired[expectedDesiredPropertyKey]!);

            string expectedReportedPropertyKey = Guid.NewGuid().ToString();
            string expectedReportedPropertyValue = Guid.NewGuid().ToString();

            getTwinResponse.Reported![expectedReportedPropertyKey] = expectedReportedPropertyValue;
            var reportedProperties = new JsonObject();

            reportedProperties[expectedReportedPropertyKey] = expectedReportedPropertyValue;

            var updateReportedPropertiesResponse = await twinClient.UpdateReportedPropertiesAsync(reportedProperties, TestContext.Current.CancellationToken);
            Assert.Equal(Result.Ok, updateReportedPropertiesResponse.Result);
            Assert.Equal((ulong)2, updateReportedPropertiesResponse.Version);

            // Check that only one desired property patch was received during this test
            Assert.Equal(1, desiredPatchesReceived);

            // Check from the service side that the reported properties were received
            twin = await registryManager.GetTwinAsync(deviceId, TestContext.Current.CancellationToken);
            Assert.True(twin.Properties.Reported.Contains(expectedReportedPropertyKey));
            Assert.Equal(expectedReportedPropertyValue, (string)twin.Properties.Reported[expectedReportedPropertyKey]);

            await testDeviceContext.DisposeAsync(); // Dispose this before any feature clients so that the test device identity can be cleaned up and the MQTT client disconnected gracefully
        }
    }
}
