// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Models.Telemetry;
using Microsoft.Azure.Iot.Device.Mqtt;
using Microsoft.Azure.Iot.Device.MqttNetAdapter;
using Microsoft.Azure.Iot.Device.MQTTnetAdapter;
using Microsoft.Azure.Iot.Device.MQTTv5.Connection;
using Microsoft.Azure.Iot.Device.MQTTv5.CustomTopics;
using Microsoft.Azure.Iot.Device.MQTTv5.Telemetry;
using System.Text;
using Xunit;
using ConnectionClientOptions = Microsoft.Azure.Iot.Device.MQTTv5.Models.ConnectionClientOptions;

namespace Microsoft.Azure.Iot.Device.IntegrationTests.MQTTv5
{
    /// <summary>
    /// Mirrors the C SDK's e2e_mqttv5_custom_topic suite. The MQTTv5 hub must have the custom topic templates
    /// e2e/{deviceId}/# and e2e/shared/#. Unlike the C suite, these tests check IoT hub's PUBACK and the health of the
    /// connection only; they do not read routed messages from the hub's events endpoint.
    /// </summary>
    [Trait(Setup.HubTrait, Setup.MQTTv5HubTraitValue)]
    public class CustomTopicsClientIntegrationTests
    {
        private const string DeviceTopicPrefix = "e2e/" + IConnectionClient.DeviceIdPlaceholder;

        // Baseline: telemetry over the same connection is accepted, so a custom topic failure below is about custom
        // topics, not the environment.
        [Fact(Timeout = Setup.TestTimeoutMilliseconds)]
        public async Task BaselineTelemetryIsAccepted()
        {
            await using CustomTopicsTestDevice device = await CustomTopicsTestDevice.CreateAsync();

            // Not 'using': disposing it the default way would dispose the shared connection client before the device disconnects it.
            TelemetryClient telemetryClient = new(device.Context.ConnectionClient);
            try
            {
                await telemetryClient.SendTelemetryAsync(
                    new DeviceToCloudTelemetry { Payload = CreatePayload("tm") },
                    TestContext.Current.CancellationToken);
            }
            finally
            {
                telemetryClient.Dispose(disposing: false);
            }
        }

        [Fact(Timeout = Setup.TestTimeoutMilliseconds)]
        public async Task Qos1ToADeviceTemplateIsAccepted()
        {
            await using CustomTopicsTestDevice device = await CustomTopicsTestDevice.CreateAsync();

            MqttPublishAck puback = await device.PublishAsync($"{DeviceTopicPrefix}/telemetry", MqttQualityOfServiceLevel.AtLeastOnce);

            AssertAccepted(puback);
        }

        // IoT hub does not acknowledge QoS 0, so this only checks that the publish is sent and the connection survives it.
        [Fact(Timeout = Setup.TestTimeoutMilliseconds)]
        public async Task Qos0ToASharedTemplateIsSent()
        {
            await using CustomTopicsTestDevice device = await CustomTopicsTestDevice.CreateAsync();
            int drops = device.HubDrops;

            MqttPublishAck puback = await device.PublishAsync("e2e/shared/alerts", MqttQualityOfServiceLevel.AtMostOnce);

            AssertAccepted(puback);
            AssertAccepted(await device.PublishAsync($"{DeviceTopicPrefix}/after-qos0", MqttQualityOfServiceLevel.AtLeastOnce));
            Assert.Equal(drops, device.HubDrops);
        }

        [Fact(Timeout = Setup.TestTimeoutMilliseconds)]
        public async Task ATopicNoTemplateAllowsIsRefused()
        {
            await using CustomTopicsTestDevice device = await CustomTopicsTestDevice.CreateAsync();
            int drops = device.HubDrops;

            MqttPublishAck puback = await device.PublishAsync("e2e-unlisted/telemetry", MqttQualityOfServiceLevel.AtLeastOnce);

            AssertRefused(puback);
            Assert.Equal(drops, device.HubDrops);
        }

        [Fact(Timeout = Setup.TestTimeoutMilliseconds)]
        public async Task AnotherDeviceIdIsRefused()
        {
            await using CustomTopicsTestDevice device = await CustomTopicsTestDevice.CreateAsync();
            int drops = device.HubDrops;

            MqttPublishAck puback = await device.PublishAsync(
                $"e2e/not-{IConnectionClient.DeviceIdPlaceholder}/telemetry", MqttQualityOfServiceLevel.AtLeastOnce);

            AssertRefused(puback);
            Assert.Equal(drops, device.HubDrops);
        }

        // A refusal leaves the connection usable.
        [Fact(Timeout = Setup.TestTimeoutMilliseconds)]
        public async Task PublishingContinuesAfterARefusal()
        {
            await using CustomTopicsTestDevice device = await CustomTopicsTestDevice.CreateAsync();
            int drops = device.HubDrops;

            AssertRefused(await device.PublishAsync("e2e-unlisted/telemetry", MqttQualityOfServiceLevel.AtLeastOnce));
            AssertRefused(await device.PublishAsync(
                $"e2e/not-{IConnectionClient.DeviceIdPlaceholder}/telemetry", MqttQualityOfServiceLevel.AtLeastOnce));

            AssertAccepted(await device.PublishAsync($"{DeviceTopicPrefix}/after", MqttQualityOfServiceLevel.AtLeastOnce));
            Assert.Equal(drops, device.HubDrops);
        }

