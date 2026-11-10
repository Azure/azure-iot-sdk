// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Google.Protobuf;
using Microsoft.Azure.Iot.Device.Authentication;
using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Models.DirectMethods;
using Microsoft.Azure.Iot.Device.Models.Twin;
using Microsoft.Azure.Iot.Device.MQTTv5.Connection;
using Microsoft.Azure.Iot.Device.MQTTv5.DirectMethods;
using Microsoft.Azure.Iot.Device.MQTTv5.Twin;
using Microsoft.Azure.Iot.Device.Models.Telemetry;
using Microsoft.Azure.Iot.Device.MQTTv5.Models;
using Microsoft.Azure.Iot.Device.Mqtt;
using System.Text;
using Xunit;
using Mqttv5ConnectionClient = Microsoft.Azure.Iot.Device.MQTTv5.Connection.ConnectionClient;
using Mqttv5ConnectionClientOptions = Microsoft.Azure.Iot.Device.MQTTv5.Models.ConnectionClientOptions;
using UnifiedConnectionClient = Microsoft.Azure.Iot.Device.Unified.Connection.ConnectionClient;
using UnifiedConnectionClientOptions = Microsoft.Azure.Iot.Device.Unified.Models.ConnectionClientOptions;

namespace Microsoft.Azure.Iot.Device.UnitTests.MQTTv5
{
    /// <summary>
    /// The topics that births, direct method responses and twin requests are published on in each
    /// <see cref="ServiceBoundTopicFormat"/>, through both the MQTTv5 and the unified connection clients.
    /// </summary>
    public class ServiceBoundTopicFormatUnitTests
    {
        private const string DeviceId = "someDeviceId";
        private static readonly TimeSpan s_testTimeout = TimeSpan.FromSeconds(30);

        [Fact]
        public void OptionsDefaultToPerDevice()
        {
            Assert.Equal(ServiceBoundTopicFormat.PerDevice, new Mqttv5ConnectionClientOptions().ServiceBoundTopicFormat);
            Assert.Equal(ServiceBoundTopicFormat.PerDevice, new UnifiedConnectionClientOptions().ServiceBoundTopicFormat);
            Assert.Equal(0, (int)ServiceBoundTopicFormat.PerDevice);
            Assert.Equal(1, (int)ServiceBoundTopicFormat.Legacy);
        }

        [Theory]
        [InlineData(ServiceBoundTopicFormat.PerDevice, "presence", "ih/srv/dev1")]
        [InlineData(ServiceBoundTopicFormat.PerDevice, "methods", "ih/srv/dev1")]
        [InlineData(ServiceBoundTopicFormat.PerDevice, "twin", "ih/srv/dev1")]
        [InlineData(ServiceBoundTopicFormat.Legacy, "presence", "ih/dev1/srv/presence")]
        [InlineData(ServiceBoundTopicFormat.Legacy, "methods", "ih/dev1/srv/methods")]
        [InlineData(ServiceBoundTopicFormat.Legacy, "twin", "ih/dev1/srv/twin")]
        public void ServiceBoundTopicsMatchFormat(ServiceBoundTopicFormat format, string legacySuffix, string expectedTopic)
        {
            Assert.Equal(expectedTopic, ServiceBoundTopics.Get(format, "dev1", legacySuffix));
        }

        [Theory]
        [InlineData(false)]
        [InlineData(true)]
        public async Task DefaultOptionsPublishBirthOnPerDeviceTopic(bool useUnifiedClient)
        {
            using MockConnectionMqttClient mockMqttClient = new() { AutoRespondToBirth = true };
            using IDisposable connectionClient = useUnifiedClient
                ? new UnifiedConnectionClient(new UnifiedConnectionClientOptions { MqttClient = mockMqttClient })
                : new Mqttv5ConnectionClient(new Mqttv5ConnectionClientOptions { MqttClient = mockMqttClient });

            await ConnectAsync((AbstractConnectionClient)connectionClient);

            MqttPublish birth = Assert.Single(SentPublishes(mockMqttClient), publish => GetType(publish) == "birth:1");
            Assert.Equal("ih/srv/" + DeviceId, birth.Topic);
        }

