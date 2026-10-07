// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Mqtt;
using Microsoft.Azure.Iot.Device.MQTTv5.Connection;
using MQTTnet;
using System.Collections.Immutable;
using System.Diagnostics;
using System.Reflection.Metadata.Ecma335;

namespace Microsoft.Azure.Iot.Device.MQTTv5.CustomTopics
{
    // Open questions:
    // QoS values supported by AEG? Just 1 and 0?
    // Are we just exposing MQTT semantics and types? Probably, but check with Usha
    // Should we allow delayed acks?
    // Other variables besides {deviceId} ? No. And my assumption was correct that this SDK is expected to pass in the actual deviceId in the topic string, not the placeholder "{deviceId}" string
    // Does Iot Hub allow sending publishes to self? Would be useful for testing/samples but users probably wouldn't

    public class CustomTopicsClient : IDisposable
    {
        // Volatile because it is read by the publish received handler and by in-flight operations on other threads. Only set
        // while holding _subscribedCustomTopicsLock so that no tracked topic update can land after disposal.
        private volatile bool _isDisposed = false;

        private IConnectionClient _connection;

        // Immutable so that readers (such as the publish received handler) can safely enumerate a snapshot while
        // subscribe/unsubscribe calls concurrently replace it. Writers must hold _subscribedCustomTopicsLock.
        private volatile ImmutableList<string> _subscribedCustomTopics = ImmutableList<string>.Empty;

        private readonly object _subscribedCustomTopicsLock = new();

        // Serializes subscribe/unsubscribe operations end-to-end (request through ack processing) so that the tracked topics
        // are updated in the same order the operations were sent on the wire.
        private readonly SemaphoreSlim _subscriptionOperationLock = new(1, 1);

        public event Func<MqttPublishReceivedEventArgs, Task>? CustomTopicPublishReceivedAsync;

        /// <summary>
        /// Construct a new <see cref="CustomTopicsClient"/> instance.
        /// </summary>
        /// <param name="connection">The connection client this feature client will use.</param>
        /// <remarks>
        /// The provided connection client does not need to be connected before this constructor is called. However, the provided connection client must be connected prior
        /// to using this feature client.
        /// </remarks>
        public CustomTopicsClient(IConnectionClient connection)
        {
            _connection = connection;
            _connection.PublishReceivedAsync += HandleReceivedMqttPublish;
        }

        /// <summary>
        /// A point-in-time snapshot of the custom topics this client is currently subscribed to.
        /// </summary>
        /// <remarks>
        /// Topics in a subscribe request that is still in flight are included, since the service may begin delivering
        /// publishes on them before the SUBACK is processed.
        /// </remarks>
        public IReadOnlyList<string> GetSubscribedTopics => _subscribedCustomTopics;

        //TODO this gets weird if the device loses connection -> reprovisions -> gets assigned a new device Id. Need some kind of hook from connection client that allows for changing the re-subscribes after connecting to the new hub
        // since the user conceptually just wants to use the deviceId wildcard, right? But the sender side would have no way of knowing that the recipient changed device ids? Maybe the developer would actually want the newly provisioned device to still
        // subscribe using the old deviceId to catch the publishes intended for it prior to provisioning? Seems like both approaches could be useful to customer. Maybe just have a flag on the subscribe call that signals if the deviceId present should stay up-to-date
        // with new deviceIds or if it should stay the same value once subscribed
        public string GetDeviceId()
        {
            ObjectDisposedException.ThrowIf(_isDisposed, this);

            var connectionContext = _connection.GetCurrentConnectionContext();
            if (connectionContext == null)
            {
                throw new NotSupportedException("Must be connected before calling this method.");
            }

            return connectionContext.DeviceId;
        }

        public async Task<MqttPublishAck> PublishAsync(MqttPublish publish, CancellationToken cancellationToken = default)
        {
            ObjectDisposedException.ThrowIf(_isDisposed, this);
            cancellationToken.ThrowIfCancellationRequested();

            return await _connection.PublishAsync(publish, cancellationToken);
        }


        //TODO currently implementation loses all subscriptions upon losing connection. Fix later.
        public async Task<MqttClientSubscribeReasonCode> SubscribeAsync(string topic, MqttQualityOfServiceLevel qos, CancellationToken cancellationToken = default)
        {
            MqttSubscribe subscribe = new()
            {
                TopicFilters = new() { { new MqttTopicFilter(topic, qos) } }
            };

            var suback = await SubscribeAsync(subscribe, cancellationToken);

            // TODO should always be the case, but don't be lazy, Tim
            return suback.Items.FirstOrDefault()!.ReasonCode;
        }

