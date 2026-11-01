// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.MQTTv5.Connection;
using Microsoft.Azure.Iot.Device.MQTTv5.Twin;
using Microsoft.Azure.Iot.Device.IntegrationTests.MQTTv5;
using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Models.Twin;
using System.Text.Json.Nodes;
using System.Threading.Channels;
using Xunit;
using Microsoft.Azure.Devices;
using ServiceTwin = Microsoft.Azure.Devices.Shared.Twin;

namespace Microsoft.Azure.Iot.Device.IntegrationTests.MQTTv5
{
    [Trait(Setup.HubTrait, Setup.MQTTv5HubTraitValue)]
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

            ProvisioningSettings provisioningSettings = Setup.CreateProvisioningSettings(testAgainstClassicHub: false);

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
            Assert.True(getTwinResponse.Desired.ContainsKey(expectedInitialDesiredPropertyKey));
            Assert.Equal(expectedInitialDesiredPropertyValue, (string)getTwinResponse.Desired[expectedInitialDesiredPropertyKey]!);
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
                        Setup.CreateProvisioningSettings(testAgainstClassicHub: false), testDeviceContext.AuthenticationProvider, cancellationToken: cancellationToken),
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
        public async Task TestReportedPatchesAreConditionalOnTrackedVersionAndCombined()
        {
            CancellationToken cancellationToken = TestContext.Current.CancellationToken;
            MQTTv5DeviceTestContext testDeviceContext = await Setup.CreateConnectedMQTTv5ConnectionClientAsync(null, cancellationToken: cancellationToken);
            using TwinClient twinClient = new(testDeviceContext.ConnectionClient);
            await using (testDeviceContext)
            {
                // Each patch is made conditional on the version from the birth ack or the previous response, so sequential
                // patches are all accepted.
                string propertyName = Guid.NewGuid().ToString();
                var first = await twinClient.UpdateReportedPropertiesAsync(
                    new() { ReportedProperties = new JsonObject { [propertyName] = "first" } }, cancellationToken);
                Assert.Equal(Result.Ok, first.Result);
                Assert.Equal(first.Version, twinClient.AuthoritativeReportedPropertiesVersion);

                var second = await twinClient.UpdateReportedPropertiesAsync(
                    new() { ReportedProperties = new JsonObject { [propertyName] = "second" } }, cancellationToken);
                Assert.Equal(Result.Ok, second.Result);
                Assert.Equal(first.Version + 1, second.Version);

                // Patches made while one is in flight are combined into the next patch, in order.
                string otherPropertyName = Guid.NewGuid().ToString();
                ReportedPatchResponse[] concurrent = await Task.WhenAll(
                    twinClient.UpdateReportedPropertiesAsync(new() { ReportedProperties = new JsonObject { [propertyName] = "third" } }, cancellationToken),
                    twinClient.UpdateReportedPropertiesAsync(new() { ReportedProperties = new JsonObject { [propertyName] = "fourth" } }, cancellationToken),
                    twinClient.UpdateReportedPropertiesAsync(new() { ReportedProperties = new JsonObject { [otherPropertyName] = "other" } }, cancellationToken));
                Assert.All(concurrent, response => Assert.Equal(Result.Ok, response.Result));
                ulong latestVersion = concurrent.Max(response => response.Version);
                Assert.True(latestVersion <= second.Version + 2);
                Assert.Equal(latestVersion, twinClient.AuthoritativeReportedPropertiesVersion);

                DeviceTwin twin = await twinClient.GetTwinAsync(cancellationToken: cancellationToken);
                Assert.NotNull(twin.Reported);
                Assert.Equal("fourth", twin.Reported[propertyName]!.GetValue<string>());
                Assert.Equal("other", twin.Reported[otherPropertyName]!.GetValue<string>());
                Assert.Equal(latestVersion, twin.ReportedVersion);
            }
        }
        [Fact(Timeout = Setup.TestTimeoutMilliseconds)]
        public async Task TestDesiredPatchesAfterTwinPushAreDeliveredInVersionOrder()
        {
            CancellationToken cancellationToken = TestContext.Current.CancellationToken;
            using RegistryManager registryManager = Setup.GetMQTTv5IotHubRegistryManager();
            MQTTv5DeviceTestContext testDeviceContext = await Setup.CreateProvisionableMQTTv5DeviceAsync(null, cancellationToken: cancellationToken);
            using TwinClient twinClient = new(testDeviceContext.ConnectionClient);
            await using (testDeviceContext)
            {
                TwinEventRecorder events = new(twinClient);
                await ConnectAsync(testDeviceContext, cancellationToken);
                ulong pushedVersion = (await events.NextAsync<TwinPushReceivedEventArgs>(cancellationToken)).Desired!.PropertiesVersion;

                List<(string Key, string Value)> updates = new();
                for (int i = 0; i < 3; i++)
                {
                    updates.Add(await UpdateDesiredPropertyAsync(registryManager, testDeviceContext.DeviceId, cancellationToken));
                }

                // Each patch is exactly one version newer than the last, so no resync is needed.
                for (int i = 0; i < updates.Count; i++)
                {
                    DesiredPatchReceivedEventArgs patch = await events.NextAsync<DesiredPatchReceivedEventArgs>(cancellationToken);
                    Assert.Equal(pushedVersion + (ulong)i + 1, patch.DesiredPropertiesVersion);
                    Assert.Equal(updates[i].Value, patch.DesiredProperties[updates[i].Key]!.GetValue<string>());
                }
            }
        }

        [Fact(Timeout = Setup.TestTimeoutMilliseconds)]
        public async Task TestDesiredPatchGapResyncsDesiredProperties()
        {
            CancellationToken cancellationToken = TestContext.Current.CancellationToken;
            using RegistryManager registryManager = Setup.GetMQTTv5IotHubRegistryManager();
            MQTTv5DeviceTestContext testDeviceContext = await Setup.CreateProvisionableMQTTv5DeviceAsync(null, cancellationToken: cancellationToken);
            using TwinClient twinClient = new(testDeviceContext.ConnectionClient);
            await using (testDeviceContext)
            {
                TwinEventRecorder events = new(twinClient);
                await ConnectAsync(testDeviceContext, cancellationToken);
                ulong pushedVersion = (await events.NextAsync<TwinPushReceivedEventArgs>(cancellationToken)).Desired!.PropertiesVersion;

                // This twin client missed the twin push, so it has not observed any desired properties version yet. The next
                // patch therefore skips versions from its point of view and makes it resync the desired properties.
                TwinClient lateTwinClient = new(testDeviceContext.ConnectionClient);
                try
                {
                    TwinEventRecorder lateEvents = new(lateTwinClient);

                    (string firstKey, string firstValue) = await UpdateDesiredPropertyAsync(registryManager, testDeviceContext.DeviceId, cancellationToken);

                    DesiredPatchReceivedEventArgs patch = await events.NextAsync<DesiredPatchReceivedEventArgs>(cancellationToken);
                    Assert.Equal(pushedVersion + 1, patch.DesiredPropertiesVersion);

                    TwinPushReceivedEventArgs snapshot = await lateEvents.NextAsync<TwinPushReceivedEventArgs>(cancellationToken);
                    Assert.Null(snapshot.Reported);
                    Assert.NotNull(snapshot.Desired);
                    Assert.Equal(pushedVersion + 1, snapshot.Desired.PropertiesVersion);
                    Assert.Equal(firstValue, snapshot.Desired.Properties[firstKey]!.GetValue<string>());

                    // Now that it has resynced, the late twin client receives the next patch as normal.
                    (string secondKey, string secondValue) = await UpdateDesiredPropertyAsync(registryManager, testDeviceContext.DeviceId, cancellationToken);

                    DesiredPatchReceivedEventArgs latePatch = await lateEvents.NextAsync<DesiredPatchReceivedEventArgs>(cancellationToken);
                    Assert.Equal(pushedVersion + 2, latePatch.DesiredPropertiesVersion);
                    Assert.Equal(secondValue, latePatch.DesiredProperties[secondKey]!.GetValue<string>());
                }
                finally
                {
                    // Don't dispose the shared connection client along with this twin client.
                    lateTwinClient.Dispose(false);
                }
            }
        }

        [Fact(Timeout = 2 * Setup.TestTimeoutMilliseconds)]
        public async Task TestReconnectAfterMissingDesiredUpdateDeliversNewerSnapshot()
        {
            CancellationToken cancellationToken = TestContext.Current.CancellationToken;
            using RegistryManager registryManager = Setup.GetMQTTv5IotHubRegistryManager();
            MQTTv5DeviceTestContext testDeviceContext = await Setup.CreateProvisionableMQTTv5DeviceAsync(null, cancellationToken: cancellationToken);
            using TwinClient twinClient = new(testDeviceContext.ConnectionClient);
            await using (testDeviceContext)
            {
                TwinEventRecorder events = new(twinClient);
                await ConnectAsync(testDeviceContext, cancellationToken);
                ulong pushedVersion = (await events.NextAsync<TwinPushReceivedEventArgs>(cancellationToken)).Desired!.PropertiesVersion;

                await testDeviceContext.ConnectionClient.DisconnectAsync(null, cancellationToken);
                (string missedKey, string missedValue) = await UpdateDesiredPropertyAsync(registryManager, testDeviceContext.DeviceId, cancellationToken);
                await ConnectAsync(testDeviceContext, cancellationToken);

                TwinPushReceivedEventArgs push = await events.NextAsync<TwinPushReceivedEventArgs>(cancellationToken);
                Assert.NotNull(push.Desired);
                Assert.Equal(pushedVersion + 1, push.Desired.PropertiesVersion);
                Assert.Equal(missedValue, push.Desired.Properties[missedKey]!.GetValue<string>());

                (string key, string value) = await UpdateDesiredPropertyAsync(registryManager, testDeviceContext.DeviceId, cancellationToken);

                DesiredPatchReceivedEventArgs patch = await events.NextAsync<DesiredPatchReceivedEventArgs>(cancellationToken);
                Assert.Equal(pushedVersion + 2, patch.DesiredPropertiesVersion);
                Assert.Equal(value, patch.DesiredProperties[key]!.GetValue<string>());
            }
        }

        [Fact(Timeout = 2 * Setup.TestTimeoutMilliseconds)]
        public async Task TestReconnectWithoutTwinUpdatesSkipsTwinPush()
        {
            CancellationToken cancellationToken = TestContext.Current.CancellationToken;
            using RegistryManager registryManager = Setup.GetMQTTv5IotHubRegistryManager();
            MQTTv5DeviceTestContext testDeviceContext = await Setup.CreateProvisionableMQTTv5DeviceAsync(null, cancellationToken: cancellationToken);
            using TwinClient twinClient = new(testDeviceContext.ConnectionClient);
            await using (testDeviceContext)
            {
                TwinEventRecorder events = new(twinClient);
                await ConnectAsync(testDeviceContext, cancellationToken);
                TwinPushReceivedEventArgs firstPush = await events.NextAsync<TwinPushReceivedEventArgs>(cancellationToken);
                ulong pushedVersion = firstPush.Desired!.PropertiesVersion;

                await testDeviceContext.ConnectionClient.DisconnectAsync(null, cancellationToken);
                await ConnectAsync(testDeviceContext, cancellationToken);

                // The birth tells IoT hub which versions this device holds, and the birth ack confirms that IoT hub has the same ones.
                Assert.Equal(pushedVersion, await WaitForAuthoritativeDesiredVersionAsync(twinClient, cancellationToken));
                Assert.Equal(firstPush.Reported!.PropertiesVersion, twinClient.AuthoritativeReportedPropertiesVersion);

                // Since neither section changed, IoT hub doesn't push the twin again, so the next event is the next patch.
                (string key, string value) = await UpdateDesiredPropertyAsync(registryManager, testDeviceContext.DeviceId, cancellationToken);

                DesiredPatchReceivedEventArgs patch = await events.NextAsync<DesiredPatchReceivedEventArgs>(cancellationToken);
                Assert.Equal(pushedVersion + 1, patch.DesiredPropertiesVersion);
                Assert.Equal(value, patch.DesiredProperties[key]!.GetValue<string>());
            }
        }

        [Fact(Timeout = Setup.TestTimeoutMilliseconds)]
        public async Task TestRestoredDesiredVersionNewerThanHubResyncsDesiredProperties()
        {
            CancellationToken cancellationToken = TestContext.Current.CancellationToken;
            using RegistryManager registryManager = Setup.GetMQTTv5IotHubRegistryManager();
            MQTTv5DeviceTestContext testDeviceContext = await Setup.CreateProvisionableMQTTv5DeviceAsync(
                null,
                new Device.MQTTv5.Models.ConnectionClientOptions(),
                cancellationToken);
            using TwinClient twinClient = new(testDeviceContext.ConnectionClient, new TwinState() { DesiredVersion = 1000 });
            await using (testDeviceContext)
            {
                TwinEventRecorder events = new(twinClient);
                await ConnectAsync(testDeviceContext, cancellationToken);

                // IoT hub treats the restored version as current, so its first push carries no desired section. Once the birth
                // ack reveals that this version is not valid, the client reconnects to announce a valid one, and gets the
                // desired properties from IoT hub's push or the GET it sends to resync them.
                TwinPushSection? desired;
                do
                {
                    desired = (await events.NextAsync<TwinPushReceivedEventArgs>(cancellationToken)).Desired;
                }
                while (desired == null);

                Assert.Equal(twinClient.AuthoritativeDesiredPropertiesVersion, desired.PropertiesVersion);

                (string key, string value) = await UpdateDesiredPropertyAsync(registryManager, testDeviceContext.DeviceId, cancellationToken);

                DesiredPatchReceivedEventArgs patch = await events.NextAsync<DesiredPatchReceivedEventArgs>(cancellationToken);
                Assert.Equal(desired.PropertiesVersion + 1, patch.DesiredPropertiesVersion);
                Assert.Equal(value, patch.DesiredProperties[key]!.GetValue<string>());
            }
        }

        private static async Task<ulong> WaitForAuthoritativeDesiredVersionAsync(TwinClient twinClient, CancellationToken cancellationToken)
        {
            // The twin client handles the birth ack independently of the connection client, so it may do so slightly later.
            ulong? version;
            while ((version = twinClient.AuthoritativeDesiredPropertiesVersion) == null)
            {
                await Task.Delay(TimeSpan.FromMilliseconds(100), cancellationToken);
            }

            return version.Value;
        }

        private static Task<ConnectionContext> ConnectAsync(MQTTv5DeviceTestContext testDeviceContext, CancellationToken cancellationToken)
        {
            return Setup.RetryAroundAuthorizationAsync<ConnectionContext>(
                () => testDeviceContext.ConnectionClient.ProvisionAndConnectAsync(
                    Setup.CreateProvisioningSettings(testAgainstClassicHub: false), testDeviceContext.AuthenticationProvider, cancellationToken: cancellationToken),
                cancellationToken);
        }

        // Each update increments the desired properties version by one.
        private static async Task<(string Key, string Value)> UpdateDesiredPropertyAsync(RegistryManager registryManager, string deviceId, CancellationToken cancellationToken)
        {
            string key = Guid.NewGuid().ToString();
            string value = Guid.NewGuid().ToString();
            ServiceTwin patch = new();
            patch.Properties.Desired[key] = value;
            await registryManager.UpdateTwinAsync(deviceId, patch, "*", cancellationToken);
            return (key, value);
        }

        // Records the desired patch and twin push events a twin client raises, in the order it raises them.
        private sealed class TwinEventRecorder
        {
            private readonly Channel<EventArgs> _events = Channel.CreateUnbounded<EventArgs>();

            public TwinEventRecorder(TwinClient twinClient)
            {
                twinClient.DesiredPatchReceived += args => _events.Writer.TryWrite(args);
                twinClient.TwinPushReceived += args => _events.Writer.TryWrite(args);
            }

            public async Task<T> NextAsync<T>(CancellationToken cancellationToken) where T : EventArgs
            {
                return Assert.IsType<T>(await _events.Reader.ReadAsync(cancellationToken));
            }
        }
    }
}