        [Theory]
        [InlineData(false, ServiceBoundTopicFormat.PerDevice, "ih/srv/someDeviceId")]
        [InlineData(true, ServiceBoundTopicFormat.PerDevice, "ih/srv/someDeviceId")]
        [InlineData(false, ServiceBoundTopicFormat.Legacy, "ih/someDeviceId/srv/presence")]
        [InlineData(true, ServiceBoundTopicFormat.Legacy, "ih/someDeviceId/srv/presence")]
        public async Task BirthIsPublishedOnTopicOfFormat(bool useUnifiedClient, ServiceBoundTopicFormat format, string expectedTopic)
        {
            using MockConnectionMqttClient mockMqttClient = new() { AutoRespondToBirth = true };
            using IDisposable connectionClient = CreateConnectionClient(useUnifiedClient, format, mockMqttClient);

            await ConnectAsync((AbstractConnectionClient)connectionClient);

            MqttPublish birth = Assert.Single(SentPublishes(mockMqttClient), publish => GetType(publish) == "birth:1");
            Assert.Equal(expectedTopic, birth.Topic);
            Assert.Equal(MqttQualityOfServiceLevel.AtMostOnce, birth.QualityOfServiceLevel);
            Assert.NotNull(birth.CorrelationData);
            Birth.Parser.ParseFrom(birth.Payload);

            // The device still subscribes to its device bound topics, whichever the format.
            MqttSubscribe subscribe = Assert.Single(mockMqttClient.SentMqttTrafficInOrder, traffic => traffic.Subscribe != null).Subscribe!;
            Assert.Contains(subscribe.TopicFilters, filter => filter.Topic == "ih/" + DeviceId + "/dev/#");
        }

        [Theory]
        [InlineData(false, ServiceBoundTopicFormat.PerDevice, "ih/srv/someDeviceId")]
        [InlineData(true, ServiceBoundTopicFormat.PerDevice, "ih/srv/someDeviceId")]
        [InlineData(false, ServiceBoundTopicFormat.Legacy, "ih/someDeviceId/srv/methods")]
        [InlineData(true, ServiceBoundTopicFormat.Legacy, "ih/someDeviceId/srv/methods")]
        public async Task DirectMethodResponsesArePublishedOnTopicOfFormat(bool useUnifiedClient, ServiceBoundTopicFormat format, string expectedTopic)
        {
            using MockConnectionMqttClient mockMqttClient = new() { AutoRespondToBirth = true };
            using IDisposable connectionClient = CreateConnectionClient(useUnifiedClient, format, mockMqttClient);
            Func<DirectMethodRequestReceivedEventArgs, Task<DirectMethodResponse>> handler =
                _ => Task.FromResult(new DirectMethodResponse() { Status = 200, Payload = Encoding.UTF8.GetBytes("done") });
            using IDisposable directMethodClient = useUnifiedClient
                ? CreateUnifiedDirectMethodClient((UnifiedConnectionClient)connectionClient, handler)
                : CreateMqttv5DirectMethodClient((Mqttv5ConnectionClient)connectionClient, handler);

            await ConnectAsync((AbstractConnectionClient)connectionClient);

            byte[] correlationData = Guid.NewGuid().ToByteArray();
            await mockMqttClient.SimulatePublishReceivedAsync(CreateInboundMethodPublish(
                "probe:1", correlationData, new Probe() { MethodName = "reboot", ResponseTimeoutSeconds = 30 }.ToByteArray()));
            MqttPublish probeAck = await WaitForSentPublishAsync(mockMqttClient, "probe-ack:1");
            Assert.Equal(expectedTopic, probeAck.Topic);
            Assert.Equal(correlationData, probeAck.CorrelationData);
            ProbeAck sentProbeAck = ProbeAck.Parser.ParseFrom(probeAck.Payload);
            Assert.NotNull(sentProbeAck.Ready);

            await mockMqttClient.SimulatePublishReceivedAsync(CreateInboundMethodPublish(
                "exec:1", correlationData, new Exec() { ReadyId = sentProbeAck.Ready.ReadyId }.ToByteArray()));
            MqttPublish result = await WaitForSentPublishAsync(mockMqttClient, "result:1");
            Assert.Equal(expectedTopic, result.Topic);
            Assert.Equal(correlationData, result.CorrelationData);
            Assert.Equal(200, Models.DirectMethods.Result.Parser.ParseFrom(result.Payload).Status);
        }

