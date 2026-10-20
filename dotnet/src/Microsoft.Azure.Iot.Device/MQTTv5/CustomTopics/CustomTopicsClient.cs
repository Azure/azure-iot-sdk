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
    // Other variables besides {deviceId} ? No. Topics may contain the "{deviceId}" placeholder (IConnectionClient.DeviceIdPlaceholder), which the connection client replaces with the device's current ID each time it sends the operation
    // Does Iot Hub allow sending publishes to self? Would be useful for testing/samples but users probably wouldn't

    /// <summary>
    /// A feature client for publishing to and subscribing to custom (application-defined) MQTT topics.
    /// </summary>
    /// <remarks>
    /// <para>
    /// Any topic or topic filter passed to this client may contain the <see cref="IConnectionClient.DeviceIdPlaceholder"/>
    /// ("{deviceId}") placeholder, for example "foo/{deviceId}/bar". The connection client replaces the placeholder with
    /// this device's current ID each time it actually sends the operation, so an operation that is pending while the device
    /// is re-provisioned and assigned a different device ID is sent with the newly assigned ID.
    /// </para>
    /// <para>
    /// Topics that hold the placeholder are tracked as given (see <see cref="GetSubscribedTopics"/>), and received publishes
    /// are matched against them using this device's current ID. To unsubscribe from such a topic, pass the same placeholder
    /// topic that was subscribed to.
    /// </para>
    /// <para>
    /// To keep using a specific device ID even if the device is later assigned a different one, put that ID in the topic instead of the placeholder.
    /// </para>
    /// </remarks>
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
        /// publishes on them before the SUBACK is processed. Topics are reported as they were passed to this client, so any
        /// <see cref="IConnectionClient.DeviceIdPlaceholder"/> they contain is not replaced.
        /// </remarks>
        public IReadOnlyList<string> GetSubscribedTopics => _subscribedCustomTopics;

        /// <summary>
        /// Publish to a custom topic.
        /// </summary>
        /// <param name="publish">
        /// The publish to send. Any <see cref="IConnectionClient.DeviceIdPlaceholder"/> in its topic is replaced with this
        /// device's current ID when it is sent.
        /// </param>
        /// <param name="cancellationToken">The cancellation token.</param>
        /// <exception cref="NotSupportedException">Thrown if the connection client is disconnected and is not trying to reconnect.</exception>
        public async Task<MqttPublishAck> PublishAsync(MqttPublish publish, CancellationToken cancellationToken = default)
        {
            ObjectDisposedException.ThrowIf(_isDisposed, this);
            cancellationToken.ThrowIfCancellationRequested();
            EnsureConnected();

            return await _connection.PublishAsync(publish, cancellationToken);
        }

        //TODO currently implementation loses all subscriptions upon losing connection. Fix later.
        /// <summary>
        /// Subscribe to a custom topic.
        /// </summary>
        /// <param name="topic">
        /// The topic filter to subscribe to. Any <see cref="IConnectionClient.DeviceIdPlaceholder"/> in it is replaced with
        /// this device's current ID when it is sent.
        /// </param>
        /// <param name="qos">The maximum QoS level to receive publishes on this topic with.</param>
        /// <param name="cancellationToken">The cancellation token.</param>
        /// <exception cref="NotSupportedException">Thrown if the connection client is disconnected and is not trying to reconnect.</exception>
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
        /// <summary>
        /// Subscribe to one or more custom topics.
        /// </summary>
        /// <param name="subscribe">
        /// The subscribe to send. Any <see cref="IConnectionClient.DeviceIdPlaceholder"/> in its topic filters is replaced
        /// with this device's current ID when it is sent.
        /// </param>
        /// <param name="cancellationToken">The cancellation token.</param>
        /// <returns>
        /// The SUBACK. Its topic filters are the ones that were sent, so any placeholder in them has been replaced.
        /// </returns>
        /// <exception cref="NotSupportedException">Thrown if the connection client is disconnected and is not trying to reconnect.</exception>
        public async Task<MqttSubscribeAck> SubscribeAsync(MqttSubscribe subscribe, CancellationToken cancellationToken = default)
        {
            //TODO probably want some kind of client-side validation of some topic strings, but only if AEG-returned error is too hard for user to interpret

            ObjectDisposedException.ThrowIf(_isDisposed, this);
            cancellationToken.ThrowIfCancellationRequested();
            EnsureConnected();

            await _subscriptionOperationLock.WaitAsync(cancellationToken);
            try
            {
                // This client may have been disposed, or the connection client disconnected, while waiting for a prior operation to finish
                ObjectDisposedException.ThrowIf(_isDisposed, this);
                EnsureConnected();

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
                    List<string> requestedTopics = subscribe.TopicFilters.Select(topicFilter => topicFilter.Topic).ToList();
                    int index = 0;
                    foreach (var subackItem in suback.Items)
                    {
                        if (subackItem.ReasonCode != MqttClientSubscribeReasonCode.GrantedQoS0 && subackItem.ReasonCode != MqttClientSubscribeReasonCode.GrantedQoS1 && subackItem.ReasonCode != MqttClientSubscribeReasonCode.GrantedQoS2)
                        {
                            builder.Remove(GetTrackedTopic(requestedTopics, suback.Items.Count, index, subackItem.TopicFilter.Topic));
                        }

                        index++;
                    }
                });

                return suback;
            }
            finally
            {
                _subscriptionOperationLock.Release();
            }
        }

        /// <summary>
        /// Unsubscribe from a custom topic.
        /// </summary>
        /// <param name="topic">
        /// The topic filter to unsubscribe from, exactly as it was subscribed to (including any
        /// <see cref="IConnectionClient.DeviceIdPlaceholder"/>). Any placeholder is replaced with this device's current ID
        /// when it is sent.
        /// </param>
        /// <param name="cancellationToken">The cancellation token.</param>
        /// <exception cref="NotSupportedException">Thrown if the connection client is disconnected and is not trying to reconnect.</exception>
        public async Task<MqttClientUnsubscribeReasonCode> UnsubscribeAsync(string topic, CancellationToken cancellationToken = default)
        {
            MqttUnsubscribe unsubscribe = new(topic);

            var unsuback = await UnsubscribeAsync(unsubscribe, cancellationToken);

            return unsuback.Items.FirstOrDefault()!.ReasonCode;
        }

        /// <summary>
        /// Unsubscribe from one or more custom topics.
        /// </summary>
        /// <param name="unsubscribe">
        /// The unsubscribe to send. Its topic filters must be exactly as they were subscribed to (including any
        /// <see cref="IConnectionClient.DeviceIdPlaceholder"/>). Any placeholder is replaced with this device's current ID
        /// when it is sent.
        /// </param>
        /// <param name="cancellationToken">The cancellation token.</param>
        /// <returns>
        /// The UNSUBACK. Its topic filters are the ones that were sent, so any placeholder in them has been replaced.
        /// </returns>
        /// <exception cref="NotSupportedException">Thrown if the connection client is disconnected and is not trying to reconnect.</exception>
        public async Task<MqttUnsubscribeAck> UnsubscribeAsync(MqttUnsubscribe unsubscribe, CancellationToken cancellationToken = default)
        {
            ObjectDisposedException.ThrowIf(_isDisposed, this);
            cancellationToken.ThrowIfCancellationRequested();
            EnsureConnected();

            await _subscriptionOperationLock.WaitAsync(cancellationToken);
            try
            {
                // This client may have been disposed, or the connection client disconnected, while waiting for a prior operation to finish
                ObjectDisposedException.ThrowIf(_isDisposed, this);
                EnsureConnected();

                // Keep tracking the topics until the UNSUBACK so publishes the service sends before then are still delivered
                var unsuback = await _connection.UnsubscribeAsync(unsubscribe, cancellationToken);

                UpdateSubscribedTopics(builder =>
                {
                    int index = 0;
                    foreach (var unsubackItem in unsuback.Items)
                    {
                        // NoSubscriptionExisted means the service no longer considers this client subscribed, so stop tracking it too
                        if (unsubackItem.ReasonCode == MqttClientUnsubscribeReasonCode.Success || unsubackItem.ReasonCode == MqttClientUnsubscribeReasonCode.NoSubscriptionExisted)
                        {
                            string trackedTopic = GetTrackedTopic(unsubscribe.TopicFilters, unsuback.Items.Count, index, unsubackItem.TopicFilter);
                            builder.RemoveAll(subscribedTopic => subscribedTopic == trackedTopic);
                        }

                        index++;
                    }
                });

                return unsuback;
            }
            finally
            {
                _subscriptionOperationLock.Release();
            }
        }

        // The connection context survives a disconnect, so IsConnectionTargetSet is what says whether the connection client
        // is still connected or reconnecting. Without it, operations after an explicit disconnect would wait indefinitely.
        private void EnsureConnected()
        {
            if (_connection.GetCurrentConnectionContext() == null || !_connection.IsConnectionTargetSet)
            {
                throw new NotSupportedException("Must be connected before calling this method.");
            }
        }

        /// <summary>
        /// Get the topic, as this client tracks it, that the ack item at <paramref name="index"/> answers.
        /// </summary>
        /// <remarks>
        /// The topic in an ack item is the one that was sent, so any <see cref="IConnectionClient.DeviceIdPlaceholder"/> in
        /// the requested topic has been replaced in it. MQTT acks answer each topic filter in the order it was requested, so
        /// when the ack has one item per requested topic filter, the requested topic at the same index is the tracked one.
        /// </remarks>
        private static string GetTrackedTopic(IReadOnlyList<string> requestedTopics, int ackItemCount, int index, string ackedTopic)
        {
            return requestedTopics.Count == ackItemCount ? requestedTopics[index] : ackedTopic;
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

            // Subscribed topics may hold the device ID placeholder, which must be matched using this device's current ID
            string? deviceId = _connection.GetCurrentConnectionContext()?.DeviceId;

            // Need matching logic since subscribed topics may include wildcard but the actual sent publish will expand from that wildcard
            // Enumerating the immutable snapshot is safe even if a subscribe/unsubscribe replaces the list concurrently
            foreach (string subscribedCustomTopic in _subscribedCustomTopics)
            {
                string subscribedCustomTopicFilter = subscribedCustomTopic;
                if (subscribedCustomTopic.Contains(IConnectionClient.DeviceIdPlaceholder, StringComparison.Ordinal))
                {
                    if (deviceId == null)
                    {
                        continue;
                    }

                    subscribedCustomTopicFilter = subscribedCustomTopic.Replace(IConnectionClient.DeviceIdPlaceholder, deviceId, StringComparison.Ordinal);
                }

                // TODO probably want to test scenarios where user subscribes to two custom topics and receives a publish that satisfies both topics (due to wildcard rules)
                //  This scenario should be fine for ACK'ing since we still only expose the publish to the user once even if they were subscribed to it in two ways
                if (DoesTopicMatchFilter(args.Publish.Topic, subscribedCustomTopicFilter))
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
