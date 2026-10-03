// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Devices;
using Microsoft.Azure.Devices.Provisioning.Service;
using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Retry;
using Microsoft.Azure.Iot.Device.Unified.Connection;
using Microsoft.Azure.Iot.Device.Unified.Telemetry;
using System;
using Xunit;

namespace Microsoft.Azure.Iot.Device.IntegrationTests.Unified
{
    public class RetryIntegrationTests
    {
        [Fact(Timeout = Setup.TestTimeoutMilliseconds)]
        public async Task TestReprovisioningAfterDeviceDeleted()
        {
            // In this test, the test device should successfully provision to an IoT Hub and successfully connect to that IoT Hub.
            // Shortly afterwards, the test should deliberately delete the device from the IoT Hub's device registry to cause
            // the device to lose connection. The device should attempt to reconnect to Hub for a while, but should eventually
            // reprovision
            CancellationToken cancellationToken = TestContext.Current.CancellationToken;

            using RegistryManager registryManager = Setup.GetMQTTv3IotHubRegistryManager();
            using ProvisioningServiceClient provisioningServiceClient = Setup.GetDpsHubServiceClient();

            // Give the device a fast reconnect cadence and a low re-provision threshold so that, once it can no longer
            // reach its assigned hub, it falls back to DPS well within this test's time budget rather than retrying the
            // unreachable hub for the production default number of attempts.
            ConnectionClientOptions options = new()
            {
                ConnectionRetryPolicy = new ExponentialBackoffRetryPolicy(uint.MaxValue, TimeSpan.FromSeconds(1)),
                ConnectionAttemptTimeout = TimeSpan.FromSeconds(5),
                MaxHubConnectAttemptsBeforeReprovision = 3,
            };

            await using UnifiedDeviceTestContext device = await Setup.CreateConnectedUnifiedConnectionClientAsync(
                testAgainstClassicHub: true,
                options,
                cancellationToken);

            // This test device provisioned from an individual enrollment whose registration id is also the id it is
            // assigned in the hub, so the same value identifies it in both the registry and DPS.
            string deviceId = device.ConnectionContext.DeviceId!;

            // The device provisioned and connected, so it should be present in the hub's device registry.
            Assert.True(
                await WaitUntilAsync(() => DeviceExistsAsync(registryManager, deviceId, cancellationToken), TimeSpan.FromSeconds(10), cancellationToken),
                "The test device was never registered in the IoT hub after provisioning.");

            // Deleting the device from the registry drops its connection and makes every subsequent reconnect attempt
            // fail, which should eventually push the device back to DPS to re-provision.
            await registryManager.RemoveDeviceAsync(deviceId, cancellationToken);

            // Re-provisioning re-registers the device in the hub's registry, so its reappearance is the signal that the
            // device recovered by re-provisioning rather than just reconnecting to the hub it had been removed from.
            Assert.True(
                await WaitUntilAsync(() => DeviceExistsAsync(registryManager, deviceId, cancellationToken), TimeSpan.FromSeconds(45), cancellationToken),
                "The device did not re-provision and reappear in the IoT hub registry after being deleted.");

            // Since the device should be provisioned to the Hub again, it should be capable of doing basic operations like sending telemetry
            TelemetryClient telemetryClient = new(device.ConnectionClient);
            await telemetryClient.SendTelemetryAsync(new Device.Models.Telemetry.DeviceToCloudTelemetry(), cancellationToken);
            telemetryClient.Dispose(false);
        }

