// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Authentication;
using Microsoft.Azure.Iot.Device.Mqtt;
using Microsoft.Azure.Iot.Device.MQTTv5.Connection;
using Microsoft.Azure.Iot.Device.MQTTv5.CustomTopics;
using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Retry;
using System.Text;
using Xunit;

namespace Microsoft.Azure.Iot.Device.UnitTests.MQTTv5
{
    public class CustomTopicsClientUnitTests
    {
        private const string DeviceId = "someDeviceId";

        // If a user subscribes using two topic filters like "foo/bar/#" and "foo/#", and this client receives a publish with topic string "foo/bar/foo", then this client should still only execute the CustomTopicPublishReceivedAsync callback once.
        // Additionally, this custom topic client should only attempt to acknowledge the underlying MQTT publish once
        [Fact]
        public async Task UserReceivesPublishThatSatisfiesMultipleSubscribedTopicsShouldOnlyReceiveCallbackOnce()
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv5, DeviceId),
            };
            using CustomTopicsClient customTopicsClient = new(connection);

            List<MqttPublish> receivedPublishes = new();
            customTopicsClient.CustomTopicPublishReceivedAsync += args =>
            {
                receivedPublishes.Add(args.Publish);
                return Task.CompletedTask;
            };

            await customTopicsClient.SubscribeAsync("foo/bar/#", MqttQualityOfServiceLevel.AtLeastOnce, TestContext.Current.CancellationToken);
            await customTopicsClient.SubscribeAsync("foo/#", MqttQualityOfServiceLevel.AtLeastOnce, TestContext.Current.CancellationToken);

            Assert.Equal(2, connection.SubscribedMessages.Count);
            Assert.Equal("foo/bar/#", Assert.Single(connection.SubscribedMessages[0].TopicFilters).Topic);
            Assert.Equal("foo/#", Assert.Single(connection.SubscribedMessages[1].TopicFilters).Topic);
            Assert.Equal(new[] { "foo/bar/#", "foo/#" }, customTopicsClient.GetSubscribedTopics);

            MqttPublish publish = new()
            {
                Topic = "foo/bar/foo",
                QualityOfServiceLevel = MqttQualityOfServiceLevel.AtLeastOnce,
                Payload = Encoding.UTF8.GetBytes("hello"),
            };

            MockMqttPublishReceivedEventArgs deliveredArgs = await connection.SimulateReceiveAsync(publish);

            MqttPublish receivedPublish = Assert.Single(receivedPublishes);
            Assert.Same(publish, receivedPublish);
            Assert.Equal(1, deliveredArgs.AcknowledgeCount);
        }

        // If a user subscribes to multiple topics at once, and only some of them are successful, this client should still track those topics as subscribed. This client should also still receive and delegate publishes to those topics to the user
        [Fact]
        public async Task SubscribeAsyncAllowsPartialSuccessCase()
        {
            const string grantedTopic = "granted/topic";
            const string rejectedTopic = "rejected/topic";

            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv5, DeviceId),
                OnSubscribe = subscribe =>
                {
                    List<MqttSubscribeAckItem> items = new();
                    foreach (MqttTopicFilter filter in subscribe.TopicFilters)
                    {
                        items.Add(new MqttSubscribeAckItem()
                        {
                            ReasonCode = filter.Topic == rejectedTopic
                                ? MqttClientSubscribeReasonCode.NotAuthorized
                                : MqttClientSubscribeReasonCode.GrantedQoS1,
                            TopicFilter = new(filter.Topic, filter.QualityOfServiceLevel),
                        });
                    }

                    return Task.FromResult(new MqttSubscribeAck() { Items = items });
                },
            };
            using CustomTopicsClient customTopicsClient = new(connection);

            List<MqttPublish> receivedPublishes = new();
            customTopicsClient.CustomTopicPublishReceivedAsync += args =>
            {
                receivedPublishes.Add(args.Publish);
                return Task.CompletedTask;
            };

            MqttSubscribe subscribe = new()
            {
                TopicFilters = new()
                {
                    new MqttTopicFilter(grantedTopic, MqttQualityOfServiceLevel.AtLeastOnce),
                    new MqttTopicFilter(rejectedTopic, MqttQualityOfServiceLevel.AtLeastOnce),
                },
            };

            MqttSubscribeAck suback = await customTopicsClient.SubscribeAsync(subscribe, TestContext.Current.CancellationToken);

            // The partial failure is surfaced to the user as-is rather than thrown
            Assert.Equal(2, suback.Items.Count);
            Assert.Equal(MqttClientSubscribeReasonCode.GrantedQoS1, suback.Items.Single(i => i.TopicFilter.Topic == grantedTopic).ReasonCode);
            Assert.Equal(MqttClientSubscribeReasonCode.NotAuthorized, suback.Items.Single(i => i.TopicFilter.Topic == rejectedTopic).ReasonCode);

            MqttSubscribe sentSubscribe = Assert.Single(connection.SubscribedMessages);
            Assert.Equal(new[] { grantedTopic, rejectedTopic }, sentSubscribe.TopicFilters.Select(f => f.Topic));

            Assert.Equal(grantedTopic, Assert.Single(customTopicsClient.GetSubscribedTopics));

            MqttPublish grantedPublish = new()
            {
                Topic = grantedTopic,
                QualityOfServiceLevel = MqttQualityOfServiceLevel.AtLeastOnce,
                Payload = Encoding.UTF8.GetBytes("granted"),
            };
            MockMqttPublishReceivedEventArgs grantedArgs = await connection.SimulateReceiveAsync(grantedPublish);

            Assert.Same(grantedPublish, Assert.Single(receivedPublishes));
            Assert.Equal(1, grantedArgs.AcknowledgeCount);

            // Publishes on the rejected topic are not this client's to handle, so they are neither delegated nor acknowledged
            MockMqttPublishReceivedEventArgs rejectedArgs = await connection.SimulateReceiveAsync(new MqttPublish()
            {
                Topic = rejectedTopic,
                QualityOfServiceLevel = MqttQualityOfServiceLevel.AtLeastOnce,
                Payload = Encoding.UTF8.GetBytes("rejected"),
            });

            Assert.Single(receivedPublishes);
            Assert.Equal(0, rejectedArgs.AcknowledgeCount);
        }

        // Unsubscribing (via either overload) should send the UNSUBSCRIBE, stop tracking the topic, and stop delegating publishes on it to the user
        [Fact]
        public async Task UnsubscribeAsyncStopsTrackingAndDelegatingTopics()
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv5, DeviceId),
            };
            using CustomTopicsClient customTopicsClient = new(connection);

            List<MqttPublish> receivedPublishes = new();
            customTopicsClient.CustomTopicPublishReceivedAsync += args =>
            {
                receivedPublishes.Add(args.Publish);
                return Task.CompletedTask;
            };

            await customTopicsClient.SubscribeAsync("a", MqttQualityOfServiceLevel.AtLeastOnce, TestContext.Current.CancellationToken);
            await customTopicsClient.SubscribeAsync("b", MqttQualityOfServiceLevel.AtLeastOnce, TestContext.Current.CancellationToken);
            await customTopicsClient.SubscribeAsync("c", MqttQualityOfServiceLevel.AtLeastOnce, TestContext.Current.CancellationToken);

            MqttClientUnsubscribeReasonCode reasonCode = await customTopicsClient.UnsubscribeAsync("a", TestContext.Current.CancellationToken);
            Assert.Equal(MqttClientUnsubscribeReasonCode.Success, reasonCode);
            Assert.Equal(new[] { "b", "c" }, customTopicsClient.GetSubscribedTopics);

            MqttUnsubscribe multiUnsubscribe = new();
            multiUnsubscribe.TopicFilters.Add("b");
            multiUnsubscribe.TopicFilters.Add("c");
            MqttUnsubscribeAck unsuback = await customTopicsClient.UnsubscribeAsync(multiUnsubscribe, TestContext.Current.CancellationToken);
            Assert.Equal(2, unsuback.Items.Count);
            Assert.Empty(customTopicsClient.GetSubscribedTopics);

            Assert.Equal(2, connection.UnsubscribedMessages.Count);
            Assert.Equal(new[] { "a" }, connection.UnsubscribedMessages[0].TopicFilters);
            Assert.Equal(new[] { "b", "c" }, connection.UnsubscribedMessages[1].TopicFilters);

            MockMqttPublishReceivedEventArgs args = await connection.SimulateReceiveAsync(new MqttPublish()
            {
                Topic = "a",
                QualityOfServiceLevel = MqttQualityOfServiceLevel.AtLeastOnce,
            });

            Assert.Empty(receivedPublishes);
            Assert.Equal(0, args.AcknowledgeCount);
        }

        // Subscribes and unsubscribes may complete on other threads while inbound publishes are being matched against the
        // tracked topics. None of these should corrupt the tracked topics or throw due to concurrent modification.
        [Fact]
        public async Task ConcurrentSubscriptionChangesAndReceivesAreThreadSafe()
        {
            const int iterations = 2000;

            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv5, DeviceId),
            };
            using CustomTopicsClient customTopicsClient = new(connection);

            int callbackCount = 0;
            customTopicsClient.CustomTopicPublishReceivedAsync += _ =>
            {
                Interlocked.Increment(ref callbackCount);
                return Task.CompletedTask;
            };

            // Always subscribed, so every publish below should be delivered exactly once
            await customTopicsClient.SubscribeAsync("stable/#", MqttQualityOfServiceLevel.AtLeastOnce, TestContext.Current.CancellationToken);

            Task churn = Task.Run(async () =>
            {
                for (int i = 0; i < iterations; i++)
                {
                    await customTopicsClient.SubscribeAsync($"churn/{i}", MqttQualityOfServiceLevel.AtLeastOnce);
                    await customTopicsClient.UnsubscribeAsync($"churn/{i}");
                }
            }, TestContext.Current.CancellationToken);

            Task receive = Task.Run(async () =>
            {
                for (int i = 0; i < iterations; i++)
                {
                    await connection.SimulateReceiveAsync(new MqttPublish()
                    {
                        Topic = "stable/topic",
                        QualityOfServiceLevel = MqttQualityOfServiceLevel.AtLeastOnce,
                    });

                    // Matches nothing, so the full tracked topic list is enumerated while it is being churned
                    await connection.SimulateReceiveAsync(new MqttPublish()
                    {
                        Topic = "unmatched/topic",
                        QualityOfServiceLevel = MqttQualityOfServiceLevel.AtLeastOnce,
                    });
                }
            }, TestContext.Current.CancellationToken);

            await Task.WhenAll(churn, receive);

            Assert.Equal(iterations, callbackCount);
            Assert.Equal(new[] { "stable/#" }, customTopicsClient.GetSubscribedTopics);
        }

        // An unsubscribe issued while a subscribe for the same topic is in flight must not be sent (or applied to the tracked
        // topics) until the subscribe completes; otherwise the tracked topics can disagree with the service's view.
        [Fact]
        public async Task SubscribeAndUnsubscribeOperationsAreSerialized()
        {
            TaskCompletionSource<MqttSubscribeAck> pendingSuback = new(TaskCreationOptions.RunContinuationsAsynchronously);
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv5, DeviceId),
                OnSubscribe = _ => pendingSuback.Task,
            };
            using CustomTopicsClient customTopicsClient = new(connection);

            Task<MqttClientSubscribeReasonCode> subscribeTask = customTopicsClient.SubscribeAsync("topic", MqttQualityOfServiceLevel.AtLeastOnce, TestContext.Current.CancellationToken);
            Task<MqttClientUnsubscribeReasonCode> unsubscribeTask = customTopicsClient.UnsubscribeAsync("topic", TestContext.Current.CancellationToken);

            await Task.Delay(100, TestContext.Current.CancellationToken);
            Assert.False(unsubscribeTask.IsCompleted);
            Assert.Empty(connection.UnsubscribedMessages);

            pendingSuback.SetResult(MqttObjectHelpers.CreateSuccessfulSuback(Assert.Single(connection.SubscribedMessages)));

            Assert.Equal(MqttClientSubscribeReasonCode.GrantedQoS1, await subscribeTask);
            Assert.Equal(MqttClientUnsubscribeReasonCode.Success, await unsubscribeTask);
            Assert.Single(connection.UnsubscribedMessages);
            Assert.Empty(customTopicsClient.GetSubscribedTopics);
        }

        [Fact]
        public async Task OperationsThrowAfterDisconnect()
        {
            using MockConnectionMqttClient mqttClient = new() { AutoRespondToBirth = true };
            using ConnectionClient connection = new(new() { MqttClient = mqttClient });
            await connection.ConnectToHubAsync(
                MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv5, DeviceId),
                TestContext.Current.CancellationToken);
            using CustomTopicsClient customTopicsClient = new(connection);

            await connection.DisconnectAsync(TestContext.Current.CancellationToken);

            await Assert.ThrowsAsync<NotSupportedException>(
                async () => await customTopicsClient.PublishAsync(new MqttPublish() { Topic = "topic" }, TestContext.Current.CancellationToken));
            await Assert.ThrowsAsync<NotSupportedException>(
                async () => await customTopicsClient.SubscribeAsync("topic", MqttQualityOfServiceLevel.AtLeastOnce, TestContext.Current.CancellationToken));
            await Assert.ThrowsAsync<NotSupportedException>(
                async () => await customTopicsClient.UnsubscribeAsync("topic", TestContext.Current.CancellationToken));
            Assert.Empty(customTopicsClient.GetSubscribedTopics);
        }

        [Fact]
        public async Task OperationsThrowWhenNeverConnected()
        {
            MockFeatureConnectionClient connection = new();
            using CustomTopicsClient customTopicsClient = new(connection);

            await Assert.ThrowsAsync<NotSupportedException>(
                async () => await customTopicsClient.PublishAsync(new MqttPublish() { Topic = "topic" }, TestContext.Current.CancellationToken));
            await Assert.ThrowsAsync<NotSupportedException>(
                async () => await customTopicsClient.SubscribeAsync("topic", MqttQualityOfServiceLevel.AtLeastOnce, TestContext.Current.CancellationToken));
            await Assert.ThrowsAsync<NotSupportedException>(
                async () => await customTopicsClient.UnsubscribeAsync("topic", TestContext.Current.CancellationToken));
            Assert.Empty(connection.PublishedMessages);
            Assert.Empty(connection.SubscribedMessages);
            Assert.Empty(connection.UnsubscribedMessages);
        }

        // A subscription operation queued behind another must not be sent if the connection client is explicitly
        // disconnected while it waits, since it would otherwise wait indefinitely for a reconnect that never comes.
        [Fact]
        public async Task QueuedSubscriptionOperationThrowsIfDisconnectedWhileWaiting()
        {
            TaskCompletionSource<MqttSubscribeAck> pendingSuback = new(TaskCreationOptions.RunContinuationsAsynchronously);
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv5, DeviceId),
                OnSubscribe = _ => pendingSuback.Task,
            };
            using CustomTopicsClient customTopicsClient = new(connection);

            Task<MqttClientSubscribeReasonCode> subscribeTask = customTopicsClient.SubscribeAsync("topic", MqttQualityOfServiceLevel.AtLeastOnce, TestContext.Current.CancellationToken);
            Task<MqttClientSubscribeReasonCode> queuedSubscribeTask = customTopicsClient.SubscribeAsync("other", MqttQualityOfServiceLevel.AtLeastOnce, TestContext.Current.CancellationToken);
            Task<MqttClientUnsubscribeReasonCode> queuedUnsubscribeTask = customTopicsClient.UnsubscribeAsync("topic", TestContext.Current.CancellationToken);

            connection.IsConnectionTargetSet = false;
            pendingSuback.SetResult(MqttObjectHelpers.CreateSuccessfulSuback(Assert.Single(connection.SubscribedMessages)));

            Assert.Equal(MqttClientSubscribeReasonCode.GrantedQoS1, await subscribeTask);
            await Assert.ThrowsAsync<NotSupportedException>(async () => await queuedSubscribeTask);
            await Assert.ThrowsAsync<NotSupportedException>(async () => await queuedUnsubscribeTask);
            Assert.Single(connection.SubscribedMessages);
            Assert.Empty(connection.UnsubscribedMessages);
        }

        // The service may deliver a publish on a newly granted topic before this client processes the SUBACK. That publish
        // must still be delivered to the user and acknowledged rather than dropped.
        [Fact]
        public async Task PublishReceivedBeforeSubackIsProcessedIsDelivered()
        {
            MockMqttPublishReceivedEventArgs? earlyArgs = null;
            MockFeatureConnectionClient connection = null!;
            connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv5, DeviceId),
                OnSubscribe = async subscribe =>
                {
                    earlyArgs = await connection.SimulateReceiveAsync(new MqttPublish()
                    {
                        Topic = "early/topic",
                        QualityOfServiceLevel = MqttQualityOfServiceLevel.AtLeastOnce,
                    });

                    return MqttObjectHelpers.CreateSuccessfulSuback(subscribe);
                },
            };
            using CustomTopicsClient customTopicsClient = new(connection);

            List<MqttPublish> receivedPublishes = new();
            customTopicsClient.CustomTopicPublishReceivedAsync += args =>
            {
                receivedPublishes.Add(args.Publish);
                return Task.CompletedTask;
            };

            await customTopicsClient.SubscribeAsync("early/#", MqttQualityOfServiceLevel.AtLeastOnce, TestContext.Current.CancellationToken);

            Assert.Equal("early/topic", Assert.Single(receivedPublishes).Topic);
            Assert.NotNull(earlyArgs);
            Assert.Equal(1, earlyArgs.AcknowledgeCount);
            Assert.Equal(new[] { "early/#" }, customTopicsClient.GetSubscribedTopics);
        }

        // Topics are tracked optimistically while a subscribe is in flight, so a subscribe that fails must roll them back
        // without disturbing a pre-existing subscription to the same topic.
        [Fact]
        public async Task FailedSubscribeRollsBackOptimisticallyTrackedTopics()
        {
            bool failSubscribe = false;
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv5, DeviceId),
                OnSubscribe = subscribe => failSubscribe
                    ? throw new InvalidOperationException("simulated failure")
                    : Task.FromResult(MqttObjectHelpers.CreateSuccessfulSuback(subscribe)),
            };
            using CustomTopicsClient customTopicsClient = new(connection);

            await customTopicsClient.SubscribeAsync("existing", MqttQualityOfServiceLevel.AtLeastOnce, TestContext.Current.CancellationToken);

            failSubscribe = true;
            MqttSubscribe subscribe = new()
            {
                TopicFilters = new()
                {
                    new MqttTopicFilter("existing", MqttQualityOfServiceLevel.AtLeastOnce),
                    new MqttTopicFilter("new", MqttQualityOfServiceLevel.AtLeastOnce),
                },
            };
            await Assert.ThrowsAsync<InvalidOperationException>(() => customTopicsClient.SubscribeAsync(subscribe, TestContext.Current.CancellationToken));

            Assert.Equal(new[] { "existing" }, customTopicsClient.GetSubscribedTopics);
        }

        // Topics may hold the device ID placeholder, which the connection client replaces when it sends the operation (so the
        // acks report the replaced topics). This client must keep tracking the topics as the user gave them, roll back the
        // right ones, and match received publishes using whatever this device's ID currently is.
        [Fact]
        public async Task DeviceIdPlaceholderTopicsAreTrackedAsGivenAndMatchedWithCurrentDeviceId()
        {
            const string grantedTopic = "granted/" + IConnectionClient.DeviceIdPlaceholder;
            const string rejectedTopic = "rejected/" + IConnectionClient.DeviceIdPlaceholder;

            MockFeatureConnectionClient connection = null!;
            connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv5, DeviceId),
                OnSubscribe = subscribe => Task.FromResult(new MqttSubscribeAck()
                {
                    Items = subscribe.TopicFilters.Select(filter => new MqttSubscribeAckItem()
                    {
                        ReasonCode = filter.Topic == rejectedTopic
                            ? MqttClientSubscribeReasonCode.NotAuthorized
                            : MqttClientSubscribeReasonCode.GrantedQoS1,
                        TopicFilter = new(ResolveDeviceId(filter.Topic, connection), filter.QualityOfServiceLevel),
                    }).ToList(),
                }),
                OnUnsubscribe = unsubscribe => Task.FromResult(new MqttUnsubscribeAck()
                {
                    Items = unsubscribe.TopicFilters.Select(filter => new MqttUnsubscribeAckItem()
                    {
                        ReasonCode = MqttClientUnsubscribeReasonCode.Success,
                        TopicFilter = ResolveDeviceId(filter, connection),
                    }).ToList(),
                }),
            };
            using CustomTopicsClient customTopicsClient = new(connection);

            List<MqttPublish> receivedPublishes = new();
            customTopicsClient.CustomTopicPublishReceivedAsync += args =>
            {
                receivedPublishes.Add(args.Publish);
                return Task.CompletedTask;
            };

            MqttSubscribe subscribe = new()
            {
                TopicFilters = new()
                {
                    new MqttTopicFilter(grantedTopic, MqttQualityOfServiceLevel.AtLeastOnce),
                    new MqttTopicFilter(rejectedTopic, MqttQualityOfServiceLevel.AtLeastOnce),
                },
            };
            await customTopicsClient.SubscribeAsync(subscribe, TestContext.Current.CancellationToken);

            // The placeholder is left for the connection client to fill in, and only the rejected topic is rolled back
            Assert.Equal(new[] { grantedTopic, rejectedTopic }, Assert.Single(connection.SubscribedMessages).TopicFilters.Select(f => f.Topic));
            Assert.Equal(new[] { grantedTopic }, customTopicsClient.GetSubscribedTopics);

            MockMqttPublishReceivedEventArgs otherDeviceArgs = await connection.SimulateReceiveAsync(new MqttPublish() { Topic = "granted/otherDeviceId" });
            Assert.Empty(receivedPublishes);
            Assert.Equal(0, otherDeviceArgs.AcknowledgeCount);

            MockMqttPublishReceivedEventArgs thisDeviceArgs = await connection.SimulateReceiveAsync(new MqttPublish() { Topic = $"granted/{DeviceId}" });
            Assert.Single(receivedPublishes);
            Assert.Equal(1, thisDeviceArgs.AcknowledgeCount);

            // Re-provisioning assigned this device a different ID
            connection.CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv5, "newDeviceId");

            await connection.SimulateReceiveAsync(new MqttPublish() { Topic = $"granted/{DeviceId}" });
            Assert.Single(receivedPublishes);

            await connection.SimulateReceiveAsync(new MqttPublish() { Topic = "granted/newDeviceId" });
            Assert.Equal(2, receivedPublishes.Count);

            await customTopicsClient.UnsubscribeAsync(grantedTopic, TestContext.Current.CancellationToken);
            Assert.Equal(grantedTopic, Assert.Single(Assert.Single(connection.UnsubscribedMessages).TopicFilters));
            Assert.Empty(customTopicsClient.GetSubscribedTopics);
        }

        private static string ResolveDeviceId(string topic, MockFeatureConnectionClient connection)
        {
            return topic.Replace(IConnectionClient.DeviceIdPlaceholder, connection.CurrentConnectionContext!.DeviceId, StringComparison.Ordinal);
        }

        // When multiple handlers are attached to CustomTopicPublishReceivedAsync, all of them must run and complete before
        // the publish is acknowledged.
        [Fact]
        public async Task AllPublishReceivedHandlersCompleteBeforeAcknowledgement()
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv5, DeviceId),
            };
            using CustomTopicsClient customTopicsClient = new(connection);

            TaskCompletionSource firstHandlerCanFinish = new(TaskCreationOptions.RunContinuationsAsynchronously);
            MockMqttPublishReceivedEventArgs? deliveredArgs = null;
            bool secondHandlerRan = false;

            customTopicsClient.CustomTopicPublishReceivedAsync += async args =>
            {
                deliveredArgs = (MockMqttPublishReceivedEventArgs)args;
                await firstHandlerCanFinish.Task;
            };
            customTopicsClient.CustomTopicPublishReceivedAsync += _ =>
            {
                secondHandlerRan = true;
                return Task.CompletedTask;
            };

            await customTopicsClient.SubscribeAsync("topic", MqttQualityOfServiceLevel.AtLeastOnce, TestContext.Current.CancellationToken);

            Task receiveTask = connection.SimulateReceiveAsync(new MqttPublish()
            {
                Topic = "topic",
                QualityOfServiceLevel = MqttQualityOfServiceLevel.AtLeastOnce,
            });

            Assert.True(secondHandlerRan);
            Assert.NotNull(deliveredArgs);
            await Task.Delay(100, TestContext.Current.CancellationToken);
            Assert.False(receiveTask.IsCompleted);
            Assert.Equal(0, deliveredArgs.AcknowledgeCount);

            firstHandlerCanFinish.SetResult();
            await receiveTask;

            Assert.Equal(1, deliveredArgs.AcknowledgeCount);
        }

        // If this client is disposed while the user's handler is still running, the publish must not be acknowledged on the
        // (now disposed) connection afterwards, and no further publishes should be delivered. Dispose must also be idempotent.
        [Fact]
        public async Task DisposeDuringPublishHandlingSkipsAcknowledgementAndStopsDelivery()
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv5, DeviceId),
            };
            CustomTopicsClient customTopicsClient = new(connection);

            TaskCompletionSource handlerCanFinish = new(TaskCreationOptions.RunContinuationsAsynchronously);
            int callbackCount = 0;
            customTopicsClient.CustomTopicPublishReceivedAsync += async _ =>
            {
                Interlocked.Increment(ref callbackCount);
                await handlerCanFinish.Task;
            };

            await customTopicsClient.SubscribeAsync("topic", MqttQualityOfServiceLevel.AtLeastOnce, TestContext.Current.CancellationToken);

            Task<MockMqttPublishReceivedEventArgs> inFlightReceive = connection.SimulateReceiveAsync(new MqttPublish()
            {
                Topic = "topic",
                QualityOfServiceLevel = MqttQualityOfServiceLevel.AtLeastOnce,
            });

            customTopicsClient.Dispose();
            customTopicsClient.Dispose();

            handlerCanFinish.SetResult();
            MockMqttPublishReceivedEventArgs inFlightArgs = await inFlightReceive;

            Assert.Equal(0, inFlightArgs.AcknowledgeCount);
            Assert.True(connection.IsDisposed);
            Assert.Empty(customTopicsClient.GetSubscribedTopics);

            await connection.SimulateReceiveAsync(new MqttPublish()
            {
                Topic = "topic",
                QualityOfServiceLevel = MqttQualityOfServiceLevel.AtLeastOnce,
            });

            Assert.Equal(1, callbackCount);
        }

        // A subscribe already in flight when this client is disposed must not repopulate the tracked topics when it completes,
        // and operations still waiting their turn must fail with ObjectDisposedException instead of using the connection.
        [Fact]
        public async Task DisposeDuringSubscriptionOperationsDoesNotTrackTopicsAndFailsQueuedOperations()
        {
            TaskCompletionSource<MqttSubscribeAck> pendingSuback = new(TaskCreationOptions.RunContinuationsAsynchronously);
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv5, DeviceId),
                OnSubscribe = _ => pendingSuback.Task,
            };
            CustomTopicsClient customTopicsClient = new(connection);

            Task<MqttClientSubscribeReasonCode> subscribeTask = customTopicsClient.SubscribeAsync("topic", MqttQualityOfServiceLevel.AtLeastOnce, TestContext.Current.CancellationToken);
            Task<MqttClientUnsubscribeReasonCode> queuedUnsubscribeTask = customTopicsClient.UnsubscribeAsync("other", TestContext.Current.CancellationToken);

            customTopicsClient.Dispose();

            pendingSuback.SetResult(MqttObjectHelpers.CreateSuccessfulSuback(Assert.Single(connection.SubscribedMessages)));

            Assert.Equal(MqttClientSubscribeReasonCode.GrantedQoS1, await subscribeTask);
            await Assert.ThrowsAsync<ObjectDisposedException>(() => queuedUnsubscribeTask);

            Assert.Empty(customTopicsClient.GetSubscribedTopics);
            Assert.Empty(connection.UnsubscribedMessages);
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
#pragma warning disable SYSLIB0026 // Mock certificate, no need to load a real one
            return new ConnectionContext()
            {
                AuthenticationProvider = new X509AuthenticationProvider(new System.Security.Cryptography.X509Certificates.X509Certificate2()),
                DeviceId = DeviceId,
                IotHubHostName = "someHostName",
                ConnectionProfile = IotHubConnectionType.Mqttv5,
            };