        [Theory]
        [InlineData(false, ServiceBoundTopicFormat.PerDevice, "ih/srv/someDeviceId")]
        [InlineData(true, ServiceBoundTopicFormat.PerDevice, "ih/srv/someDeviceId")]
        [InlineData(false, ServiceBoundTopicFormat.Legacy, "ih/someDeviceId/srv/twin")]
        [InlineData(true, ServiceBoundTopicFormat.Legacy, "ih/someDeviceId/srv/twin")]
        public async Task TwinRequestsArePublishedOnTopicOfFormat(bool useUnifiedClient, ServiceBoundTopicFormat format, string expectedTopic)
        {
            using MockConnectionMqttClient mockMqttClient = new() { AutoRespondToBirth = true };
            using IDisposable connectionClient = CreateConnectionClient(useUnifiedClient, format, mockMqttClient);
            using IDisposable twinClient = useUnifiedClient
                ? new Device.Unified.Twin.TwinClient((UnifiedConnectionClient)connectionClient)
                : new Device.MQTTv5.Twin.TwinClient((Mqttv5ConnectionClient)connectionClient);

            await ConnectAsync((AbstractConnectionClient)connectionClient);

            using CancellationTokenSource getCancellation = CancellationTokenSource.CreateLinkedTokenSource(TestContext.Current.CancellationToken);
            Task getTwin = twinClient is Device.Unified.Twin.TwinClient unifiedTwinClient
                ? unifiedTwinClient.GetTwinAsync(getCancellation.Token)
                : ((Device.MQTTv5.Twin.TwinClient)twinClient).GetTwinAsync(cancellationToken: getCancellation.Token);
            MqttPublish getRequest = await WaitForSentPublishAsync(mockMqttClient, "get:1");
            Assert.Equal(expectedTopic, getRequest.Topic);
            Assert.NotNull(getRequest.CorrelationData);
            TwinGet.Parser.ParseFrom(getRequest.Payload);

            await getCancellation.CancelAsync();
            await Assert.ThrowsAnyAsync<OperationCanceledException>(() => getTwin.WaitAsync(s_testTimeout, TestContext.Current.CancellationToken));

            Task<ReportedPatchResponse> patch = twinClient is Device.Unified.Twin.TwinClient unifiedPatchClient
                ? unifiedPatchClient.UpdateReportedPropertiesAsync(new System.Text.Json.Nodes.JsonObject { ["a"] = 1 }, TestContext.Current.CancellationToken)
                : ((Device.MQTTv5.Twin.TwinClient)twinClient).UpdateReportedPropertiesAsync(
                    new ReportedPatchRequest { ReportedProperties = new System.Text.Json.Nodes.JsonObject { ["a"] = 1 } }, TestContext.Current.CancellationToken);
            MqttPublish patchRequest = await WaitForSentPublishAsync(mockMqttClient, "reported-patch:1");
            Assert.Equal(expectedTopic, patchRequest.Topic);
            _ = patch;
        }

        [Theory]
        [InlineData(false, ServiceBoundTopicFormat.PerDevice)]
        [InlineData(true, ServiceBoundTopicFormat.PerDevice)]
        [InlineData(false, ServiceBoundTopicFormat.Legacy)]
        [InlineData(true, ServiceBoundTopicFormat.Legacy)]
        public async Task TelemetryTopicDoesNotDependOnFormat(bool useUnifiedClient, ServiceBoundTopicFormat format)
        {
            using MockConnectionMqttClient mockMqttClient = new() { AutoRespondToBirth = true };
            using IDisposable connectionClient = CreateConnectionClient(useUnifiedClient, format, mockMqttClient);

            await ConnectAsync((AbstractConnectionClient)connectionClient);

            DeviceToCloudTelemetry telemetry = new() { Payload = Encoding.UTF8.GetBytes("hello") };
            if (useUnifiedClient)
            {
                using Device.Unified.Telemetry.TelemetryClient telemetryClient = new((UnifiedConnectionClient)connectionClient);
                await telemetryClient.SendTelemetryAsync(telemetry, TestContext.Current.CancellationToken);
            }
            else
            {
                using Device.MQTTv5.Telemetry.TelemetryClient telemetryClient = new((Mqttv5ConnectionClient)connectionClient);
                await telemetryClient.SendTelemetryAsync(telemetry, TestContext.Current.CancellationToken);
            }

            MqttPublish sent = Assert.Single(SentPublishes(mockMqttClient), publish => publish.Topic.EndsWith("/srv/telemetry", StringComparison.Ordinal));
            Assert.Equal("ih/" + DeviceId + "/srv/telemetry", sent.Topic);
        }

