// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Mqtt;
using Microsoft.Azure.Iot.Device.MQTTv5.CustomTopics;
using Microsoft.Azure.Iot.Device.Provisioning.Models;
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
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(ConnectionProfile.MqttV5, DeviceId),
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
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(ConnectionProfile.MqttV5, DeviceId),
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
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(ConnectionProfile.MqttV5, DeviceId),
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
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(ConnectionProfile.MqttV5, DeviceId),
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
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(ConnectionProfile.MqttV5, DeviceId),
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

        // The service may deliver a publish on a newly granted topic before this client processes the SUBACK. That publish
        // must still be delivered to the user and acknowledged rather than dropped.
        [Fact]
        public async Task PublishReceivedBeforeSubackIsProcessedIsDelivered()
        {
            MockMqttPublishReceivedEventArgs? earlyArgs = null;
            MockFeatureConnectionClient connection = null!;
            connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(ConnectionProfile.MqttV5, DeviceId),
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
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(ConnectionProfile.MqttV5, DeviceId),
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

        // When multiple handlers are attached to CustomTopicPublishReceivedAsync, all of them must run and complete before
        // the publish is acknowledged.
        [Fact]
        public async Task AllPublishReceivedHandlersCompleteBeforeAcknowledgement()
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(ConnectionProfile.MqttV5, DeviceId),
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
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(ConnectionProfile.MqttV5, DeviceId),
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
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(ConnectionProfile.MqttV5, DeviceId),
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
    }
}