#pragma warning restore SYSLIB0026
        }

        // Lose the connection and wait for the connection client to re-establish this device's presence, returning the
        // device presence SUBSCRIBE sent on the new connection.
        private static async Task<MqttSubscribe> ReconnectAndGetPresenceSubscribeAsync(MockConnectionMqttClient mockMqttClient, ConnectionClient connectionClient)
        {
            TaskCompletionSource presenceCompleted = new(TaskCreationOptions.RunContinuationsAsynchronously);
            Func<DevicePresenceFlowCompletedArgs, Task> onPresenceCompleted = _ =>
            {
                presenceCompleted.TrySetResult();
                return Task.CompletedTask;
            };
            connectionClient.DevicePresenceFlowCompletedAsync += onPresenceCompleted;

            int trafficCountBeforeReconnect;
            lock (mockMqttClient.SentMqttTrafficInOrder)
            {
                trafficCountBeforeReconnect = mockMqttClient.SentMqttTrafficInOrder.Count;
            }

            try
            {
                await mockMqttClient.SimulateServerDisconnectAsync(MqttDisconnectReason.ImplementationSpecificError, new Exception("mock exception"))
                    .WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);
                await presenceCompleted.Task.WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);
            }
            finally
            {
                connectionClient.DevicePresenceFlowCompletedAsync -= onPresenceCompleted;
            }

            lock (mockMqttClient.SentMqttTrafficInOrder)
            {
                return mockMqttClient.SentMqttTrafficInOrder
                    .Skip(trafficCountBeforeReconnect)
                    .Select(traffic => traffic.Subscribe)
                    .OfType<MqttSubscribe>()
                    .Single(subscribe => subscribe.TopicFilters.Any(topicFilter => topicFilter.Topic == $"ih/{DeviceId}/dev/#"));
            }
        }

        // Custom topic subscriptions are lost with the session, so they must be re-subscribed to (with their QoS and the
        // device's current ID) in the device presence SUBSCRIBE. A topic the service then rejects must stop being tracked
        // without failing the presence flow.
        [Fact]
        public async Task SubscribedTopicsAreResubscribedInDevicePresenceSubscribeUponReconnecting()
        {
            const string placeholderTopic = "foo/{deviceId}/bar";
            const string rejectedTopic = "rejected/topic";

            using MockConnectionMqttClient mockMqttClient = new() { AutoRespondToBirth = true };
            using ConnectionClient connectionClient = CreateReconnectingConnectionClient(mockMqttClient);
            using CustomTopicsClient customTopicsClient = new(connectionClient);

            await connectionClient.ConnectToHubAsync(CreateHubConnectionContext(), TestContext.Current.CancellationToken);

            await customTopicsClient.SubscribeAsync(placeholderTopic, MqttQualityOfServiceLevel.AtMostOnce, TestContext.Current.CancellationToken);
            await customTopicsClient.SubscribeAsync("baz/#", MqttQualityOfServiceLevel.AtLeastOnce, TestContext.Current.CancellationToken);
            await customTopicsClient.SubscribeAsync(rejectedTopic, MqttQualityOfServiceLevel.AtLeastOnce, TestContext.Current.CancellationToken);

            mockMqttClient.OnSubscribe = subscribe =>
            {
                MqttSubscribeAck suback = MqttObjectHelpers.CreateSuccessfulSuback(subscribe);
                foreach (MqttSubscribeAckItem item in suback.Items.Where(item => item.TopicFilter.Topic == rejectedTopic))
                {
                    item.ReasonCode = MqttClientSubscribeReasonCode.NotAuthorized;
                }

                return Task.FromResult(suback);
            };

            MqttSubscribe presenceSubscribe = await ReconnectAndGetPresenceSubscribeAsync(mockMqttClient, connectionClient);

            Assert.Equal(
                new (string, MqttQualityOfServiceLevel)[]
                {
                    ($"ih/{DeviceId}/dev/#", MqttQualityOfServiceLevel.AtLeastOnce),
                    ($"foo/{DeviceId}/bar", MqttQualityOfServiceLevel.AtMostOnce),
                    ("baz/#", MqttQualityOfServiceLevel.AtLeastOnce),
                    (rejectedTopic, MqttQualityOfServiceLevel.AtLeastOnce),
                },
                presenceSubscribe.TopicFilters.Select(topicFilter => (topicFilter.Topic, topicFilter.QualityOfServiceLevel)));

            Assert.Equal(new[] { placeholderTopic, "baz/#" }, customTopicsClient.GetSubscribedTopics);
            Assert.Empty(mockMqttClient.DisconnectRequests);
        }

        // A reconnection while an unsubscribe is in flight must not re-subscribe to its topic (which could undo the
        // unsubscribe), and since the service then no longer has that subscription, it stops being tracked even if the
        // unsubscribe fails.
        [Fact]
        public async Task TopicWithPendingUnsubscribeIsNotResubscribedUponReconnecting()
        {
            using MockConnectionMqttClient mockMqttClient = new() { AutoRespondToBirth = true };
            using ConnectionClient connectionClient = CreateReconnectingConnectionClient(mockMqttClient);
            using CustomTopicsClient customTopicsClient = new(connectionClient);

            await connectionClient.ConnectToHubAsync(CreateHubConnectionContext(), TestContext.Current.CancellationToken);

            await customTopicsClient.SubscribeAsync("a", MqttQualityOfServiceLevel.AtLeastOnce, TestContext.Current.CancellationToken);
            await customTopicsClient.SubscribeAsync("b", MqttQualityOfServiceLevel.AtLeastOnce, TestContext.Current.CancellationToken);

            TaskCompletionSource unsubscribeSent = new(TaskCreationOptions.RunContinuationsAsynchronously);
            TaskCompletionSource<MqttUnsubscribeAck> pendingUnsuback = new(TaskCreationOptions.RunContinuationsAsynchronously);
            mockMqttClient.OnUnsubscribe = _ =>
            {
                unsubscribeSent.TrySetResult();
                return pendingUnsuback.Task;
            };

            Task<MqttClientUnsubscribeReasonCode> unsubscribeTask = customTopicsClient.UnsubscribeAsync("a", TestContext.Current.CancellationToken);
            await unsubscribeSent.Task.WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);

            MqttSubscribe presenceSubscribe = await ReconnectAndGetPresenceSubscribeAsync(mockMqttClient, connectionClient);

            Assert.Equal(new[] { $"ih/{DeviceId}/dev/#", "b" }, presenceSubscribe.TopicFilters.Select(topicFilter => topicFilter.Topic));

            pendingUnsuback.SetException(new Exception("mock unsubscribe failure"));
            await Assert.ThrowsAnyAsync<Exception>(() => unsubscribeTask);

            Assert.Equal(new[] { "b" }, customTopicsClient.GetSubscribedTopics);
        }

        // Once disposed, a custom topics client must no longer add its topics to the device presence SUBSCRIBE
        [Fact]
        public async Task DisposedClientTopicsAreNotResubscribedUponReconnecting()
        {
            using MockConnectionMqttClient mockMqttClient = new() { AutoRespondToBirth = true };
            using ConnectionClient connectionClient = CreateReconnectingConnectionClient(mockMqttClient);
            CustomTopicsClient customTopicsClient = new(connectionClient);

            await connectionClient.ConnectToHubAsync(CreateHubConnectionContext(), TestContext.Current.CancellationToken);
            await customTopicsClient.SubscribeAsync("a", MqttQualityOfServiceLevel.AtLeastOnce, TestContext.Current.CancellationToken);

            customTopicsClient.Dispose(disposing: false);

            MqttSubscribe presenceSubscribe = await ReconnectAndGetPresenceSubscribeAsync(mockMqttClient, connectionClient);

            Assert.Equal($"ih/{DeviceId}/dev/#", Assert.Single(presenceSubscribe.TopicFilters).Topic);
        }
    }
}
