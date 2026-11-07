// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Models.Twin;
using Microsoft.Azure.Iot.Device.Unified.Twin;
using Microsoft.Azure.Iot.Device.Unified.Models;
using System.Text.Json.Nodes;
using Xunit;
using Microsoft.Azure.Devices;
using EnrollmentTwinState = Microsoft.Azure.Devices.Provisioning.Service.TwinState;

namespace Microsoft.Azure.Iot.Device.IntegrationTests.Unified
{
    public class TwinClientIntegrationTests
    {
        [Fact(Timeout = Setup.TestTimeoutMilliseconds)]
        public async Task TestTwinStartsWithTheInitialTwinFromTheDpsEnrollment()
        {
            string expectedDesiredPropertyKey = "initial" + Guid.NewGuid().ToString("N");
            string expectedDesiredPropertyValue = Guid.NewGuid().ToString();
            Microsoft.Azure.Devices.Shared.TwinCollection initialDesiredProperties = new();
            initialDesiredProperties[expectedDesiredPropertyKey] = expectedDesiredPropertyValue;

            TwinClient? createdTwinClient = null;
            TaskCompletionSource<TwinPushReceivedEventArgs> twinPushReceived = new(TaskCreationOptions.RunContinuationsAsynchronously);
            UnifiedDeviceTestContext testDeviceContext = await Setup.CreateConnectedUnifiedConnectionClientWithSymmetricKeyAsync(
                cancellationToken: TestContext.Current.CancellationToken,
                initialTwinState: new EnrollmentTwinState(null, initialDesiredProperties),
                beforeConnect: connectionClient =>
                {
                    // Constructed before provisioning, as recommended, so that the twin push raised upon connecting is not missed.
                    createdTwinClient = new TwinClient(connectionClient);
                    createdTwinClient.TwinPushReceived += args =>
                    {
                        if (args.Desired != null)
                        {
                            twinPushReceived.TrySetResult(args);
                        }
                    };
                });

            // Declared in this order so that the test device context is disposed before the twin client.
            using TwinClient twinClient = createdTwinClient!;
            await using (testDeviceContext)
            {
                // The twin client adopts the initial twin without the application asking for it.
                TwinPushReceivedEventArgs push = await twinPushReceived.Task.WaitAsync(TestContext.Current.CancellationToken);
                Assert.Equal(expectedDesiredPropertyValue, (string)push.Desired!.Properties[expectedDesiredPropertyKey]!);

                TwinState twinState = twinClient.GetTwinState();
                Assert.Equal(push.Desired.PropertiesVersion, twinState.DesiredVersion);
                Assert.Equal(expectedDesiredPropertyValue, (string)twinState.Desired[expectedDesiredPropertyKey]!);
            }
        }

        [Theory(Timeout = Setup.TestTimeoutMilliseconds)]
        [InlineData(true)]
        [InlineData(false, Traits = new[] { Setup.HubTrait, Setup.MQTTv5HubTraitValue })]
        public async Task TestTwin(bool testAgainstClassicHub)
        {
            // MQTTv5 IoT hubs only send desired properties patches if asked to. MQTTv3 IoT hubs always send them.
            ConnectionClientOptions options = new() { ReceiveDesiredPropertyUpdates = true };
            UnifiedDeviceTestContext testDeviceContext = await Setup.CreateConnectedUnifiedConnectionClientAsync(testAgainstClassicHub, options, TestContext.Current.CancellationToken);
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

            // Queue two patches without awaiting them. The SDK may combine them into one send, so wait until IoT hub applied both.
            string firstEnqueuedKey = Guid.NewGuid().ToString();
            string secondEnqueuedKey = Guid.NewGuid().ToString();
            HashSet<string> appliedEnqueuedKeys = new();
            TaskCompletionSource enqueuedPatchesApplied = new(TaskCreationOptions.RunContinuationsAsynchronously);
            twinClient.ReportedPatchCompleted += (args) =>
            {
                if (args.Response?.Result != Result.Ok)
                {
                    enqueuedPatchesApplied.TrySetException(new InvalidOperationException(
                        $"IoT hub didn't apply the reported properties patch {args.Patch.ToJsonString()}: {args.Response?.Result.ToString() ?? args.Error?.Message}"));
                    return;
                }

                lock (appliedEnqueuedKeys)
                {
                    appliedEnqueuedKeys.UnionWith(args.Patch.Select(property => property.Key));
                    if (appliedEnqueuedKeys.Contains(firstEnqueuedKey) && appliedEnqueuedKeys.Contains(secondEnqueuedKey))
                    {
                        enqueuedPatchesApplied.TrySetResult();
                    }
                }
            };

            twinClient.EnqueueReportedPropertiesUpdate(new JsonObject() { [firstEnqueuedKey] = "first" });
            twinClient.EnqueueReportedPropertiesUpdate(new JsonObject() { [secondEnqueuedKey] = "second" });
            await enqueuedPatchesApplied.Task.WaitAsync(TestContext.Current.CancellationToken);

            twin = await registryManager.GetTwinAsync(deviceId, TestContext.Current.CancellationToken);
            Assert.Equal("first", (string)twin.Properties.Reported[firstEnqueuedKey]);
            Assert.Equal("second", (string)twin.Properties.Reported[secondEnqueuedKey]);

            // The device's copy of its reported properties includes the applied patches, and none are left pending.
            TwinState twinState = twinClient.GetTwinState();
            Assert.Empty(twinState.PendingReportedPatches);
            Assert.Equal("first", (string)twinState.Reported[firstEnqueuedKey]!);
            Assert.Equal("second", (string)twinState.Reported[secondEnqueuedKey]!);
            Assert.Equal((ulong)twin.Properties.Reported.Version, twinState.ReportedVersion);

            await testDeviceContext.DisposeAsync(); // Dispose this before any feature clients so that the test device identity can be cleaned up and the MQTT client disconnected gracefully
        }
    }
}