        private static IDisposable CreateConnectionClient(bool useUnifiedClient, ServiceBoundTopicFormat format, MockConnectionMqttClient mockMqttClient)
        {
            return useUnifiedClient
                ? new UnifiedConnectionClient(new UnifiedConnectionClientOptions { MqttClient = mockMqttClient, ServiceBoundTopicFormat = format })
                : new Mqttv5ConnectionClient(new Mqttv5ConnectionClientOptions { MqttClient = mockMqttClient, ServiceBoundTopicFormat = format });
        }

        private static Device.Unified.DirectMethods.DirectMethodClient CreateUnifiedDirectMethodClient(
            UnifiedConnectionClient connectionClient,
            Func<DirectMethodRequestReceivedEventArgs, Task<DirectMethodResponse>> handler)
        {
            Device.Unified.DirectMethods.DirectMethodClient directMethodClient = new(connectionClient);
            directMethodClient.DirectMethodInvokedAsync += handler;
            return directMethodClient;
        }

        private static Device.MQTTv5.DirectMethods.DirectMethodClient CreateMqttv5DirectMethodClient(
            Mqttv5ConnectionClient connectionClient,
            Func<DirectMethodRequestReceivedEventArgs, Task<DirectMethodResponse>> handler)
        {
            Device.MQTTv5.DirectMethods.DirectMethodClient directMethodClient = new(connectionClient);
            directMethodClient.DirectMethodProbeReceivedAsync += _ => Task.FromResult(DirectMethodProbeAck.Accepted());
            directMethodClient.DirectMethodInvokedAsync += handler;
            return directMethodClient;
        }

        private static async Task ConnectAsync(AbstractConnectionClient connectionClient)
        {
            TaskCompletionSource presenceEstablished = new(TaskCreationOptions.RunContinuationsAsynchronously);
            connectionClient.DevicePresenceFlowCompletedAsync += _ =>
            {
                presenceEstablished.TrySetResult();
                return Task.CompletedTask;
            };

            await connectionClient.ConnectToHubAsync(CreateConnectionContext(), TestContext.Current.CancellationToken)
                .WaitAsync(s_testTimeout, TestContext.Current.CancellationToken);
            await presenceEstablished.Task.WaitAsync(s_testTimeout, TestContext.Current.CancellationToken);
        }

        private static ConnectionContext CreateConnectionContext()
        {
#pragma warning disable SYSLIB0026 // A mock certificate; no need to load a real one.
            return new ConnectionContext()
            {
                AuthenticationProvider = new X509AuthenticationProvider(new System.Security.Cryptography.X509Certificates.X509Certificate2(), null),
                DeviceId = DeviceId,
                IotHubHostName = "someHostName",
                ConnectionProfile = IotHubConnectionType.Mqttv5,
            };
#pragma warning restore SYSLIB0026
        }

        private static MqttPublish CreateInboundMethodPublish(string type, byte[] correlationData, byte[] payload)
        {
            MqttPublish publish = new()
            {
                Topic = "ih/" + DeviceId + "/dev/methods",
                Payload = payload,
                CorrelationData = correlationData,
                MessageExpiryInterval = 30,
                QualityOfServiceLevel = MqttQualityOfServiceLevel.AtLeastOnce,
            };
            publish.UserProperties.Add(new("type", Encoding.UTF8.GetBytes(type)));
            return publish;
        }

        private static IEnumerable<MqttPublish> SentPublishes(MockConnectionMqttClient mockMqttClient)
        {
            return mockMqttClient.SentMqttTrafficInOrder.Select(traffic => traffic.Publish).OfType<MqttPublish>();
        }

        private static async Task<MqttPublish> WaitForSentPublishAsync(MockConnectionMqttClient mockMqttClient, string type)
        {
            using CancellationTokenSource timeout = CancellationTokenSource.CreateLinkedTokenSource(TestContext.Current.CancellationToken);
            timeout.CancelAfter(s_testTimeout);
            while (true)
            {
                MqttPublish? publish = SentPublishes(mockMqttClient).FirstOrDefault(publish => GetType(publish) == type);
                if (publish != null)
                {
                    return publish;
                }

                await Task.Delay(10, timeout.Token);
            }
        }

        private static string? GetType(MqttPublish publish)
        {
            return publish.UserProperties.TryGetProperty("type", out string? type) ? type : null;
        }
    }
}