        // "{deviceId}" resolves to the hub-assigned device ID (which need not equal the registration ID), and IoT hub
        // accepts the result just as it accepts that device ID written into the topic directly.
        [Fact(Timeout = Setup.TestTimeoutMilliseconds)]
        public async Task DeviceIdPlaceholderAndConcreteDeviceIdAreAccepted()
        {
            await using CustomTopicsTestDevice device = await CustomTopicsTestDevice.CreateAsync();
            string deviceId = device.CurrentDeviceId;

            AssertAccepted(await device.PublishAsync($"{DeviceTopicPrefix}/fmt/a-b_c.0", MqttQualityOfServiceLevel.AtLeastOnce));
            AssertAccepted(await device.PublishAsync($"e2e/{deviceId}/fmt/a-b_c.0", MqttQualityOfServiceLevel.AtLeastOnce));
        }

        [Fact(Timeout = Setup.TestTimeoutMilliseconds)]
        public async Task TopicAtMaxLengthIsAccepted()
        {
            await using CustomTopicsTestDevice device = await CustomTopicsTestDevice.CreateAsync();
            string topic = MakeTopicOfLength(device.CurrentDeviceId, CustomTopicsClient.MaxTopicLength);

            AssertAccepted(await device.PublishAsync(topic, MqttQualityOfServiceLevel.AtLeastOnce));
        }

        [Fact(Timeout = Setup.TestTimeoutMilliseconds)]
        public async Task TopicAtMaxLevelsIsAccepted()
        {
            await using CustomTopicsTestDevice device = await CustomTopicsTestDevice.CreateAsync();
            string topic = MakeTopicOfLevels(device.CurrentDeviceId, CustomTopicsClient.MaxTopicLevels);

            AssertAccepted(await device.PublishAsync(topic, MqttQualityOfServiceLevel.AtLeastOnce));
        }

        // One byte or one level past the limit is refused by the client without sending anything; the connection stays usable.
        [Fact(Timeout = Setup.TestTimeoutMilliseconds)]
        public async Task TopicPastLimitsIsRejectedLocally()
        {
            await using CustomTopicsTestDevice device = await CustomTopicsTestDevice.CreateAsync();
            int drops = device.HubDrops;
            string deviceId = device.CurrentDeviceId;

            await Assert.ThrowsAsync<ArgumentException>(() => device.PublishAsync(
                MakeTopicOfLength(deviceId, CustomTopicsClient.MaxTopicLength + 1), MqttQualityOfServiceLevel.AtLeastOnce));
            await Assert.ThrowsAsync<ArgumentException>(() => device.PublishAsync(
                MakeTopicOfLevels(deviceId, CustomTopicsClient.MaxTopicLevels + 1), MqttQualityOfServiceLevel.AtLeastOnce));

            AssertAccepted(await device.PublishAsync($"{DeviceTopicPrefix}/after-limits", MqttQualityOfServiceLevel.AtLeastOnce));
            Assert.Equal(drops, device.HubDrops);
        }

        [Fact(Timeout = Setup.TestTimeoutMilliseconds)]
        public async Task UserPropertiesAreAccepted()
        {
            await using CustomTopicsTestDevice device = await CustomTopicsTestDevice.CreateAsync();

            MqttPublishAck puback = await device.PublishAsync(
                $"{DeviceTopicPrefix}/props",
                MqttQualityOfServiceLevel.AtLeastOnce,
                publish =>
                {
                    publish.AddUserProperty("color", "blue");
                    publish.AddUserProperty("with space", "a value with spaces");
                    publish.AddUserProperty("reserved", "a/b+c#d?e=f&g");
                    publish.AddUserProperty("empty", "");
                    publish.AddUserProperty("e2e", "repeated");
                });

            AssertAccepted(puback);
        }

