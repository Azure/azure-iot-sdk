// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Exceptions;
using Microsoft.Azure.Iot.Device.Models.Telemetry;
using Microsoft.Azure.Iot.Device.Mqtt;
using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Provisioning.Models;
using Microsoft.Azure.Iot.Device.Unified.Connection;
using Microsoft.Azure.Iot.Device.Unified.Telemetry;
using System.Text;
using Xunit;

namespace Microsoft.Azure.Iot.Device.UnitTests.Unified
{
    public class TelemetryClientUnitTests
    {
        private const string DeviceId = "someDeviceId";

        [Fact]
        public async Task CloudToDeviceTelemetryReceivedAsync_DecodesPropertiesAndDeliversPayload()
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv3, DeviceId),
            };
            using TelemetryClient telemetryClient = new(connection);
            List<CloudToDeviceTelemetry> receivedMessages = new();
            telemetryClient.CloudToDeviceTelemetryReceivedAsync += message =>
            {
                receivedMessages.Add(message);
                return Task.CompletedTask;
            };
            byte[] payload = Encoding.UTF8.GetBytes("hello");
            MockMqttPublishReceivedEventArgs args = new()
            {
                Publish = new MqttPublish
                {
                    Topic = $"devices/{DeviceId}/messages/devicebound/"
                        + "%24.mid=message%2F1&%24.cid=correlation%201&%24.ct=application%2Fjson&%24.ce=utf-8"
                        + "&custom%20key=value%26with%3Ddelimiters&empty=&flag&equals=a=b&plus=%2B",
                    Payload = payload,
                    QualityOfServiceLevel = MqttQualityOfServiceLevel.AtLeastOnce,
                },
            };

            await connection.SimulateReceiveAsync(args);

            CloudToDeviceTelemetry message = Assert.Single(receivedMessages);
            Assert.Equal(payload, message.Payload);
            Assert.Equal("message/1", message.MessageId);
            Assert.Equal("correlation 1", message.CorrelationId);
            Assert.Equal("application/json", message.ContentType);
            Assert.Equal("utf-8", message.ContentEncoding);
            Assert.Equal(5, message.UserProperties.Count);
            Assert.Equal("value&with=delimiters", message.UserProperties["custom key"]);
            Assert.Equal("", message.UserProperties["empty"]);
            Assert.Equal("", message.UserProperties["flag"]);
            Assert.Equal("a=b", message.UserProperties["equals"]);
            Assert.Equal("+", message.UserProperties["plus"]);
            Assert.False(args.IsAcknowledged);
        }

        [Theory]
        [InlineData("")]
        [InlineData("$.mid=message-1&$.cid=correlation-1&$.ct=text%2Fplain&$.ce=utf-8")]
        public async Task CloudToDeviceTelemetryReceivedAsync_HandlesOptionalPropertyBag(string propertyBag)
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv3, DeviceId),
            };
            using TelemetryClient telemetryClient = new(connection);
            List<CloudToDeviceTelemetry> receivedMessages = new();
            telemetryClient.CloudToDeviceTelemetryReceivedAsync += message =>
            {
                receivedMessages.Add(message);
                return Task.CompletedTask;
            };
            MockMqttPublishReceivedEventArgs args = new()
            {
                Publish = new MqttPublish
                {
                    Topic = $"devices/{DeviceId}/messages/devicebound/{propertyBag}",
                    Payload = Array.Empty<byte>(),
                    QualityOfServiceLevel = MqttQualityOfServiceLevel.AtLeastOnce,
                },
            };

            await connection.SimulateReceiveAsync(args);

            CloudToDeviceTelemetry message = Assert.Single(receivedMessages);
            Assert.Empty(message.Payload);
            Assert.Equal(propertyBag.Length == 0 ? null : "message-1", message.MessageId);
            Assert.Equal(propertyBag.Length == 0 ? null : "correlation-1", message.CorrelationId);
            Assert.Equal(propertyBag.Length == 0 ? null : "text/plain", message.ContentType);
            Assert.Equal(propertyBag.Length == 0 ? null : "utf-8", message.ContentEncoding);
            Assert.Empty(message.UserProperties);
            Assert.False(args.IsAcknowledged);
        }

        [Theory]
        [InlineData("$dps/registrations/res/200/?$rid=1", false)]
        [InlineData("$iothub/twin/res/200/?$rid=1", true)]
        [InlineData("other/someDeviceId/messages/devicebound/", true)]
        [InlineData("devices/someDeviceId/messages/events/", true)]
        [InlineData("devices/anotherDevice/messages/devicebound/", true)]
        [InlineData("devices/someDeviceIdSuffix/messages/devicebound/", true)]
        [InlineData("devices/someDeviceId/messages/devicebound/", false)]
        public async Task CloudToDeviceTelemetryReceivedAsync_IgnoresUnrelatedOrDisconnectedPublishes(string topic, bool connected)
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = connected
                    ? MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv3, DeviceId)
                    : null,
            };
            using TelemetryClient telemetryClient = new(connection);
            List<CloudToDeviceTelemetry> receivedMessages = new();
            telemetryClient.CloudToDeviceTelemetryReceivedAsync += message =>
            {
                receivedMessages.Add(message);
                return Task.CompletedTask;
            };
            MockMqttPublishReceivedEventArgs args = new()
            {
                Publish = new MqttPublish { Topic = topic, QualityOfServiceLevel = MqttQualityOfServiceLevel.AtLeastOnce },
            };

            await connection.SimulateReceiveAsync(args);

            Assert.Empty(receivedMessages);
            Assert.False(args.IsAcknowledged);
        }

        [Fact]
        public async Task CloudToDeviceTelemetryReceivedAsync_UsesCurrentDeviceIdAfterConnectionChanges()
        {
            MockFeatureConnectionClient connection = new();
            using TelemetryClient telemetryClient = new(connection);
            List<CloudToDeviceTelemetry> receivedMessages = new();
            telemetryClient.CloudToDeviceTelemetryReceivedAsync += message =>
            {
                receivedMessages.Add(message);
                return Task.CompletedTask;
            };

            foreach (string deviceId in new[] { DeviceId, "reassignedDeviceId" })
            {
                connection.CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv3, deviceId);
                await connection.SimulateReceiveAsync(new MqttPublish
                {
                    Topic = $"devices/{deviceId}/messages/devicebound/",
                    Payload = Encoding.UTF8.GetBytes(deviceId),
                });
            }

            await connection.SimulateReceiveAsync(new MqttPublish { Topic = $"devices/{DeviceId}/messages/devicebound/" });

            Assert.Collection(receivedMessages,
                message => Assert.Equal(Encoding.UTF8.GetBytes(DeviceId), message.Payload),
                message => Assert.Equal(Encoding.UTF8.GetBytes("reassignedDeviceId"), message.Payload));
        }

        [Fact]
        public async Task CloudToDeviceTelemetryReceivedAsync_AwaitsHandlerWithoutAcknowledging()
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv3, DeviceId),
            };
            using TelemetryClient telemetryClient = new(connection);
            TaskCompletionSource handlerStarted = new(TaskCreationOptions.RunContinuationsAsynchronously);
            TaskCompletionSource completeHandler = new(TaskCreationOptions.RunContinuationsAsynchronously);
            telemetryClient.CloudToDeviceTelemetryReceivedAsync += async _ =>
            {
                handlerStarted.SetResult();
                await completeHandler.Task;
            };
            MockMqttPublishReceivedEventArgs args = new()
            {
                Publish = new MqttPublish
                {
                    Topic = $"devices/{DeviceId}/messages/devicebound/",
                    QualityOfServiceLevel = MqttQualityOfServiceLevel.AtLeastOnce,
                },
            };

            Task receiveTask = connection.SimulateReceiveAsync(args);
            try
            {
                await handlerStarted.Task.WaitAsync(TestContext.Current.CancellationToken);
                Assert.False(args.IsAcknowledged);
                Assert.False(receiveTask.IsCompleted);
            }
            finally
            {
                completeHandler.TrySetResult();
                await receiveTask.WaitAsync(TestContext.Current.CancellationToken);
            }

            Assert.False(args.IsAcknowledged);
        }

        [Fact]
        public async Task CloudToDeviceTelemetryReceivedAsync_AwaitsEveryHandlerWithoutAcknowledging()
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv3, DeviceId),
            };
            using TelemetryClient telemetryClient = new(connection);
            TaskCompletionSource completeFirstHandler = new(TaskCreationOptions.RunContinuationsAsynchronously);
            bool secondHandlerRan = false;
            telemetryClient.CloudToDeviceTelemetryReceivedAsync += async _ => await completeFirstHandler.Task;
            telemetryClient.CloudToDeviceTelemetryReceivedAsync += _ =>
            {
                secondHandlerRan = true;
                return Task.CompletedTask;
            };
            MockMqttPublishReceivedEventArgs args = new()
            {
                Publish = new MqttPublish
                {
                    Topic = $"devices/{DeviceId}/messages/devicebound/",
                    QualityOfServiceLevel = MqttQualityOfServiceLevel.AtLeastOnce,
                },
            };

            Task receiveTask = connection.SimulateReceiveAsync(args);
            try
            {
                Assert.False(args.IsAcknowledged);
                Assert.False(receiveTask.IsCompleted);
            }
            finally
            {
                completeFirstHandler.TrySetResult();
                await receiveTask.WaitAsync(TestContext.Current.CancellationToken);
            }

            Assert.True(secondHandlerRan);
            Assert.False(args.IsAcknowledged);
        }

        [Fact]
        public async Task CloudToDeviceTelemetryReceivedAsync_PropagatesHandlerFailureWithoutAcknowledging()
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv3, DeviceId),
            };
            using TelemetryClient telemetryClient = new(connection);
            InvalidOperationException failure = new("Handler failed.");
            telemetryClient.CloudToDeviceTelemetryReceivedAsync += _ => Task.FromException(failure);
            MockMqttPublishReceivedEventArgs args = new()
            {
                Publish = new MqttPublish
                {
                    Topic = $"devices/{DeviceId}/messages/devicebound/",
                    QualityOfServiceLevel = MqttQualityOfServiceLevel.AtLeastOnce,
                },
            };

            InvalidOperationException actual = await Assert.ThrowsAsync<InvalidOperationException>(() => connection.SimulateReceiveAsync(args));

            Assert.Same(failure, actual);
            Assert.False(args.IsAcknowledged);
        }

        [Fact]
        public async Task CloudToDeviceTelemetryReceivedAsync_IgnoresMessagesWithoutHandler()
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv3, DeviceId),
            };
            using TelemetryClient telemetryClient = new(connection);
            MockMqttPublishReceivedEventArgs args = new()
            {
                Publish = new MqttPublish
                {
                    Topic = $"devices/{DeviceId}/messages/devicebound/",
                    QualityOfServiceLevel = MqttQualityOfServiceLevel.AtLeastOnce,
                },
            };

            await connection.SimulateReceiveAsync(args);

            Assert.False(args.IsAcknowledged);
        }

        [Theory]
        [InlineData(false, false)]
        [InlineData(true, false)]
        [InlineData(true, true)]
        public async Task CloudToDeviceMessagesAreAcknowledgedByConnectionRegardlessOfHandler(bool hasHandler, bool handlerThrows)
        {
            using MockConnectionMqttClient mqttClient = new();
            using ConnectionClient connection = new(new() { MqttClient = mqttClient });
            using TelemetryClient telemetryClient = new(connection);
            await connection.ConnectToHubAsync(
                MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv3, DeviceId),
                TestContext.Current.CancellationToken);
            MockMqttPublishReceivedEventArgs args = new()
            {
                Publish = new()
                {
                    Topic = $"devices/{DeviceId}/messages/devicebound/",
                    QualityOfServiceLevel = MqttQualityOfServiceLevel.AtLeastOnce,
                },
            };
            bool handlerRan = false;
            int acknowledgementsAtHandlerStart = 0;
            if (hasHandler)
            {
                telemetryClient.CloudToDeviceTelemetryReceivedAsync += _ =>
                {
                    handlerRan = true;
                    acknowledgementsAtHandlerStart = args.AcknowledgeCount;
                    return handlerThrows
                        ? Task.FromException(new InvalidOperationException("Handler failed."))
                        : Task.CompletedTask;
                };
            }

            await mqttClient.SimulatePublishReceivedEventAsync(args);

            Assert.Equal(hasHandler, handlerRan);
            Assert.Equal(hasHandler ? 1 : 0, acknowledgementsAtHandlerStart);
            Assert.Equal(1, args.AcknowledgeCount);
        }

        [Fact]
        public async Task CloudToDeviceTelemetryReceivedAsync_UnsubscribesOnDispose()
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv3, DeviceId),
            };
            TelemetryClient telemetryClient = new(connection);
            List<CloudToDeviceTelemetry> receivedMessages = new();
            telemetryClient.CloudToDeviceTelemetryReceivedAsync += message =>
            {
                receivedMessages.Add(message);
                return Task.CompletedTask;
            };
            MockMqttPublishReceivedEventArgs args = new()
            {
                Publish = new MqttPublish { Topic = $"devices/{DeviceId}/messages/devicebound/" },
            };
            telemetryClient.Dispose();

            await connection.SimulateReceiveAsync(args);

            Assert.Empty(receivedMessages);
            Assert.False(args.IsAcknowledged);
        }

        [Fact]
        public async Task SendTelemetryAsync_Classic_PublishesToEventsTopicWithPropertyBag()
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv3, DeviceId),
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
            // The device ID is left for the connection client to fill in when it actually sends the publish.
            Assert.StartsWith($"devices/{IConnectionClient.DeviceIdPlaceholder}/messages/events/", publish.Topic);
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
        public async Task SendTelemetryAsync_Classic_ThrowsWhenMessageTooLarge()
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv3, DeviceId),
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
        public async Task SendTelemetryAsync_ThrowsAfterDisconnect()
        {
            MockConnectionMqttClient mqttClient = new();
            using ConnectionClient connection = new(new() { MqttClient = mqttClient });
            await connection.ConnectToHubAsync(
                MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv3, DeviceId),
                TestContext.Current.CancellationToken);
            using TelemetryClient telemetryClient = new(connection);

            await connection.DisconnectAsync(TestContext.Current.CancellationToken);

            await Assert.ThrowsAsync<NotSupportedException>(
                async () => await telemetryClient.SendTelemetryAsync(new DeviceToCloudTelemetry(), TestContext.Current.CancellationToken));
        }

        [Fact]
        public async Task SendTelemetryAsync_ThrowsAfterDispose()
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv3, DeviceId),
            };
            TelemetryClient telemetryClient = new(connection);
            telemetryClient.Dispose();

            await Assert.ThrowsAsync<ObjectDisposedException>(
                async () => await telemetryClient.SendTelemetryAsync(new DeviceToCloudTelemetry(), TestContext.Current.CancellationToken));
        }
    }
}
