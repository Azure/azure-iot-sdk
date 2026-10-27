// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Devices;
using Microsoft.Azure.Devices.Common.Exceptions;
using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Models.Telemetry;
using Microsoft.Azure.Iot.Device.Models.Twin;
using Microsoft.Azure.Iot.Device.MQTTv5.Connection;
using Microsoft.Azure.Iot.Device.MQTTv5.Telemetry;
using Microsoft.Azure.Iot.Device.MQTTv5.Twin;
using Microsoft.Azure.Iot.Device.Retry;
using System.Diagnostics;
using System.Text;
using System.Text.Json.Nodes;
using Xunit;
using ConnectionClientOptions = Microsoft.Azure.Iot.Device.MQTTv5.Models.ConnectionClientOptions;

namespace Microsoft.Azure.Iot.Device.IntegrationTests.MQTTv5
{
    [Trait(Setup.HubTrait, Setup.MQTTv5HubTraitValue)]
    public class RetryIntegrationTests
    {
        [Fact(Timeout = 2 * Setup.TestTimeoutMilliseconds, Skip = "This device doesn't go to reprovisioning once deleted? Service bug?")]
        public async Task TestReprovisioningAfterDeviceDeleted()
        {
            CancellationToken cancellationToken = TestContext.Current.CancellationToken;
            using RegistryManager registryManager = Setup.GetMQTTv5IotHubRegistryManager();
            MQTTv5DeviceTestContext device = await Setup.CreateProvisionableMQTTv5DeviceAsync(null, CreateOptions(), cancellationToken);
            using TelemetryClient telemetryClient = new(device.ConnectionClient);
            using TwinClient twinClient = new(device.ConnectionClient);
            await using (device)
            {
                ConnectionContext initialContext = await ConnectAsync(device, cancellationToken);
                Assert.True(await DeviceExistsAsync(registryManager, device.DeviceId, cancellationToken));

                await registryManager.RemoveDeviceAsync(device.DeviceId, cancellationToken);
                Assert.False(await DeviceExistsAsync(registryManager, device.DeviceId, cancellationToken));
                await telemetryClient.SendTelemetryAsync(
                    new DeviceToCloudTelemetry { Payload = Encoding.UTF8.GetBytes("{\"awaitingRecovery\":true}") }, cancellationToken);

                await WaitUntilAsync(
                    () => HasReprovisionedAsync(device.ConnectionClient, initialContext, registryManager, device.DeviceId, cancellationToken),
                    TimeSpan.FromSeconds(60),
                    "The MQTTv5 device did not re-provision after its identity was deleted.",
                    cancellationToken);

                await AssertFeatureClientsWorkAsync(telemetryClient, twinClient, cancellationToken);
            }
        }

        [Fact(Timeout = 2 * Setup.TestTimeoutMilliseconds, Skip = "This device doesn't go to reprovisioning once deleted? Service bug?")]
        public async Task TestReprovisioningAfterDeviceDeletedUsingCachedAssignment()
        {
            CancellationToken cancellationToken = TestContext.Current.CancellationToken;
            using RegistryManager registryManager = Setup.GetMQTTv5IotHubRegistryManager();
            ConnectionClientOptions options = CreateOptions();
            await using MQTTv5DeviceTestContext device = await Setup.CreateProvisionableMQTTv5DeviceAsync(null, options, cancellationToken);
            ConnectionContext cachedContext = await ConnectAsync(device, cancellationToken);
            Assert.True(await DeviceExistsAsync(registryManager, device.DeviceId, cancellationToken));

            await device.ConnectionClient.DisconnectAsync(cancellationToken);
            await registryManager.RemoveDeviceAsync(device.DeviceId, cancellationToken);
            Assert.False(await DeviceExistsAsync(registryManager, device.DeviceId, cancellationToken));

            using ConnectionClient reconnectingClient = new(options, connectionContext: cachedContext);
            using TelemetryClient telemetryClient = new(reconnectingClient);
            using TwinClient twinClient = new(reconnectingClient);
            try
            {
                ConnectionContext recoveredContext = await reconnectingClient.ProvisionAndConnectAsync(
                    Setup.CreateProvisioningSettings(testAgainstClassicHub: false), device.AuthenticationProvider, cancellationToken: cancellationToken);

                Assert.NotSame(cachedContext, recoveredContext);
                Assert.Equal(IotHubConnectionType.Mqttv5, recoveredContext.ConnectionProfile);
                Assert.Equal(device.DeviceId, recoveredContext.DeviceId);
                await WaitUntilAsync(
                    () => DeviceExistsAsync(registryManager, device.DeviceId, cancellationToken),
                    TimeSpan.FromSeconds(15),
                    "The MQTTv5 device did not reappear after falling back from its cached assignment to DPS.",
                    cancellationToken);

                await AssertFeatureClientsWorkAsync(telemetryClient, twinClient, cancellationToken);
            }
            finally
            {
                await reconnectingClient.DisconnectAsync(CancellationToken.None);
            }
        }