        //TODO is the underlying connection client going to retry subscriptions if the user provides a malformed topic string? Probably don't want that. I think MQTT-level failures (suback with negative reason code) are just returned as-is, right?
        public async Task<MqttSubscribeAck> SubscribeAsync(MqttSubscribe subscribe, CancellationToken cancellationToken = default)
        {
            //TODO probably want some kind of client-side validation of some topic strings, but only if AEG-returned error is too hard for user to interpret

            ObjectDisposedException.ThrowIf(_isDisposed, this);
            cancellationToken.ThrowIfCancellationRequested();

            await _subscriptionOperationLock.WaitAsync(cancellationToken);
            try
            {
                // This client may have been disposed while waiting for a prior operation to finish
                ObjectDisposedException.ThrowIf(_isDisposed, this);

                // Track the requested topics before sending the SUBSCRIBE. The service may deliver publishes on a topic as soon
                // as it grants the subscription, which can be before this client processes the SUBACK. Those publishes must
                // still be delivered to the user (and acknowledged) rather than dropped.
                UpdateSubscribedTopics(builder =>
                {
                    foreach (var topicFilter in subscribe.TopicFilters)
                    {
                        builder.Add(topicFilter.Topic);
                    }
                });

                MqttSubscribeAck suback;
                try
                {
                    suback = await _connection.SubscribeAsync(subscribe, cancellationToken);
                }
                catch
                {
                    UpdateSubscribedTopics(builder =>
                    {
                        foreach (var topicFilter in subscribe.TopicFilters)
                        {
                            builder.Remove(topicFilter.Topic);
                        }
                    });

                    throw;
                }

                // Roll back only the topics the service did not grant. Remove a single instance so that a pre-existing
                // subscription to the same topic stays tracked.
                UpdateSubscribedTopics(builder =>
                {
                    foreach (var subackItem in suback.Items)
                    {
                        if (subackItem.ReasonCode != MqttClientSubscribeReasonCode.GrantedQoS0 && subackItem.ReasonCode != MqttClientSubscribeReasonCode.GrantedQoS1 && subackItem.ReasonCode != MqttClientSubscribeReasonCode.GrantedQoS2)
                        {
                            builder.Remove(subackItem.TopicFilter.Topic);
                        }
                    }
                });

                return suback;
            }
            finally
            {
                _subscriptionOperationLock.Release();
            }
        }

        public async Task<MqttClientUnsubscribeReasonCode> UnsubscribeAsync(string topic, CancellationToken cancellationToken = default)
        {
            MqttUnsubscribe unsubscribe = new(topic);

            var unsuback = await UnsubscribeAsync(unsubscribe, cancellationToken);

            return unsuback.Items.FirstOrDefault()!.ReasonCode;
        }

        public async Task<MqttUnsubscribeAck> UnsubscribeAsync(MqttUnsubscribe unsubscribe, CancellationToken cancellationToken = default)
        {
            ObjectDisposedException.ThrowIf(_isDisposed, this);
            cancellationToken.ThrowIfCancellationRequested();

            await _subscriptionOperationLock.WaitAsync(cancellationToken);
            try
            {
                // This client may have been disposed while waiting for a prior operation to finish
                ObjectDisposedException.ThrowIf(_isDisposed, this);

                // Keep tracking the topics until the UNSUBACK so publishes the service sends before then are still delivered
                var unsuback = await _connection.UnsubscribeAsync(unsubscribe, cancellationToken);

                UpdateSubscribedTopics(builder =>
                {
                    foreach (var unsubackItem in unsuback.Items)
                    {
                        // NoSubscriptionExisted means the service no longer considers this client subscribed, so stop tracking it too
                        if (unsubackItem.ReasonCode == MqttClientUnsubscribeReasonCode.Success || unsubackItem.ReasonCode == MqttClientUnsubscribeReasonCode.NoSubscriptionExisted)
                        {
                            builder.RemoveAll(subscribedTopic => subscribedTopic == unsubackItem.TopicFilter);
                        }
                    }
                });

                return unsuback;
            }
            finally
            {
                _subscriptionOperationLock.Release();
            }
        }

        private void UpdateSubscribedTopics(Action<ImmutableList<string>.Builder> update)
        {
            lock (_subscribedCustomTopicsLock)
            {
                // An operation that was already in flight when this client was disposed must not repopulate the tracked topics
                if (_isDisposed)
                {
                    return;
                }

                var builder = _subscribedCustomTopics.ToBuilder();
                update(builder);
                _subscribedCustomTopics = builder.ToImmutable();
            }
        }

