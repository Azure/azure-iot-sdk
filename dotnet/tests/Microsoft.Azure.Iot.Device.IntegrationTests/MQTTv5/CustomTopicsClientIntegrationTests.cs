// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Devices;
using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Mqtt;
using Microsoft.Azure.Iot.Device.MQTTv5.Connection;
using Microsoft.Azure.Iot.Device.MQTTv5.CustomTopics;
using Microsoft.Azure.Iot.Device.Retry;
using System.Text;
using Xunit;

namespace Microsoft.Azure.Iot.Device.IntegrationTests.MQTTv5
{
    [Trait(Setup.HubTrait, Setup.MQTTv5HubTraitValue)]
    public class CustomTopicsClientIntegrationTests
    {
        [Fact(Timeout = 2 * Setup.TestTimeoutMilliseconds, Skip = "Need test infrastructure to authorize these test custom topics")]
        public async Task TestCustomTopicSubscriptionSurvivesReprovisioningAfterDeviceDeleted()
        {
            // In this test, a device subscribes to a custom topic, then the test deletes that device from the IoT hub's
            // registry. That drops the device's connection and every reconnect attempt fails until the device falls back to
            // DPS and re-provisions. Hub connections never resume a previous MQTT session, so the subscription only survives
            // if the SDK re-subscribes to it as part of re-establishing the device's presence. A second device then publishes
            // to the custom topic, and the recovered device should still receive it.
            CancellationToken cancellationToken = TestContext.Current.CancellationToken;

            // Give the device a fast reconnect cadence and a low re-provision threshold so that, once it can no longer
            // reach its assigned hub, it falls back to DPS well within this test's time budget.
            ConnectionClientOptions receiverOptions = new()
            {
                ConnectionRetryPolicy = new ExponentialBackoffRetryPolicy(uint.MaxValue, TimeSpan.FromSeconds(1), maxHubConnectAttemptsBeforeReprovision: 3),
                ConnectionAttemptTimeout = TimeSpan.FromSeconds(5),
            };

            MQTTv5DeviceTestContext receiverContext = await Setup.CreateConnectedMQTTv5ConnectionClientAsync(null, receiverOptions, cancellationToken);
            MQTTv5DeviceTestContext? senderContext = null;
            CustomTopicsClient receiverCustomTopicsClient = new(receiverContext.ConnectionClient);
            CustomTopicsClient? senderCustomTopicsClient = null;

            try
            {
                using RegistryManager registryManager = Setup.GetMQTTv5IotHubRegistryManager();

                // This test device provisioned from an individual enrollment whose registration id is also the id it is
                // assigned in the hub, so it keeps the same device id after re-provisioning.
                string receiverDeviceId = receiverContext.DeviceId;

                string expectedPayload = Guid.NewGuid().ToString();
                var publishReceived = new TaskCompletionSource<MqttPublish>(TaskCreationOptions.RunContinuationsAsynchronously);
                receiverCustomTopicsClient.CustomTopicPublishReceivedAsync += args =>
                {
                    if (args.Publish.Payload != null && Encoding.UTF8.GetString(args.Publish.Payload) == expectedPayload)
                    {
                        publishReceived.TrySetResult(args.Publish);
                    }

                    return Task.CompletedTask;
                };

                MqttClientSubscribeReasonCode subackReasonCode = await receiverCustomTopicsClient.SubscribeAsync(
                    $"customtopicstest/{IConnectionClient.DeviceIdPlaceholder}",
                    MqttQualityOfServiceLevel.AtLeastOnce,
                    cancellationToken);
                Assert.True(
                    subackReasonCode == MqttClientSubscribeReasonCode.GrantedQoS0 || subackReasonCode == MqttClientSubscribeReasonCode.GrantedQoS1,
                    $"IoT hub did not grant the custom topic subscription. Reason code: {subackReasonCode}");

                // The device provisioned and connected, so it should be present in the hub's device registry.
                Assert.True(
                    await Setup.WaitUntilAsync(() => Setup.DeviceExistsAsync(registryManager, receiverDeviceId, cancellationToken), TimeSpan.FromSeconds(10), cancellationToken),
                    "The test device was never registered in the IoT hub after provisioning.");

                // Deleting the device from the registry drops its connection and makes every subsequent reconnect attempt
                // fail, which should eventually push the device back to DPS to re-provision.
                await registryManager.RemoveDeviceAsync(receiverDeviceId, cancellationToken);

                // Re-provisioning re-registers the device in the hub's registry, so its reappearance is the signal that the
                // device recovered by re-provisioning rather than just reconnecting to the hub it had been removed from.
                Assert.True(
                    await Setup.WaitUntilAsync(() => Setup.DeviceExistsAsync(registryManager, receiverDeviceId, cancellationToken), TimeSpan.FromSeconds(45), cancellationToken),
                    "The device did not re-provision and reappear in the IoT hub registry after being deleted.");

                // Wait for the device's presence to be fully re-established. Operations wait on the device presence flow
                // when the client is not yet connected, and that flow is where custom topic subscriptions are re-sent, so a
                // successful publish from the recovered device means its custom topic subscription has been re-sent too.
                MqttPublishAck receiverPublishAck = await receiverCustomTopicsClient.PublishAsync(
                    new MqttPublish()
                    {
                        Topic = $"customtopicstest/{IConnectionClient.DeviceIdPlaceholder}/presence",
                        Payload = Encoding.UTF8.GetBytes("presence check"),
                        QualityOfServiceLevel = MqttQualityOfServiceLevel.AtLeastOnce,
                    },
                    cancellationToken);
                Assert.True(
                    receiverPublishAck.ReasonCode == MqttPublishAckReasonCode.Success || receiverPublishAck.ReasonCode == MqttPublishAckReasonCode.NoMatchingSubscribers,
                    $"The recovered device failed to publish. Reason code: {receiverPublishAck.ReasonCode}");

                // Publish to the receiving device's custom topic from a completely separate device identity (its own device
                // id, certificate and DPS enrollment) so that the receiving device can only get this traffic through its
                // re-established subscription.
                senderContext = await Setup.CreateConnectedMQTTv5ConnectionClientAsync(null, null, cancellationToken);
                Assert.NotEqual(receiverDeviceId, senderContext.DeviceId);
                senderCustomTopicsClient = new(senderContext.ConnectionClient);

                MqttPublishAck senderPublishAck = await senderCustomTopicsClient.PublishAsync(
                    new MqttPublish()
                    {
                        Topic = $"customtopicstest/{receiverDeviceId}",
                        Payload = Encoding.UTF8.GetBytes(expectedPayload),
                        QualityOfServiceLevel = MqttQualityOfServiceLevel.AtLeastOnce,
                    },
                    cancellationToken);
                Assert.Equal(MqttPublishAckReasonCode.Success, senderPublishAck.ReasonCode);

                MqttPublish receivedPublish = await publishReceived.Task.WaitAsync(TimeSpan.FromSeconds(30), cancellationToken);
                Assert.Equal($"customtopicstest/{receiverDeviceId}", receivedPublish.Topic);
            }
            finally
            {
                // Dispose the device contexts before any feature clients so that the test device identities can be cleaned
                // up and the MQTT clients disconnected gracefully.
                await receiverContext.DisposeAsync();
                if (senderContext != null)
                {
                    await senderContext.DisposeAsync();
                }

                receiverCustomTopicsClient.Dispose(false);
                senderCustomTopicsClient?.Dispose(false);
            }
        }
    }
}