        [Fact(Timeout = 3 * Setup.TestTimeoutMilliseconds, Skip = "This device doesn't go to reprovisioning once deleted? Service bug?")]
        public async Task TestReprovisioningAfterDeviceDeletedAndEnrollmentTemporarilyDeleted()
        {
            CancellationToken cancellationToken = TestContext.Current.CancellationToken;
            using RegistryManager registryManager = Setup.GetMQTTv5IotHubRegistryManager();
            using var provisioningServiceClient = Setup.GetDpsHubServiceClient(testAgainstClassicHub: false);
            CountingRetryPolicy retryPolicy = new();
            MQTTv5DeviceTestContext device = await Setup.CreateProvisionableMQTTv5DeviceAsync(null, CreateOptions(retryPolicy), cancellationToken);
            using TelemetryClient telemetryClient = new(device.ConnectionClient);
            using TwinClient twinClient = new(device.ConnectionClient);
            await using (device)
            {
                ConnectionContext initialContext = await ConnectAsync(device, cancellationToken);
                Assert.True(await DeviceExistsAsync(registryManager, device.DeviceId, cancellationToken));
                Assert.NotNull(device.IndividualEnrollment);
                int initialDpsConsultations = retryPolicy.DeviceProvisioningServiceConsultations;

                await provisioningServiceClient.DeleteIndividualEnrollmentAsync(device.IndividualEnrollment.RegistrationId, cancellationToken);
                await registryManager.RemoveDeviceAsync(device.DeviceId, cancellationToken);
                Assert.False(await DeviceExistsAsync(registryManager, device.DeviceId, cancellationToken));

                await WaitUntilAsync(
                    () => Task.FromResult(retryPolicy.DeviceProvisioningServiceConsultations >= initialDpsConsultations + 2),
                    TimeSpan.FromSeconds(45),
                    "The MQTTv5 device did not retry provisioning while its enrollment was missing.",
                    cancellationToken);
                Assert.False(await DeviceExistsAsync(registryManager, device.DeviceId, cancellationToken));

                device.IndividualEnrollment.ETag = null;
                await provisioningServiceClient.CreateOrUpdateIndividualEnrollmentAsync(device.IndividualEnrollment, cancellationToken);

                await WaitUntilAsync(
                    () => HasReprovisionedAsync(device.ConnectionClient, initialContext, registryManager, device.DeviceId, cancellationToken),
                    TimeSpan.FromSeconds(60),
                    "The MQTTv5 device did not recover after its enrollment was restored.",
                    cancellationToken);

                await AssertFeatureClientsWorkAsync(telemetryClient, twinClient, cancellationToken);
            }
        }

        private static ConnectionClientOptions CreateOptions(IRetryPolicy? retryPolicy = null) => new()
        {
            ConnectionRetryPolicy = retryPolicy ?? CreateRetryPolicy(),
            ConnectionAttemptTimeout = TimeSpan.FromSeconds(5),
        };

