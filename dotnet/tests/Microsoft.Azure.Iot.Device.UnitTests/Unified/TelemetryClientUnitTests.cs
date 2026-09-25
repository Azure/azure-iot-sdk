// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Exceptions;
using Microsoft.Azure.Iot.Device.Models.Telemetry;
using Microsoft.Azure.Iot.Device.Mqtt;
using Microsoft.Azure.Iot.Device.Provisioning.Models;
using Microsoft.Azure.Iot.Device.Unified.Telemetry;
using System.Text;
using Xunit;

namespace Microsoft.Azure.Iot.Device.UnitTests.Unified
{
    public class TelemetryClientUnitTests
    {
        private const string DeviceId = "someDeviceId";

        [Fact]
        public async Task SendTelemetryAsync_Classic_PublishesToEventsTopicWithPropertyBag()
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(ConnectionProfile.Classic, DeviceId),
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
            message.UserProperties.Add("customKey", "custom value");

            await telemetryClient.SendTelemetryAsync(message, TestContext.Current.CancellationToken);

            MqttPublish publish = Assert.Single(connection.PublishedMessages);
            Assert.StartsWith($"devices/{DeviceId}/messages/events/", publish.Topic);
            Assert.Equal(MqttQualityOfServiceLevel.AtLeastOnce, publish.QualityOfServiceLevel);
            Assert.Equal(Encoding.UTF8.GetBytes("hello"), publish.Payload);

            // Classic hubs carry all message properties URL-encoded in the topic string.
            Assert.Contains($"&{TelemetryClient.MessagePropertyMessageId}=message-1", publish.Topic);
            Assert.Contains($"&{TelemetryClient.MessagePropertyCorrelationId}=correlation-1", publish.Topic);
            Assert.Contains($"&{TelemetryClient.MessagePropertyContentType}={Uri.EscapeDataString("application/json")}", publish.Topic);
            Assert.Contains($"&{TelemetryClient.MessagePropertyContentEncoding}=utf-8", publish.Topic);
            Assert.Contains($"&customKey={Uri.EscapeDataString("custom value")}", publish.Topic);
        }

        [Fact]
        public async Task SendTelemetryAsync_MqttV5_PublishesToMqttv5Topic()
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(ConnectionProfile.MqttV5, DeviceId),
            };
            using TelemetryClient telemetryClient = new(connection);

            DeviceToCloudTelemetry message = new()
            {
                Payload = Encoding.UTF8.GetBytes("hello"),
                MessageId = "message-1",
            };

            await telemetryClient.SendTelemetryAsync(message, TestContext.Current.CancellationToken);

            MqttPublish publish = Assert.Single(connection.PublishedMessages);
            Assert.Equal($"ih/{DeviceId}/srv/telemetry", publish.Topic);
            Assert.Equal(Encoding.UTF8.GetBytes("hello"), publish.Payload);
            Assert.Contains(publish.UserProperties, p => p.Name == TelemetryClient.MessagePropertyMessageId && p.ReadValueAsString() == "message-1");
        }

        [Fact]
        public async Task CloudToDeviceTelemetryReceivedAsync_RaisedOnDeviceBoundMessage()
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(ConnectionProfile.Classic, DeviceId),
            };
            using TelemetryClient telemetryClient = new(connection);

            CloudToDeviceTelemetry? received = null;
            telemetryClient.CloudToDeviceTelemetryReceivedAsync += (message) =>
            {
                received = message;
                return Task.CompletedTask;
            };

            string propertyBag = $"{Uri.EscapeDataString(TelemetryClient.MessagePropertyMessageId)}=msg-1&color=blue";
            await connection.SimulateReceiveAsync(new MqttPublish()
            {
                Topic = $"devices/{DeviceId}/messages/devicebound/{propertyBag}",
                Payload = Encoding.UTF8.GetBytes("cloud payload"),
                QualityOfServiceLevel = MqttQualityOfServiceLevel.AtLeastOnce,
            });

            Assert.NotNull(received);
            Assert.Equal(Encoding.UTF8.GetBytes("cloud payload"), received!.Payload);
            Assert.Equal("msg-1", received.MessageId);
            Assert.Equal("blue", received.UserProperties["color"]);
        }

        [Fact]
        public async Task SendTelemetryAsync_Classic_ThrowsWhenMessageTooLarge()
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(ConnectionProfile.Classic, DeviceId),
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
        public async Task SendTelemetryAsync_ThrowsWhenDisconnected()
        {
            MockFeatureConnectionClient connection = new();
            using TelemetryClient telemetryClient = new(connection);

            await Assert.ThrowsAsync<NotSupportedException>(
                async () => await telemetryClient.SendTelemetryAsync(new DeviceToCloudTelemetry(), TestContext.Current.CancellationToken));
        }

        [Fact]
        public async Task SendTelemetryAsync_ThrowsAfterDispose()
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(ConnectionProfile.Classic, DeviceId),
            };
            TelemetryClient telemetryClient = new(connection);
            telemetryClient.Dispose();

            await Assert.ThrowsAsync<ObjectDisposedException>(
                async () => await telemetryClient.SendTelemetryAsync(new DeviceToCloudTelemetry(), TestContext.Current.CancellationToken));
        }
    }
}