        [Fact(Timeout = 2 * Setup.TestTimeoutMilliseconds)]
        public async Task TestReprovisioningAfterDeviceDeletedAndEnrollmentTemporarilyDeleted()
        {
            // In this test, the test device should successfully provision to an IoT Hub and successfully connect to that IoT Hub.
            // Shortly afterwards, the test should deliberately delete that device's enrollment in DPS and delete the device from
            // the IoT Hub's device registry. This should cause the device to attempt to reconnect to Hub for a while before falling
            // back to re-provisioning. However, re-provisioning should fail for a while before this test re-adds the device's enrollment
            // to DPS to allow re-provisioning to succeed
            CancellationToken cancellationToken = TestContext.Current.CancellationToken;

            using RegistryManager registryManager = Setup.GetMQTTv3IotHubRegistryManager();
            using ProvisioningServiceClient provisioningServiceClient = Setup.GetDpsHubServiceClient();

            // As above, a fast reconnect cadence and a low re-provision threshold keep the hub-retry-then-reprovision cycle
            // inside this test's time budget. The retry policy is wrapped so the test can count how many times the client
            // consults it specifically for Device Provisioning Service, which proves re-provisioning was retried rather
            // than succeeding on its first attempt while the enrollment was still missing.
            CountingRetryPolicy retryPolicy = new(new ExponentialBackoffRetryPolicy(uint.MaxValue, TimeSpan.FromSeconds(1)));
            ConnectionClientOptions options = new()
            {
                ConnectionRetryPolicy = retryPolicy,
                ConnectionAttemptTimeout = TimeSpan.FromSeconds(5),
                MaxHubConnectAttemptsBeforeReprovision = 3,
            };

            await using UnifiedDeviceTestContext device = await Setup.CreateConnectedUnifiedConnectionClientAsync(
                testAgainstClassicHub: true,
                options,
                cancellationToken);

            // This test device provisioned from an individual enrollment whose registration id is also the id it is
            // assigned in the hub, so the same value identifies it in both the registry and DPS.
            string deviceId = device.ConnectionContext.DeviceId!;

            // The device provisioned and connected, so it should be present in the hub's device registry.
            Assert.True(
                await WaitUntilAsync(() => DeviceExistsAsync(registryManager, deviceId, cancellationToken), TimeSpan.FromSeconds(10), cancellationToken),
                "The test device was never registered in the IoT hub after provisioning.");

            // Remove both the device's enrollment and its registry entry. Losing the registry entry drops the hub
            // connection and sends the device back to DPS, but with no enrollment in place the re-provisioning attempts
            // cannot succeed yet.
            await provisioningServiceClient.DeleteIndividualEnrollmentAsync(deviceId, cancellationToken);
            await registryManager.RemoveDeviceAsync(deviceId, cancellationToken);

            // Wait for the client to fall back to DPS and have re-provisioning fail more than once while the enrollment
            // is missing. Waiting for multiple DPS retry consultations before restoring the enrollment (rather than
            // restoring it right away) proves the device did not simply re-provision on its first attempt; it kept
            // retrying provisioning, which is what lets restoring the enrollment recover it.
            const int requiredDpsConsultations = 2;
            Assert.True(
                await WaitUntilAsync(
                    () => Task.FromResult(retryPolicy.DeviceProvisioningServiceConsultations >= requiredDpsConsultations),
                    TimeSpan.FromSeconds(45),
                    cancellationToken),
                $"The DPS retry policy was consulted only {retryPolicy.DeviceProvisioningServiceConsultations} time(s) while the enrollment was missing; expected at least {requiredDpsConsultations}.");

            // Re-provisioning cannot succeed while the enrollment is gone, so the device must not have reappeared in the
            // registry during that wait.
            Assert.False(
                await DeviceExistsAsync(registryManager, deviceId, cancellationToken),
                "The device reappeared in the registry even though its DPS enrollment had been deleted.");

            // Restore the enrollment this device originally provisioned from so that its ongoing re-provisioning
            // attempts can finally succeed. Reusing the cached enrollment restores the exact same registration and
            // device certificate the device was provisioned with. Its ETag is cleared first because the enrollment was
            // deleted above, so the recreation must be unconditional rather than matching the now-gone resource.
            device.IndividualEnrollment.ETag = null;
            await provisioningServiceClient.CreateOrUpdateIndividualEnrollmentAsync(device.IndividualEnrollment, cancellationToken);

            // With the enrollment back in place, the device should re-provision and reappear in the hub's registry.
            Assert.True(
                await WaitUntilAsync(() => DeviceExistsAsync(registryManager, deviceId, cancellationToken), TimeSpan.FromSeconds(60), cancellationToken),
                "The device did not re-provision after its enrollment was restored.");

            // Since the device should be provisioned to the Hub again, it should be capable of doing basic operations like sending telemetry
            TelemetryClient telemetryClient = new(device.ConnectionClient);
            await telemetryClient.SendTelemetryAsync(new Device.Models.Telemetry.DeviceToCloudTelemetry(), cancellationToken);
            telemetryClient.Dispose(false);
        }

        // Polls a condition until it becomes true or the timeout elapses.
        private static async Task<bool> WaitUntilAsync(Func<Task<bool>> condition, TimeSpan timeout, CancellationToken cancellationToken)
        {
            DateTime deadline = DateTime.UtcNow + timeout;
            while (DateTime.UtcNow < deadline)
            {
                cancellationToken.ThrowIfCancellationRequested();

                if (await condition())
                {
                    return true;
                }

                await Task.Delay(TimeSpan.FromSeconds(2), cancellationToken);
            }

            return false;
        }

        private static async Task<bool> DeviceExistsAsync(RegistryManager registryManager, string deviceId, CancellationToken cancellationToken)
        {
            try
            {
                return await registryManager.GetDeviceAsync(deviceId, cancellationToken) != null;
            }
            catch
            {
                // Treat any failure to read the device (including a not-found response) as the device not being present.
                return false;
            }
        }

        /// <summary>
        /// Wraps another retry policy and, using the endpoint the client passes to each retry check, counts how many of
        /// those checks were for Device Provisioning Service. This lets a test tell re-provisioning retries apart from
        /// hub reconnection retries and assert on how many times the device fell back to DPS.
        /// </summary>
        private sealed class CountingRetryPolicy : IRetryPolicy
        {
            private readonly IRetryPolicy _inner;
            private int _deviceProvisioningServiceConsultations;

            public CountingRetryPolicy(IRetryPolicy inner) => _inner = inner;

            /// <summary>How many times this policy has been consulted for a Device Provisioning Service retry.</summary>
            public int DeviceProvisioningServiceConsultations => Volatile.Read(ref _deviceProvisioningServiceConsultations);

            public bool ShouldRetry(uint currentRetryCount, Exception? lastException, ConnectionEndpoint connectionEndpoint, out TimeSpan retryDelay)
            {
                if (connectionEndpoint == ConnectionEndpoint.DeviceProvisioningService)
                {
                    Interlocked.Increment(ref _deviceProvisioningServiceConsultations);
                }

                return _inner.ShouldRetry(currentRetryCount, lastException, connectionEndpoint, out retryDelay);
            }
        }
    }
}
