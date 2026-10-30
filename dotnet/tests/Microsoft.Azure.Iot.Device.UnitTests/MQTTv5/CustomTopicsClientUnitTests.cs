// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Exceptions;
using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Mqtt;
using Microsoft.Azure.Iot.Device.MQTTv5.Connection;
using Microsoft.Azure.Iot.Device.MQTTv5.CustomTopics;
using Microsoft.Azure.Iot.Device.Retry;
using Xunit;

namespace Microsoft.Azure.Iot.Device.UnitTests.MQTTv5
{
    public class CustomTopicsClientUnitTests
    {
        private const string DeviceId = "someDeviceId";

        [Fact]
        public async Task PublishForwardsMessageAndDoesNotAcknowledgeInboundTraffic()
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv5, DeviceId),
            };
            using CustomTopicsClient customTopicsClient = new(connection);
            MqttPublish publish = new()
            {
                Topic = $"custom/{IConnectionClient.DeviceIdPlaceholder}",
                Payload = new byte[] { 1, 2, 3 },
                QualityOfServiceLevel = MqttQualityOfServiceLevel.AtLeastOnce,
            };

            await customTopicsClient.PublishAsync(publish, TestContext.Current.CancellationToken);
            MockMqttPublishReceivedEventArgs received = await connection.SimulateReceiveAsync(publish);

            Assert.Same(publish, Assert.Single(connection.PublishedMessages));
            Assert.Empty(connection.SubscribedMessages);
            Assert.Empty(connection.UnsubscribedMessages);
            Assert.Equal(0, received.AcknowledgeCount);
        }

        [Fact]
        public async Task PublishThrowsAfterDisconnect()
        {
            using MockConnectionMqttClient mqttClient = new() { AutoRespondToBirth = true };
            using ConnectionClient connection = new(new() { MqttClient = mqttClient });
            await connection.ConnectToHubAsync(
                MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv5, DeviceId),
                TestContext.Current.CancellationToken);
            using CustomTopicsClient customTopicsClient = new(connection);

            await connection.DisconnectAsync(TestContext.Current.CancellationToken);

            await Assert.ThrowsAsync<NotSupportedException>(
                () => customTopicsClient.PublishAsync(new MqttPublish() { Topic = "topic" }, TestContext.Current.CancellationToken));
        }

        [Fact]
        public async Task PublishThrowsWhenNeverConnected()
        {
            MockFeatureConnectionClient connection = new();
            using CustomTopicsClient customTopicsClient = new(connection);

            await Assert.ThrowsAsync<NotSupportedException>(
                () => customTopicsClient.PublishAsync(new MqttPublish() { Topic = "topic" }, TestContext.Current.CancellationToken));
            Assert.Empty(connection.PublishedMessages);
        }

        [Fact]
        public async Task CanceledPublishIsNotSent()
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv5, DeviceId),
            };
            using CustomTopicsClient customTopicsClient = new(connection);
            using CancellationTokenSource cancellation = new();
            cancellation.Cancel();

            await Assert.ThrowsAnyAsync<OperationCanceledException>(
                () => customTopicsClient.PublishAsync(new MqttPublish() { Topic = "topic" }, cancellation.Token));
            Assert.Empty(connection.PublishedMessages);
        }

        [Theory]
        [InlineData(false)]
        [InlineData(true)]
        public async Task DisposedClientRejectsPublishes(bool disposeConnection)
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv5, DeviceId),
            };
            CustomTopicsClient customTopicsClient = new(connection);

            customTopicsClient.Dispose(disposeConnection);
            customTopicsClient.Dispose();

            await Assert.ThrowsAsync<ObjectDisposedException>(
                () => customTopicsClient.PublishAsync(new MqttPublish() { Topic = "topic" }, TestContext.Current.CancellationToken));
            Assert.Empty(connection.PublishedMessages);
            Assert.Equal(disposeConnection, connection.IsDisposed);
        }

        private static ConnectionClient CreateReconnectingConnectionClient(MockConnectionMqttClient mockMqttClient)
        {
            return new(new()
            {
                MqttClient = mockMqttClient,
                ConnectionRetryPolicy = new ExponentialBackoffRetryPolicy(uint.MaxValue, TimeSpan.Zero, useJitter: false),
            });
        }

        private static ConnectionContext CreateHubConnectionContext()
        {
            return MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv5, DeviceId);
        }

        [Fact]
        public async Task ErrorReasonCodesAreReturnedAsIsRatherThanThrown()
        {
            const string topic = "custom/topic";

            using MockConnectionMqttClient mockMqttClient = new() { AutoRespondToBirth = true };
            using ConnectionClient connectionClient = CreateReconnectingConnectionClient(mockMqttClient);
            using CustomTopicsClient customTopicsClient = new(connectionClient);

            await connectionClient.ConnectToHubAsync(CreateHubConnectionContext(), TestContext.Current.CancellationToken);

            mockMqttClient.OnPublish = publish => Task.FromResult(new MqttPublishAck()
            {
                ReasonCode = publish.Topic == topic ? MqttPublishAckReasonCode.NotAuthorized : MqttPublishAckReasonCode.Success,
            });

            MqttPublishAck puback = await customTopicsClient.PublishAsync(
                new MqttPublish() { Topic = topic, QualityOfServiceLevel = MqttQualityOfServiceLevel.AtLeastOnce },
                TestContext.Current.CancellationToken);
            Assert.Equal(MqttPublishAckReasonCode.NotAuthorized, puback.ReasonCode);
        }

        // Connection-level failures are still the SDK's to handle: a publish the connection loss interrupts is re-sent
        // once the device is present again, and only the eventual MQTT-level result is returned.
        [Fact]
        public async Task PublishInterruptedByConnectionLossIsResentAndItsErrorReasonCodeReturned()
        {
            const string topic = "custom/topic";

            using MockConnectionMqttClient mockMqttClient = new() { AutoRespondToBirth = true };
            using ConnectionClient connectionClient = CreateReconnectingConnectionClient(mockMqttClient);
            using CustomTopicsClient customTopicsClient = new(connectionClient);

            await connectionClient.ConnectToHubAsync(CreateHubConnectionContext(), TestContext.Current.CancellationToken);
            mockMqttClient.OnConnect = _ => Task.FromResult(new MqttConnectAck()
            {
                ResultCode = MqttConnectReasonCode.Success,
                IsSessionPresent = true,
            });

            int publishAttempts = 0;
            mockMqttClient.OnPublish = publish =>
            {
                if (publish.Topic != topic)
                {
                    return Task.FromResult(new MqttPublishAck() { ReasonCode = MqttPublishAckReasonCode.Success });
                }

                if (Interlocked.Increment(ref publishAttempts) == 1)
                {
                    _ = mockMqttClient.SimulateServerDisconnectAsync(MqttDisconnectReason.ImplementationSpecificError, new IOException("mock connection reset"));
                    throw new IOException("mock connection reset");
                }

                return Task.FromResult(new MqttPublishAck() { ReasonCode = MqttPublishAckReasonCode.QuotaExceeded });
            };

            MqttPublishAck puback = await customTopicsClient
                .PublishAsync(new MqttPublish() { Topic = topic, QualityOfServiceLevel = MqttQualityOfServiceLevel.AtLeastOnce }, TestContext.Current.CancellationToken)
                .WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);

            Assert.Equal(MqttPublishAckReasonCode.QuotaExceeded, puback.ReasonCode);
            Assert.Equal(2, publishAttempts);
        }

        [Fact]
        public async Task PublishFailureWithoutConnectionLossThrowsRetryableDeviceExceptionWithoutBeingResent()
        {
            using MockConnectionMqttClient mockMqttClient = new() { AutoRespondToBirth = true };
            using ConnectionClient connectionClient = CreateReconnectingConnectionClient(mockMqttClient);
            using CustomTopicsClient customTopicsClient = new(connectionClient);

            await connectionClient.ConnectToHubAsync(CreateHubConnectionContext(), TestContext.Current.CancellationToken);

            int publishAttempts = 0;
            Exception connectionDropped = new MQTTnet.Exceptions.MqttClientDisconnectedException(new Exception("mock connection loss"));
            mockMqttClient.OnPublish = publish =>
            {
                if (publish.Topic != "custom/topic")
                {
                    return Task.FromResult(new MqttPublishAck() { ReasonCode = MqttPublishAckReasonCode.Success });
                }

                Interlocked.Increment(ref publishAttempts);
                throw connectionDropped;
            };

            DeviceException publishException = await Assert.ThrowsAsync<DeviceException>(() => customTopicsClient.PublishAsync(
                new MqttPublish() { Topic = "custom/topic", QualityOfServiceLevel = MqttQualityOfServiceLevel.AtLeastOnce },
                TestContext.Current.CancellationToken));

            Assert.Equal(ErrorRetryability.Retryable, publishException.Retryability);
            Assert.Equal(1, publishAttempts);
        }

        [Fact]
        public async Task DeliberateDisconnectWhileAwaitingAckCancelsOperation()
        {
            using MockConnectionMqttClient mockMqttClient = new() { AutoRespondToBirth = true };
            using ConnectionClient connectionClient = CreateReconnectingConnectionClient(mockMqttClient);
            using CustomTopicsClient customTopicsClient = new(connectionClient);

            await connectionClient.ConnectToHubAsync(CreateHubConnectionContext(), TestContext.Current.CancellationToken);

            TaskCompletionSource publishSent = new(TaskCreationOptions.RunContinuationsAsynchronously);
            TaskCompletionSource<MqttPublishAck> pendingPuback = new(TaskCreationOptions.RunContinuationsAsynchronously);
            mockMqttClient.OnPublish = _ =>
            {
                publishSent.TrySetResult();
                return pendingPuback.Task;
            };

            mockMqttClient.OnDisconnect = _ =>
            {
                pendingPuback.TrySetException(new MQTTnet.Exceptions.MqttClientDisconnectedException(null));
                return Task.CompletedTask;
            };

            Task<MqttPublishAck> publishTask = customTopicsClient.PublishAsync(
                new MqttPublish() { Topic = "custom/topic", QualityOfServiceLevel = MqttQualityOfServiceLevel.AtLeastOnce },
                TestContext.Current.CancellationToken);
            await publishSent.Task.WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);

            await connectionClient.DisconnectAsync(TestContext.Current.CancellationToken);

            await Assert.ThrowsAsync<OperationCanceledException>(() => publishTask.WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken));
        }

        [Fact]
        public async Task UnrecoverableFaultWhileWaitingForConnectionThrowsOperationCanceledWithTheFault()
        {
            using MockConnectionMqttClient mockMqttClient = new() { AutoRespondToBirth = true };
            using ConnectionClient connectionClient = CreateReconnectingConnectionClient(mockMqttClient);
            using CustomTopicsClient customTopicsClient = new(connectionClient);

            await connectionClient.ConnectToHubAsync(CreateHubConnectionContext(), TestContext.Current.CancellationToken);

            TaskCompletionSource publishAttempted = new(TaskCreationOptions.RunContinuationsAsynchronously);
            mockMqttClient.OnPublish = _ =>
            {
                publishAttempted.TrySetResult();
                throw new MqttClientNotConnectedException("mock client not connected exception");
            };

            Task<MqttPublishAck> publishTask = Task.Run(
                () => customTopicsClient.PublishAsync(new MqttPublish() { Topic = "custom/topic" }, TestContext.Current.CancellationToken),
                TestContext.Current.CancellationToken);
            await publishAttempted.Task.WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);

            await mockMqttClient.SimulateServerDisconnectAsync(MqttDisconnectReason.ServerMoved);

            OperationCanceledException exception = await Assert.ThrowsAsync<OperationCanceledException>(
                () => publishTask.WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken));
            DeviceException fault = Assert.IsType<DeviceException>(exception.InnerException);
            Assert.Equal(ErrorRetryability.Terminal, fault.Retryability);
        }

        [Fact]
        public async Task DeliberateDisconnectCancelsOperationWaitingForConnection()
        {
            using MockConnectionMqttClient mockMqttClient = new() { AutoRespondToBirth = true };
            using ConnectionClient connectionClient = CreateReconnectingConnectionClient(mockMqttClient);
            using CustomTopicsClient customTopicsClient = new(connectionClient);

            await connectionClient.ConnectToHubAsync(CreateHubConnectionContext(), TestContext.Current.CancellationToken);

            TaskCompletionSource publishAttempted = new(TaskCreationOptions.RunContinuationsAsynchronously);
            mockMqttClient.OnPublish = _ =>
            {
                publishAttempted.TrySetResult();
                throw new MqttClientNotConnectedException("mock client not connected exception");
            };

            using CancellationTokenSource operationCancellation = CancellationTokenSource.CreateLinkedTokenSource(TestContext.Current.CancellationToken);
            Task<MqttPublishAck> publishTask = Task.Run(
                () => customTopicsClient.PublishAsync(new MqttPublish() { Topic = "custom/topic" }, operationCancellation.Token),
                TestContext.Current.CancellationToken);
            await publishAttempted.Task.WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);

            // There is no hook for when the operation starts waiting, so give it time to get there after its failed
            // attempt. A disconnect that lands before then fails the operation fast instead.
            await Task.Delay(TimeSpan.FromMilliseconds(500), TestContext.Current.CancellationToken);

            await connectionClient.DisconnectAsync(TestContext.Current.CancellationToken);

            await Assert.ThrowsAsync<OperationCanceledException>(() => publishTask.WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken));
            Assert.False(operationCancellation.IsCancellationRequested);
        }
    }
}