        private static IRetryPolicy CreateRetryPolicy() => new ExponentialBackoffRetryPolicy(
            uint.MaxValue, TimeSpan.FromSeconds(1), useJitter: false, maxHubConnectAttemptsBeforeReprovision: 3);

        private static async Task<ConnectionContext> ConnectAsync(MQTTv5DeviceTestContext device, CancellationToken cancellationToken)
        {
            ConnectionContext context = await Setup.RetryAroundAuthorizationAsync<ConnectionContext>(
                () => device.ConnectionClient.ProvisionAndConnectAsync(
                    Setup.CreateProvisioningSettings(testAgainstClassicHub: false), device.AuthenticationProvider, cancellationToken: cancellationToken),
                cancellationToken);
            Assert.Equal(IotHubConnectionType.Mqttv5, context.ConnectionProfile);
            Assert.Equal(device.DeviceId, context.DeviceId);
            return context;
        }

        private static async Task<bool> HasReprovisionedAsync(
            ConnectionClient connectionClient,
            ConnectionContext initialContext,
            RegistryManager registryManager,
            string deviceId,
            CancellationToken cancellationToken)
        {
            ConnectionContext? context = connectionClient.GetCurrentConnectionContext();
            return context != null
                && !ReferenceEquals(context, initialContext)
                && context.ConnectionProfile == IotHubConnectionType.Mqttv5
                && context.DeviceId == deviceId
                && await DeviceExistsAsync(registryManager, deviceId, cancellationToken);
        }

        private static async Task AssertFeatureClientsWorkAsync(
            TelemetryClient telemetryClient, TwinClient twinClient, CancellationToken cancellationToken)
        {
            await telemetryClient.SendTelemetryAsync(
                new DeviceToCloudTelemetry { Payload = Encoding.UTF8.GetBytes("{\"recovered\":true}") }, cancellationToken);

            string marker = Guid.NewGuid().ToString();
            var response = await twinClient.UpdateReportedPropertiesAsync(
                new() { ReportedProperties = new JsonObject { ["recoveryMarker"] = marker } }, cancellationToken);
            Assert.Equal(Result.Ok, response.Result);

            var twin = await twinClient.GetTwinAsync(cancellationToken: cancellationToken);
            Assert.NotNull(twin.Reported);
            Assert.Equal(marker, twin.Reported["recoveryMarker"]!.GetValue<string>());
        }

        private static async Task<bool> DeviceExistsAsync(RegistryManager registryManager, string deviceId, CancellationToken cancellationToken)
        {
            try
            {
                return await registryManager.GetDeviceAsync(deviceId, cancellationToken) != null;
            }
            catch (DeviceNotFoundException)
            {
                return false;
            }
        }

        private static async Task WaitUntilAsync(
            Func<Task<bool>> condition, TimeSpan timeout, string failureMessage, CancellationToken cancellationToken)
        {
            Stopwatch elapsed = Stopwatch.StartNew();
            while (elapsed.Elapsed < timeout)
            {
                cancellationToken.ThrowIfCancellationRequested();
                if (await condition())
                {
                    return;
                }

                await Task.Delay(TimeSpan.FromSeconds(1), cancellationToken);
            }

            Assert.Fail(failureMessage);
        }

        private sealed class CountingRetryPolicy : IRetryPolicy
        {
            private readonly IRetryPolicy _inner = CreateRetryPolicy();
            private int _deviceProvisioningServiceConsultations;

            public int DeviceProvisioningServiceConsultations => Volatile.Read(ref _deviceProvisioningServiceConsultations);

            public RetryGuidance GetRetryGuidance(
                uint currentRetryCount, Exception? lastException, ConnectionEndpoint connectionEndpoint, out TimeSpan retryDelay)
            {
                if (connectionEndpoint == ConnectionEndpoint.DeviceProvisioningService)
                {
                    Interlocked.Increment(ref _deviceProvisioningServiceConsultations);
                }

                return _inner.GetRetryGuidance(currentRetryCount, lastException, connectionEndpoint, out retryDelay);
            }
        }
    }
}