        // After a re-provision, a topic with the placeholder resolves to the newly assigned device ID and is accepted. The
        // enrollment fixes the device ID, so DPS assigns the same one; the unit tests cover a change.
        [Fact(Timeout = 2 * Setup.TestTimeoutMilliseconds)]
        public async Task ATopicSentAfterAReprovisionIsAccepted()
        {
            CancellationToken cancellationToken = TestContext.Current.CancellationToken;
            await using CustomTopicsTestDevice device = await CustomTopicsTestDevice.CreateAsync();
            ConnectionClient connectionClient = device.Context.ConnectionClient;
            ConnectionContext initialContext = connectionClient.GetCurrentConnectionContext()!;

            await connectionClient.DisconnectAsync(null, cancellationToken);

            await Assert.ThrowsAsync<NotSupportedException>(() => device.PublishAsync(
                $"{DeviceTopicPrefix}/reprovisioned", MqttQualityOfServiceLevel.AtLeastOnce));

            ConnectionContext reprovisionedContext = await Setup.RetryAroundAuthorizationAsync<ConnectionContext>(
                () => connectionClient.ProvisionAndConnectAsync(
                    Setup.CreateProvisioningSettings(testAgainstClassicHub: false),
                    device.Context.AuthenticationProvider,
                    forceProvisioning: true,
                    cancellationToken: cancellationToken),
                cancellationToken);

            Assert.NotSame(initialContext, reprovisionedContext);
            Assert.Equal(IotHubConnectionType.Mqttv5, reprovisionedContext.ConnectionProfile);

            AssertAccepted(await device.PublishAsync($"{DeviceTopicPrefix}/reprovisioned", MqttQualityOfServiceLevel.AtLeastOnce));
            AssertAccepted(await device.PublishAsync(
                $"e2e/{reprovisionedContext.DeviceId}/reprovisioned", MqttQualityOfServiceLevel.AtLeastOnce));
        }

        private static void AssertAccepted(MqttPublishAck puback)
        {
            Assert.True(
                puback.ReasonCode == MqttPublishAckReasonCode.Success,
                $"Expected the publish to be accepted, but it was refused with {puback.ReasonCode} (\"{puback.ReasonString}\").");
        }

        // Every refusal IoT hub reports for a topic the device may not publish to is NotAuthorized (0x87).
        private static void AssertRefused(MqttPublishAck puback)
        {
            Assert.Equal(MqttPublishAckReasonCode.NotAuthorized, puback.ReasonCode);
        }

        // Unique per run, since the events endpoint is shared by concurrent runs.
        private static byte[] CreatePayload(string tag)
        {
            return Encoding.UTF8.GetBytes($"{{\"marker\":\"{tag}-{Guid.NewGuid()}\"}}");
        }

        // e2e/<device ID>/len/ padded with 'x' to exactly length bytes.
        private static string MakeTopicOfLength(string deviceId, int length)
        {
            string prefix = $"e2e/{deviceId}/len/";
            Assert.True(Encoding.UTF8.GetByteCount(prefix) < length);
            return prefix + new string('x', length - Encoding.UTF8.GetByteCount(prefix));
        }

        // e2e/<device ID>/l2/l3/... with exactly levels levels.
        private static string MakeTopicOfLevels(string deviceId, int levels)
        {
            StringBuilder topic = new($"e2e/{deviceId}");
            for (int level = 2; level < levels; level++)
            {
                topic.Append("/l").Append(level);
            }

            Assert.Equal(levels, topic.ToString().Split('/').Length);
            return topic.ToString();
        }

        private sealed class CustomTopicsTestDevice : IAsyncDisposable
        {
            private int _hubDrops;
            private bool _connected;

            private CustomTopicsTestDevice(MQTTv5DeviceTestContext context, MqttNetClient mqttClient)
            {
                Context = context;
                Client = new CustomTopicsClient(context.ConnectionClient);
                _connected = true;
                mqttClient.ConnectedAsync += _ =>
                {
                    Volatile.Write(ref _connected, true);
                    return Task.CompletedTask;
                };
                mqttClient.DisconnectedAsync += _ =>
                {
                    if (Volatile.Read(ref _connected))
                    {
                        Interlocked.Increment(ref _hubDrops);
                    }

                    Volatile.Write(ref _connected, false);
                    return Task.CompletedTask;
                };
            }

            public MQTTv5DeviceTestContext Context { get; }

            public CustomTopicsClient Client { get; }

            // Times the connection to IoT hub was lost after the device connected. A refused publish must not cause one.
            public int HubDrops => Volatile.Read(ref _hubDrops);

            public string CurrentDeviceId => Context.ConnectionClient.GetCurrentConnectionContext()!.DeviceId;

            public static async Task<CustomTopicsTestDevice> CreateAsync()
            {
                MqttNetClient mqttClient = new();
                MQTTv5DeviceTestContext context = await Setup.CreateConnectedMQTTv5ConnectionClientAsync(
                    null, new ConnectionClientOptions { MqttClient = mqttClient }, cancellationToken: TestContext.Current.CancellationToken);
                return new CustomTopicsTestDevice(context, mqttClient);
            }

            public Task<MqttPublishAck> PublishAsync(string topic, MqttQualityOfServiceLevel qos, Action<MqttPublish>? configure = null)
            {
                MqttPublish publish = new()
                {
                    Topic = topic,
                    Payload = CreatePayload("ct"),
                    QualityOfServiceLevel = qos,
                    ContentType = "application/json",
                };
                publish.AddUserProperty("e2e", "custom-topic");
                configure?.Invoke(publish);
                return Client.PublishAsync(publish, TestContext.Current.CancellationToken);
            }

            // Dispose the device context before the feature client so that the device identity is cleaned up and the
            // MQTT client disconnected gracefully.
            public async ValueTask DisposeAsync()
            {
                try
                {
                    await Context.DisposeAsync();
                }
                finally
                {
                    Client.Dispose(disposing: false);
                }
            }
        }
    }
}
