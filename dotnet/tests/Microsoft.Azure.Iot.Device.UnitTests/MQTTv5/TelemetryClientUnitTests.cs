// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Exceptions;
using Microsoft.Azure.Iot.Device.MQTTv5.Connection;
using Microsoft.Azure.Iot.Device.MQTTv5.Telemetry;
using Microsoft.Azure.Iot.Device.Models.Telemetry;
using Microsoft.Azure.Iot.Device.Mqtt;
using Microsoft.Azure.Iot.Device.Models;
using System.Text;
using Xunit;

namespace Microsoft.Azure.Iot.Device.UnitTests.MQTTv5
{
    public class TelemetryClientUnitTests
    {
        private const string DeviceId = "someDeviceId";

        [Fact]
        public async Task SendTelemetryAsync_PublishesToMqttv5TopicWithProperties()
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv5, DeviceId),
            };
            using TelemetryClient telemetryClient = new(connection);

            DeviceToCloudTelemetry message = new()
            {
                Payload = Encoding.UTF8.GetBytes("hello"),
                MessageId = "message-1",
                CorrelationId = "correlation-1",
                ContentType = "application/json",
                ContentEncoding = "utf-8",
            };
            message.UserProperties.Add("customKey", "customValue");

            await telemetryClient.SendTelemetryAsync(message, TestContext.Current.CancellationToken);

            MqttPublish publish = Assert.Single(connection.PublishedMessages);
            // The device ID is left for the connection client to fill in when it actually sends the publish.
            Assert.Equal($"ih/{IConnectionClient.DeviceIdPlaceholder}/srv/telemetry", publish.Topic);
            Assert.Equal(MqttQualityOfServiceLevel.AtLeastOnce, publish.QualityOfServiceLevel);
            Assert.Equal(Encoding.UTF8.GetBytes("hello"), publish.Payload);
            Assert.Equal("application/json", publish.ContentType);
            Assert.Equal("correlation-1", Encoding.UTF8.GetString(publish.CorrelationData!));

            Assert.Equal("message-1", GetUserProperty(publish, TelemetryClient.MessagePropertyMessageId));
            Assert.Equal("utf-8", GetUserProperty(publish, TelemetryClient.MessagePropertyContentEncoding));
            Assert.Equal("customValue", GetUserProperty(publish, "customKey"));
        }

        [Fact]
        public async Task SendTelemetryAsync_ThrowsWhenPubackRejectsTelemetry()
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv5, DeviceId),
                OnPublish = _ => Task.FromResult(new MqttPublishAck() { ReasonCode = MqttPublishAckReasonCode.QuotaExceeded }),
            };
            using TelemetryClient telemetryClient = new(connection);

            DeviceException exception = await Assert.ThrowsAsync<DeviceException>(
                () => telemetryClient.SendTelemetryAsync(new DeviceToCloudTelemetry() { Payload = Encoding.UTF8.GetBytes("hello") }, TestContext.Current.CancellationToken));

            Assert.Equal(ErrorRetryability.Retryable, exception.Retryability);
        }

        [Fact]
        public async Task SendTelemetryAsync_ThrowsWhenDisconnected()
        {
            MockFeatureConnectionClient connection = new(); // Never connected -> null context
            using TelemetryClient telemetryClient = new(connection);

            await Assert.ThrowsAsync<NotSupportedException>(
                async () => await telemetryClient.SendTelemetryAsync(new DeviceToCloudTelemetry(), TestContext.Current.CancellationToken));

            Assert.Empty(connection.PublishedMessages);
        }

        [Fact]
        public async Task SendTelemetryAsync_ThrowsAfterDisconnect()
        {
            using MockConnectionMqttClient mqttClient = new() { AutoRespondToBirth = true };
            using ConnectionClient connection = new(new() { MqttClient = mqttClient });
            await connection.ConnectToHubAsync(
                MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv5, DeviceId),
                TestContext.Current.CancellationToken);
            using TelemetryClient telemetryClient = new(connection);

            await connection.DisconnectAsync(null, TestContext.Current.CancellationToken);

            await Assert.ThrowsAsync<NotSupportedException>(
                async () => await telemetryClient.SendTelemetryAsync(new DeviceToCloudTelemetry(), TestContext.Current.CancellationToken));
        }

        [Fact]
        public async Task SendTelemetryAsync_ThrowsWhenMessageTooLarge()
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv5, DeviceId),
            };
            using TelemetryClient telemetryClient = new(connection);

            DeviceToCloudTelemetry message = new()
            {
                Payload = new byte[255001],
            };

            await Assert.ThrowsAsync<MessageTooLargeException>(
                async () => await telemetryClient.SendTelemetryAsync(message, TestContext.Current.CancellationToken));

            Assert.Empty(connection.PublishedMessages);
        }

        [Fact]
        public async Task SendTelemetryAsync_ThrowsAfterDispose()
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv5, DeviceId),
            };
            TelemetryClient telemetryClient = new(connection);
            telemetryClient.Dispose();

            await Assert.ThrowsAsync<ObjectDisposedException>(
                async () => await telemetryClient.SendTelemetryAsync(new DeviceToCloudTelemetry(), TestContext.Current.CancellationToken));
        }

        private static string GetUserProperty(MqttPublish publish, string key)
        {
            MqttUserProperty property = Assert.Single(publish.UserProperties, p => p.Name == key);
            return property.ReadValueAsString();
        }
    }
}