        /// <summary>
        /// Releases the unmanaged resources used by this client and optionally disposes of the managed resources.
        /// </summary>
        /// <param name="disposing">true to release both managed and unmanaged resources; false to releases only unmanaged resources.</param>
        /// <remarks>
        /// Safe to call multiple times and concurrently with other operations on this client. Once this returns, no new
        /// <see cref="CustomTopicPublishReceivedAsync"/> invocations will start, but invocations already in progress are not
        /// waited on. Subscribe/unsubscribe operations already in flight will complete, but will no longer update
        /// <see cref="GetSubscribedTopics"/>.
        /// </remarks>
        public void Dispose(bool disposing)
        {
            lock (_subscribedCustomTopicsLock)
            {
                if (_isDisposed)
                {
                    return;
                }

                // Mark disposed before touching the connection so the publish received handler stops delivering to the user
                // (and stops acknowledging on a connection that is about to be disposed)
                _isDisposed = true;
                _subscribedCustomTopics = ImmutableList<string>.Empty;
            }

            _connection.PublishReceivedAsync -= HandleReceivedMqttPublish;

            if (disposing)
            {
                _connection.Dispose();
            }
        }

        /// <summary>
        /// Releases the unmanaged resources and disposes of the managed resources used by this client 
        /// </summary>
        /// <remarks>
        /// See <see cref="Dispose(bool)"/> for the guarantees this provides relative to concurrent operations.
        /// </remarks>
        public void Dispose()
        {
            Dispose(true);
            GC.SuppressFinalize(this);
        }

        private async Task HandleReceivedMqttPublish(MqttPublishReceivedEventArgs args)
        {
            // The connection may still be mid-dispatch of a publish when this client is disposed
            if (_isDisposed)
            {
                return;
            }

            // Need matching logic since subscribed topics may include wildcard but the actual sent publish will expand from that wildcard
            // Enumerating the immutable snapshot is safe even if a subscribe/unsubscribe replaces the list concurrently
            foreach (string subscribedCustomTopic in _subscribedCustomTopics)
            {
                // TODO probably want to test scenarios where user subscribes to two custom topics and receives a publish that satisfies both topics (due to wildcard rules)
                //  This scenario should be fine for ACK'ing since we still only expose the publish to the user once even if they were subscribed to it in two ways
                if (DoesTopicMatchFilter(args.Publish.Topic, subscribedCustomTopic))
                {
                    // The snapshot above may predate a concurrent Dispose, so check again before calling into user code
                    if (_isDisposed)
                    {
                        return;
                    }

                    // Read the event once so a handler being removed concurrently can't null it out between the check and the invoke
                    var handlers = CustomTopicPublishReceivedAsync;
                    if (handlers != null)
                    {
                        await InvokeAllHandlersAsync(handlers, args);
                    }
                    else
                    {
                        Trace.TraceError("Received a publish on a custom topic, but user did not setup CustomTopicPublishReceivedAsync to receive it. The publish will be acknowledged and will not be re-delivered.");
                    }

                    // The user's handler may have run long enough for this client (and possibly the connection) to be disposed
                    if (_isDisposed)
                    {
                        return;
                    }

                    // For now, just ack for the user (regardless of if they have callback set). Probably needs more thought
                    await args.AcknowledgeAsync(CancellationToken.None);

                    // Don't execute message received callback again even if that same message satisfies multiple topic strings
                    break;
                }
            }
        }

        /// <summary>
        /// Invokes every handler attached to a multicast async event and waits for all of them. Invoking the multicast
        /// delegate directly would only return (and so only await) the last handler's task.
        /// </summary>
        private static async Task InvokeAllHandlersAsync(Func<MqttPublishReceivedEventArgs, Task> handlers, MqttPublishReceivedEventArgs args)
        {
            Delegate[] invocationList = handlers.GetInvocationList();
            var tasks = new Task[invocationList.Length];
            for (int i = 0; i < invocationList.Length; i++)
            {
                try
                {
                    tasks[i] = ((Func<MqttPublishReceivedEventArgs, Task>)invocationList[i]).Invoke(args);
                }
                catch (Exception ex)
                {
                    // A handler throwing synchronously shouldn't prevent the remaining handlers from running
                    tasks[i] = Task.FromException(ex);
                }
            }

            await Task.WhenAll(tasks);
        }

        /// <summary>
        /// Checks if the given topic matches the given filter (including accounting for wildcard characters)
        /// </summary>
        private static bool DoesTopicMatchFilter(string topic, string filter)
        {
            return MqttTopicFilterComparer.Compare(topic, filter) == MqttTopicFilterCompareResult.IsMatch;
        }
    }
}
