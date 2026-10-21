// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.MQTTv5.Connection;
using Microsoft.Azure.Iot.Device.MQTTv5.Twin;
using Microsoft.Azure.Iot.Device.IntegrationTests.MQTTv5;
using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Models.Twin;
using System.Text.Json.Nodes;
using Xunit;
using Microsoft.Azure.Devices;

namespace Microsoft.Azure.Iot.Device.IntegrationTests.MQTTv5
{
    public class TwinClientIntegrationTests
    {
        [Fact(Timeout = Setup.TestTimeoutMilliseconds)]
        public async Task TestTwin()
        {
            DeviceTwin initialTwin = new()
            {
                Desired = new JsonObject(),
                DesiredVersion = 5,
            };

            string expectedInitialDesiredPropertyKey = Guid.NewGuid().ToString();
            string expectedInitialDesiredPropertyValue = Guid.NewGuid().ToString();
            initialTwin.Desired[expectedInitialDesiredPropertyKey] = expectedInitialDesiredPropertyValue;

            // Want to defer connecting until TwinClient is set up to consume TwinPush
            MQTTv5DeviceTestContext testDeviceContext = await Setup.CreateProvisionableMQTTv5DeviceAsync(initialTwin, null, TestContext.Current.CancellationToken);

            string deviceId = testDeviceContext.DeviceId;

            RegistryManager registryManager = Setup.GetMQTTv5IotHubRegistryManager();

            ProvisioningSettings provisioningSettings = new(Setup.DpsIdScope);

            TwinClient twinClient = new(testDeviceContext.ConnectionClient);
            TaskCompletionSource<TwinPushReceivedEventArgs> twinPushTcs = new();
            twinClient.TwinPushReceived += (args) =>
            {
                twinPushTcs.TrySetResult(args);
            };

            TaskCompletionSource<DesiredPatchReceivedEventArgs> onDesiredPropertiesUpdateReceived = new();
            int desiredPatchesReceived = 0;
            twinClient.DesiredPatchReceived += (args) =>
            {
                desiredPatchesReceived++;
                onDesiredPropertiesUpdateReceived.TrySetResult(args);
            };

            ConnectionContext connectionContext = await Setup.RetryAroundAuthorizationAsync<ConnectionContext>(
                async () => await testDeviceContext.ConnectionClient.ProvisionAndConnectAsync(provisioningSettings, testDeviceContext.AuthenticationProvider, cancellationToken: TestContext.Current.CancellationToken),
                TestContext.Current.CancellationToken);

            try
            {
                var twinPushArgs = await twinPushTcs.Task.WaitAsync(TestContext.Current.CancellationToken);
                Assert.NotNull(twinPushArgs.Desired);
                Assert.True(twinPushArgs.Desired.Properties.ContainsKey(expectedInitialDesiredPropertyKey));
                Assert.Equal(expectedInitialDesiredPropertyValue, twinPushArgs.Desired.Properties[expectedInitialDesiredPropertyKey]!.GetValue<string>());
            }
            catch (TimeoutException)
            {
                Assert.Fail("Timed out waiting for twin push to be received");
            }

            var getTwinResponse = await twinClient.GetTwinAsync(cancellationToken: TestContext.Current.CancellationToken);
            Assert.NotNull(getTwinResponse.Desired);
            Assert.NotNull(getTwinResponse.Reported);
            Assert.Equal(expectedInitialDesiredPropertyValue, getTwinResponse.Desired[expectedInitialDesiredPropertyKey]!.GetValue<string>());
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
            ReportedPatchRequest reportedPatchRequest = new()
            {
                ReportedProperties = new JsonObject(),
                IfMatch = 0
            };

            reportedPatchRequest.ReportedProperties[expectedReportedPropertyKey] = expectedReportedPropertyValue;

            var updateReportedPropertiesResponse = await twinClient.UpdateReportedPropertiesAsync(reportedPatchRequest, TestContext.Current.CancellationToken);
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

        [Fact(Timeout = Setup.TestTimeoutMilliseconds)]
        public async Task TestIfNotMatchFilteringWithTwinPushVersions()
        {
            CancellationToken cancellationToken = TestContext.Current.CancellationToken;
            MQTTv5DeviceTestContext testDeviceContext = await Setup.CreateProvisionableMQTTv5DeviceAsync(null, cancellationToken: cancellationToken);
            using TwinClient twinClient = new(testDeviceContext.ConnectionClient);
            await using (testDeviceContext)
            {
                TaskCompletionSource<TwinPushReceivedEventArgs> twinPushTcs = new(TaskCreationOptions.RunContinuationsAsynchronously);
                twinClient.TwinPushReceived += args => twinPushTcs.TrySetResult(args);

                await Setup.RetryAroundAuthorizationAsync<ConnectionContext>(
                    () => testDeviceContext.ConnectionClient.ProvisionAndConnectAsync(
                        new(Setup.DpsIdScope), testDeviceContext.AuthenticationProvider, cancellationToken: cancellationToken),
                    cancellationToken);

                TwinPushReceivedEventArgs push = await twinPushTcs.Task.WaitAsync(cancellationToken);
                Assert.NotNull(push.Desired);
                Assert.NotNull(push.Reported);
                Assert.True(push.Desired.PropertiesVersion > 0);
                Assert.True(push.Reported.PropertiesVersion > 0);

                DeviceTwin unchanged = await twinClient.GetTwinAsync(
                    ifNotMatchDesired: push.Desired.PropertiesVersion,
                    ifNotMatchReported: push.Reported.PropertiesVersion,
                    cancellationToken: cancellationToken);

                Assert.Null(unchanged.Desired);
                Assert.Null(unchanged.Reported);
                Assert.Equal(push.Desired.PropertiesVersion, unchanged.DesiredVersion);
                Assert.Equal(push.Reported.PropertiesVersion, unchanged.ReportedVersion);

                DeviceTwin newer = await twinClient.GetTwinAsync(
                    ifNotMatchDesired: push.Desired.PropertiesVersion - 1,
                    ifNotMatchReported: push.Reported.PropertiesVersion - 1,
                    cancellationToken: cancellationToken);

                Assert.NotNull(newer.Desired);
                Assert.NotNull(newer.Reported);
                Assert.Equal(push.Desired.PropertiesVersion, newer.DesiredVersion);
                Assert.Equal(push.Reported.PropertiesVersion, newer.ReportedVersion);
            }
        }

        [Fact(Timeout = Setup.TestTimeoutMilliseconds)]
        public async Task TestIfMatchFilteringReportedPatchRequest()
        {
            CancellationToken cancellationToken = TestContext.Current.CancellationToken;
            MQTTv5DeviceTestContext testDeviceContext = await Setup.CreateConnectedMQTTv5ConnectionClientAsync(null, cancellationToken: cancellationToken);
            using TwinClient twinClient = new(testDeviceContext.ConnectionClient);
            await using (testDeviceContext)
            {
                string propertyName = Guid.NewGuid().ToString();
                var first = await twinClient.UpdateReportedPropertiesAsync(
                    new() { ReportedProperties = new JsonObject { [propertyName] = "first" } }, cancellationToken);
                Assert.Equal(Result.Ok, first.Result);

                var current = await twinClient.UpdateReportedPropertiesAsync(
                    new()
                    {
                        ReportedProperties = new JsonObject { [propertyName] = "current" },
                        IfMatch = first.Version,
                    }, cancellationToken);
                Assert.Equal(Result.Ok, current.Result);
                Assert.True(current.Version > first.Version);

                var stale = await twinClient.UpdateReportedPropertiesAsync(
                    new()
                    {
                        ReportedProperties = new JsonObject { [propertyName] = "stale" },
                        IfMatch = first.Version,
                    }, cancellationToken);
                Assert.Equal(Result.VersionMismatch, stale.Result);
                Assert.Equal(current.Version, stale.Version);

                DeviceTwin unchanged = await twinClient.GetTwinAsync(cancellationToken: cancellationToken);
                Assert.NotNull(unchanged.Reported);
                Assert.Equal("current", unchanged.Reported[propertyName]!.GetValue<string>());
                Assert.Equal(current.Version, unchanged.ReportedVersion);

                var next = await twinClient.UpdateReportedPropertiesAsync(
                    new() { ReportedProperties = new JsonObject { [propertyName] = "next" } }, cancellationToken);
                Assert.Equal(Result.Ok, next.Result);
                Assert.True(next.Version > current.Version);
            }
        }
    }
}
