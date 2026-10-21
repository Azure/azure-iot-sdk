// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Exceptions;
using Microsoft.Azure.Iot.Device.Mqtt;
using Microsoft.Azure.Iot.Device.MQTTv5.Connection;
using MQTTnet;
using System.Collections;
using System.Collections.Immutable;
using System.Diagnostics;
using System.Reflection.Metadata.Ecma335;

namespace Microsoft.Azure.Iot.Device.MQTTv5.CustomTopics
{
    // Open questions:
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
    /// <para>
    /// MQTT-level results are returned as-is. A PUBACK, SUBACK or UNSUBACK whose reason code reports a failure (for example,
    /// <see cref="MqttClientSubscribeReasonCode.NotAuthorized"/>) is returned to the caller rather than thrown as an
    /// exception, and the operation is not retried. Connection-level failures are still handled by the connection client:
    /// an operation that cannot be sent because the device is not connected waits for the connection to be re-established
    /// and is then sent. An operation that the connection drops after it was sent is not re-sent; it fails instead. See the
    /// remarks on each operation for how it behaves in exceptional cases.
    /// </para>
    /// <para>
    /// Subscriptions survive connection loss. Whenever the connection client re-establishes this device's presence on IoT
    /// hub without a resumed session (including after re-provisioning), it re-subscribes to every topic in
    /// <see cref="GetSubscribedTopics"/> in the same SUBSCRIBE as its own device presence subscriptions, with the QoS each
    /// topic was most recently successfully subscribed to with. Operations that were waiting on the reconnection resume only after that. A
    /// topic that IoT hub rejects upon re-subscribing is no longer tracked as subscribed. A topic whose unsubscribe is in
    /// flight is not re-subscribed to, and stops being tracked even if that unsubscribe then fails.
    /// </para>
    /// </remarks>
    public class CustomTopicsClient : IDisposable, IDevicePresenceSubscriptionSource
    {
        // Volatile because it is read by the publish received handler and by in-flight operations on other threads. Only set
        // while holding _subscribedCustomTopicsLock so that no tracked topic update can land after disposal.
        private volatile bool _isDisposed = false;

        private IConnectionClient _connection;

        // Immutable so that readers (such as the publish received handler) can safely enumerate a snapshot while
        // subscribe/unsubscribe calls concurrently replace it. Writers must hold _subscribedCustomTopicsLock.
        private volatile ImmutableList<string> _subscribedCustomTopics = ImmutableList<string>.Empty;

        private readonly object _subscribedCustomTopicsLock = new();

        // The QoS to re-subscribe to each tracked topic with upon reconnecting. Pruned along with _subscribedCustomTopics.
        // Guarded by _subscribedCustomTopicsLock.
        private ImmutableDictionary<string, MqttQualityOfServiceLevel> _subscribedCustomTopicQos = ImmutableDictionary<string, MqttQualityOfServiceLevel>.Empty;

        // The unsubscribe currently in flight, if any. Guarded by _subscribedCustomTopicsLock.
        private PendingUnsubscribe? _pendingUnsubscribe;

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

            // Fold this client's subscriptions into the device presence flow so they are re-established upon reconnecting
            (_connection as AbstractConnectionClient)?.AddDevicePresenceSubscriptionSource(this);
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
        /// <param name="cancellationToken">
        /// The cancellation token. This also bounds how long this method waits for a lost connection to be re-established.
        /// </param>
        /// <returns>
        /// The PUBACK, whatever its reason code. For a QoS 0 publish, which IoT hub does not acknowledge, this is a
        /// successful PUBACK created once the PUBLISH has been sent.
        /// </returns>
        /// <remarks>
        /// <para>How this method behaves in exceptional cases:</para>
        /// <list type="bullet">
        /// <item><description>
        /// <b>A temporary disconnection.</b> If this device is not connected to IoT hub when the PUBLISH is about to be
        /// sent (for example, because the connection dropped and is being re-established, or because the device is being
        /// re-provisioned), this method waits until the connection client has re-established this device's presence on IoT
        /// hub, then sends the PUBLISH. Any <see cref="IConnectionClient.DeviceIdPlaceholder"/> is replaced with the
        /// device ID at that time. However, if the connection drops after the PUBLISH was sent but before its PUBACK
        /// arrived, the PUBLISH is not re-sent. Instead, this method throws a <see cref="DeviceException"/> whose
        /// <see cref="DeviceException.Retryability"/> is <see cref="ErrorRetryability.Retryable"/>. IoT hub may or may not
        /// have received that PUBLISH, so publishing it again may deliver it twice.
        /// </description></item>
        /// <item><description>
        /// <b>A fatal disconnection.</b> If the connection client hits a fault that it will not recover from (which it
        /// reports through <see cref="AbstractConnectionClient.ConnectionFaultedAsync"/>) while this method waits for the
        /// connection, this method stops waiting. It throws an <see cref="OperationCanceledException"/> whose
        /// <see cref="Exception.InnerException"/> is the <see cref="DeviceException"/> that describes the fault. Calls made
        /// after such a fault fail without waiting, until the application connects the connection client again.
        /// </description></item>
        /// <item><description>
        /// <b>A PUBACK with a failure reason code.</b> The PUBACK is returned as-is. This method does not throw, and the
        /// PUBLISH is not retried. Check <see cref="MqttPublishAck.ReasonCode"/> to find out whether IoT hub accepted it.
        /// </description></item>
        /// <item><description>
        /// <b>A deliberate disconnection by the application.</b> If <see cref="AbstractConnectionClient.DisconnectAsync"/>
        /// has been called before this method is called, this method throws a <see cref="NotSupportedException"/> without
        /// sending anything. If it is called while the PUBLISH is awaiting its PUBACK, this method throws a
        /// <see cref="DeviceException"/>, as described above for a dropped connection. If it is called while this method is
        /// waiting for a lost connection to be re-established, this method keeps waiting until the application connects
        /// the connection client again, the connection client faults, or <paramref name="cancellationToken"/> is canceled.
        /// Use a cancellable token so that this wait is bounded. If it is called just as this method finds the connection
        /// gone, before this method has started waiting, this method throws a <see cref="MqttClientNotConnectedException"/>
        /// instead.
        /// </description></item>
        /// </list>
        /// </remarks>
        /// <exception cref="NotSupportedException">
        /// Thrown if the connection client is disconnected and is not trying to reconnect, for example because
        /// <see cref="AbstractConnectionClient.DisconnectAsync"/> was called.
        /// </exception>
        /// <exception cref="MqttClientNotConnectedException">
        /// Thrown if <see cref="AbstractConnectionClient.DisconnectAsync"/> is called just as this operation finds the
        /// connection gone, before it has started waiting for the connection to be re-established.
        /// </exception>
        /// <exception cref="OperationCanceledException">
        /// Thrown if <paramref name="cancellationToken"/> is canceled, or if the connection client hits a fault that it
        /// will not recover from while this operation is waiting for the connection. In the latter case, the
        /// <see cref="Exception.InnerException"/> is the <see cref="DeviceException"/> describing that fault.
        /// </exception>
        /// <exception cref="DeviceException">
        /// Thrown if this operation fails for any reason other than an MQTT reason code, for example because the connection
        /// dropped while the operation was in flight.
        /// </exception>
        /// <exception cref="ObjectDisposedException">Thrown if this client has been disposed.</exception>
        public async Task<MqttPublishAck> PublishAsync(MqttPublish publish, CancellationToken cancellationToken = default)
        {
            ObjectDisposedException.ThrowIf(_isDisposed, this);
            cancellationToken.ThrowIfCancellationRequested();
            EnsureConnected();

            // Unlike other feature clients, this client hands MQTT-level results to the user as-is
            return await _connection.PublishAsync(publish, throwOnErrorReasonCode: false, cancellationToken);
        }

        /// <summary>
        /// Subscribe to a custom topic.
        /// </summary>
        /// <param name="topic">
        /// The topic filter to subscribe to. Any <see cref="IConnectionClient.DeviceIdPlaceholder"/> in it is replaced with
        /// this device's current ID when it is sent.
        /// </param>
        /// <param name="qos">The maximum QoS level to receive publishes on this topic with.</param>
        /// <param name="cancellationToken">
        /// The cancellation token. This also bounds how long this method waits for a lost connection to be re-established.
        /// </param>
        /// <returns>The reason code IoT hub returned for the topic, whether or not it granted the subscription.</returns>
        /// <remarks>
        /// This behaves exactly as <see cref="SubscribeAsync(MqttSubscribe, CancellationToken)"/> does in exceptional cases:
        /// <inheritdoc cref="SubscribeAsync(MqttSubscribe, CancellationToken)" path="/remarks/list"/>
        /// </remarks>
        /// <inheritdoc cref="SubscribeAsync(MqttSubscribe, CancellationToken)" path="/exception"/>
        /// <exception cref="InvalidOperationException">Thrown if the SUBACK does not hold exactly one reason code.</exception>
        public async Task<MqttClientSubscribeReasonCode> SubscribeAsync(string topic, MqttQualityOfServiceLevel qos, CancellationToken cancellationToken = default)
        {
            MqttSubscribe subscribe = new()
            {
                TopicFilters = new() { { new MqttTopicFilter(topic, qos) } }
            };

            MqttSubscribeAck suback = await SubscribeAsync(subscribe, cancellationToken);
            if (suback.Items.Count != 1)
            {
                throw new InvalidOperationException("IoT Hub returned a suback with an unexpected number of subscriptions granted");
            }

            return suback.Items.First().ReasonCode;
        }

        /// <summary>
        /// Subscribe to one or more custom topics.
        /// </summary>
        /// <param name="subscribe">
        /// The subscribe to send. Any <see cref="IConnectionClient.DeviceIdPlaceholder"/> in its topic filters is replaced
        /// with this device's current ID when it is sent.
        /// </param>
        /// <param name="cancellationToken">
        /// The cancellation token. This also bounds how long this method waits for a lost connection to be re-established,
        /// and for any subscribe or unsubscribe that is already in progress on this client to finish.
        /// </param>
        /// <returns>
        /// The SUBACK, whatever its reason codes. Its topic filters are the ones that were sent, so any placeholder in them
        /// has been replaced.
        /// </returns>
        /// <remarks>
        /// <para>How this method behaves in exceptional cases:</para>
        /// <list type="bullet">
        /// <item><description>
        /// <b>A temporary disconnection.</b> If this device is not connected to IoT hub when the SUBSCRIBE is about to be
        /// sent (for example, because the connection dropped and is being re-established, or because the device is being
        /// re-provisioned), this method waits until the connection client has re-established this device's presence on IoT
        /// hub, then sends the SUBSCRIBE. Any <see cref="IConnectionClient.DeviceIdPlaceholder"/> is replaced with the
        /// device ID at that time. However, if the connection drops after the SUBSCRIBE was sent but before its SUBACK
        /// arrived, the SUBSCRIBE is not re-sent. Instead, this method throws a <see cref="DeviceException"/> whose
        /// <see cref="DeviceException.Retryability"/> is         <see cref="ErrorRetryability.Retryable"/>, and this request leaves
                /// <see cref="GetSubscribedTopics"/> as it was. Subscribe again to retry.
        /// </description></item>
        /// <item><description>
        /// <b>A fatal disconnection.</b> If the connection client hits a fault that it will not recover from (which it
        /// reports through <see cref="AbstractConnectionClient.ConnectionFaultedAsync"/>) while this method waits for the
        /// connection, this method stops waiting. It throws an <see cref="OperationCanceledException"/> whose
        /// <see cref="Exception.InnerException"/> is the <see cref="DeviceException"/>         that describes the fault, and this
                /// request leaves <see cref="GetSubscribedTopics"/> as it was. Calls made after such a fault fail without waiting, until the
        /// application connects the connection client again.
        /// </description></item>
        /// <item><description>
        /// <b>A SUBACK with a failure reason code.</b> The SUBACK is returned as-is. This method does not throw, and the
        /// SUBSCRIBE is not retried. Check the reason code of each item in <see cref="MqttSubscribeAck.Items"/> to find
        /// out which topics IoT hub granted. Topics that were not granted are not tracked as subscribed (see
        /// <see cref="GetSubscribedTopics"/>), so they are not re-subscribed to when the connection is re-established.
        /// </description></item>
        /// <item><description>
        /// <b>A deliberate disconnection by the application.</b> If <see cref="AbstractConnectionClient.DisconnectAsync"/>
        /// has been called before this method sends the SUBSCRIBE (including while this method waits for an earlier
        /// subscribe or unsubscribe on this client to finish), this method throws a <see cref="NotSupportedException"/>
        /// without sending anything. If it is called while the SUBSCRIBE is awaiting its SUBACK, this method throws a
        /// <see cref="DeviceException"/>, as described above for a dropped connection. If it is called while this method
        /// is waiting for a lost connection to be re-established, this method keeps waiting until the application connects
        /// the connection client again, the connection client faults, or <paramref name="cancellationToken"/> is canceled.
        /// Use a cancellable token so that this wait is bounded. If it is called just as this method finds the connection
        /// gone, before this method has started waiting, this method throws a <see cref="MqttClientNotConnectedException"/>
        /// instead. In every case where this method throws, this request leaves <see cref="GetSubscribedTopics"/> as it was.
        /// </description></item>
        /// </list>
        /// </remarks>
        /// <exception cref="NotSupportedException">
        /// Thrown if the connection client is disconnected and is not trying to reconnect, for example because
        /// <see cref="AbstractConnectionClient.DisconnectAsync"/> was called.
        /// </exception>
        /// <exception cref="MqttClientNotConnectedException">
        /// Thrown if <see cref="AbstractConnectionClient.DisconnectAsync"/> is called just as this operation finds the
        /// connection gone, before it has started waiting for the connection to be re-established.
        /// </exception>
        /// <exception cref="OperationCanceledException">
        /// Thrown if <paramref name="cancellationToken"/> is canceled, or if the connection client hits a fault that it
        /// will not recover from while this operation is waiting for the connection. In the latter case, the
        /// <see cref="Exception.InnerException"/> is the <see cref="DeviceException"/> describing that fault.
        /// </exception>
        /// <exception cref="DeviceException">
        /// Thrown if this operation fails for any reason other than an MQTT reason code, for example because the connection
        /// dropped while the operation was in flight.
        /// </exception>
        /// <exception cref="ObjectDisposedException">Thrown if this client has been disposed.</exception>
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
                UpdateSubscribedTopics((builder, qosBuilder) =>
                {
                    foreach (var topicFilter in subscribe.TopicFilters)
                    {
                        builder.Add(topicFilter.Topic);

                        // An in-flight topic may need to be re-subscribed to before its SUBACK arrives. An already tracked
                        // topic keeps its current QoS until this request is granted.
                        qosBuilder.TryAdd(topicFilter.Topic, topicFilter.QualityOfServiceLevel);
                    }
                });

                MqttSubscribeAck suback;
                try
                {
                    suback = await _connection.SubscribeAsync(subscribe, throwOnErrorReasonCode: false, cancellationToken);
                }
                catch
                {
                    UpdateSubscribedTopics((builder, _) =>
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
                UpdateSubscribedTopics((builder, qosBuilder) =>
                {
                    List<string> requestedTopics = subscribe.TopicFilters.Select(topicFilter => topicFilter.Topic).ToList();
                    int index = 0;
                    foreach (var subackItem in suback.Items)
                    {
                        string trackedTopic = GetTrackedTopic(requestedTopics, suback.Items.Count, index, subackItem.TopicFilter.Topic);
                        if (IsGranted(subackItem.ReasonCode))
                        {
                            MqttTopicFilter? requestedFilter = subscribe.TopicFilters.Count == suback.Items.Count
                                ? subscribe.TopicFilters[index]
                                : subscribe.TopicFilters.FirstOrDefault(topicFilter => topicFilter.Topic == trackedTopic);
                            if (requestedFilter != null && builder.Contains(trackedTopic))
                            {
                                qosBuilder[trackedTopic] = requestedFilter.QualityOfServiceLevel;
                            }
                        }
                        else
                        {
                            builder.Remove(trackedTopic);
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
        /// <param name="cancellationToken">
        /// The cancellation token. This also bounds how long this method waits for a lost connection to be re-established.
        /// </param>
        /// <returns>The reason code IoT hub returned for the topic, whether or not the unsubscribe succeeded.</returns>
        /// <remarks>
        /// This behaves exactly as <see cref="UnsubscribeAsync(MqttUnsubscribe, CancellationToken)"/> does in exceptional cases:
        /// <inheritdoc cref="UnsubscribeAsync(MqttUnsubscribe, CancellationToken)" path="/remarks/list"/>
        /// </remarks>
        /// <inheritdoc cref="UnsubscribeAsync(MqttUnsubscribe, CancellationToken)" path="/exception"/>
        /// <exception cref="InvalidOperationException">Thrown if the UNSUBACK does not hold exactly one reason code.</exception>
        public async Task<MqttClientUnsubscribeReasonCode> UnsubscribeAsync(string topic, CancellationToken cancellationToken = default)
        {
            MqttUnsubscribe unsubscribe = new(topic);

            var unsuback = await UnsubscribeAsync(unsubscribe, cancellationToken);

            if (unsuback.Items.Count != 1)
            {
                throw new InvalidOperationException("IoT Hub returned a suback with an unexpected number of unsubscriptions granted");
            }

            return unsuback.Items.First().ReasonCode;
        }

        /// <summary>
        /// Unsubscribe from one or more custom topics.
        /// </summary>
        /// <param name="unsubscribe">
        /// The unsubscribe to send. Its topic filters must be exactly as they were subscribed to (including any
        /// <see cref="IConnectionClient.DeviceIdPlaceholder"/>). Any placeholder is replaced with this device's current ID
        /// when it is sent.
        /// </param>
        /// <param name="cancellationToken">
        /// The cancellation token. This also bounds how long this method waits for a lost connection to be re-established,
        /// and for any subscribe or unsubscribe that is already in progress on this client to finish.
        /// </param>
        /// <returns>
        /// The UNSUBACK, whatever its reason codes. Its topic filters are the ones that were sent, so any placeholder in
        /// them has been replaced.
        /// </returns>
        /// <remarks>
        /// <para>How this method behaves in exceptional cases:</para>
        /// <list type="bullet">
        /// <item><description>
        /// <b>A temporary disconnection.</b> If this device is not connected to IoT hub when the UNSUBSCRIBE is about to be
        /// sent (for example, because the connection dropped and is being re-established, or because the device is being
        /// re-provisioned), this method waits until the connection client has re-established this device's presence on IoT
        /// hub, then sends the UNSUBSCRIBE. Any <see cref="IConnectionClient.DeviceIdPlaceholder"/> is replaced with the
        /// device ID at that time. The topics being unsubscribed from are not re-subscribed to upon that reconnection, so
        /// they stop being tracked as subscribed (see <see cref="GetSubscribedTopics"/>) whatever the outcome of this
        /// method. However, if the connection drops after the UNSUBSCRIBE was sent but before its UNSUBACK arrived, the
        /// UNSUBSCRIBE is not re-sent. Instead, this method throws a <see cref="DeviceException"/> whose
        /// <see cref="DeviceException.Retryability"/> is <see cref="ErrorRetryability.Retryable"/>, the topics stay tracked
        /// as subscribed, and they are re-subscribed to when the connection is re-established. Unsubscribe again to retry.
        /// </description></item>
        /// <item><description>
        /// <b>A fatal disconnection.</b> If the connection client hits a fault that it will not recover from (which it
        /// reports through <see cref="AbstractConnectionClient.ConnectionFaultedAsync"/>) while this method waits for the
        /// connection, this method stops waiting. It throws an <see cref="OperationCanceledException"/> whose
        /// <see cref="Exception.InnerException"/> is the <see cref="DeviceException"/> that describes the fault. Calls made
        /// after such a fault fail without waiting, until the application connects the connection client again.
        /// </description></item>
        /// <item><description>
        /// <b>An UNSUBACK with a failure reason code.</b> The UNSUBACK is returned as-is. This method does not throw, and
        /// the UNSUBSCRIBE is not retried. Check the reason code of each item in <see cref="MqttUnsubscribeAck.Items"/> to
        /// find out which topics IoT hub unsubscribed this device from. Topics whose reason code is
        /// <see cref="MqttClientUnsubscribeReasonCode.Success"/> or
        /// <see cref="MqttClientUnsubscribeReasonCode.NoSubscriptionExisted"/> stop being tracked as subscribed. Any other
        /// topic stays tracked, so it keeps being delivered and is re-subscribed to when the connection is re-established.
        /// </description></item>
        /// <item><description>
        /// <b>A deliberate disconnection by the application.</b> If <see cref="AbstractConnectionClient.DisconnectAsync"/>
        /// has been called before this method sends the UNSUBSCRIBE (including while this method waits for an earlier
        /// subscribe or unsubscribe on this client to finish), this method throws a <see cref="NotSupportedException"/>
        /// without sending anything, and the topics stay tracked as subscribed. If it is called while the UNSUBSCRIBE is
        /// awaiting its UNSUBACK, this method throws a <see cref="DeviceException"/>, as described above for a dropped
        /// connection. If it is called while this method is waiting for a lost connection to be re-established, this
        /// method keeps waiting until the application connects the connection client again, the connection client faults,
        /// or <paramref name="cancellationToken"/> is canceled. Use a cancellable token so that this wait is bounded. If it
        /// is called just as this method finds the connection gone, before this method has started waiting, this method
        /// throws a <see cref="MqttClientNotConnectedException"/> instead.
        /// </description></item>
        /// </list>
        /// </remarks>
        /// <exception cref="NotSupportedException">
        /// Thrown if the connection client is disconnected and is not trying to reconnect, for example because
        /// <see cref="AbstractConnectionClient.DisconnectAsync"/> was called.
        /// </exception>
        /// <exception cref="MqttClientNotConnectedException">
        /// Thrown if <see cref="AbstractConnectionClient.DisconnectAsync"/> is called just as this operation finds the
        /// connection gone, before it has started waiting for the connection to be re-established.
        /// </exception>
        /// <exception cref="OperationCanceledException">
        /// Thrown if <paramref name="cancellationToken"/> is canceled, or if the connection client hits a fault that it
        /// will not recover from while this operation is waiting for the connection. In the latter case, the
        /// <see cref="Exception.InnerException"/> is the <see cref="DeviceException"/> describing that fault.
        /// </exception>
        /// <exception cref="DeviceException">
        /// Thrown if this operation fails for any reason other than an MQTT reason code, for example because the connection
        /// dropped while the operation was in flight.
        /// </exception>
        /// <exception cref="ObjectDisposedException">Thrown if this client has been disposed.</exception>
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

                // Keep tracking the topics until the UNSUBACK so publishes the service sends before then are still delivered.
                // Meanwhile, a reconnection must not re-subscribe to them, or this UNSUBSCRIBE could be undone.
                PendingUnsubscribe pendingUnsubscribe = new(unsubscribe.TopicFilters);
                lock (_subscribedCustomTopicsLock)
                {
                    _pendingUnsubscribe = pendingUnsubscribe;
                }

                MqttUnsubscribeAck unsuback;
                try
                {
                    unsuback = await _connection.UnsubscribeAsync(unsubscribe, throwOnErrorReasonCode: false, cancellationToken);
                }
                finally
                {
                    lock (_subscribedCustomTopicsLock)
                    {
                        _pendingUnsubscribe = null;
                    }

                    // A reconnection that skipped these topics left them unsubscribed on the service, whatever the
                    // outcome of this UNSUBSCRIBE, so stop tracking them.
                    if (pendingUnsubscribe.WasSkippedByResubscription)
                    {
                        UpdateSubscribedTopics((builder, _) => builder.RemoveAll(pendingUnsubscribe.Topics.Contains));
                    }
                }

                UpdateSubscribedTopics((builder, _) =>
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

        private static bool IsGranted(MqttClientSubscribeReasonCode reasonCode)
        {
            return reasonCode == MqttClientSubscribeReasonCode.GrantedQoS0
                || reasonCode == MqttClientSubscribeReasonCode.GrantedQoS1
                || reasonCode == MqttClientSubscribeReasonCode.GrantedQoS2;
        }

        private void UpdateSubscribedTopics(Action<ImmutableList<string>.Builder, ImmutableDictionary<string, MqttQualityOfServiceLevel>.Builder> update)
        {
            lock (_subscribedCustomTopicsLock)
            {
                // An operation that was already in flight when this client was disposed must not repopulate the tracked topics
                if (_isDisposed)
                {
                    return;
                }

                var builder = _subscribedCustomTopics.ToBuilder();
                var qosBuilder = _subscribedCustomTopicQos.ToBuilder();
                update(builder, qosBuilder);

                // Forget the QoS of topics that are no longer tracked at all
                HashSet<string> trackedTopics = new(builder, StringComparer.Ordinal);
                qosBuilder.RemoveRange(qosBuilder.Keys.Where(topic => !trackedTopics.Contains(topic)).ToList());

                _subscribedCustomTopics = builder.ToImmutable();
                _subscribedCustomTopicQos = qosBuilder.ToImmutable();
            }
        }

        IReadOnlyList<MqttTopicFilter> IDevicePresenceSubscriptionSource.GetDevicePresenceTopicFilters()
        {
            lock (_subscribedCustomTopicsLock)
            {
                if (_isDisposed)
                {
                    return Array.Empty<MqttTopicFilter>();
                }

                List<MqttTopicFilter> topicFilters = new();
                HashSet<string> addedTopics = new(StringComparer.Ordinal);
                foreach (string topic in _subscribedCustomTopics)
                {
                    if (_pendingUnsubscribe != null && _pendingUnsubscribe.Topics.Contains(topic))
                    {
                        _pendingUnsubscribe.WasSkippedByResubscription = true;
                        continue;
                    }

                    if (addedTopics.Add(topic))
                    {
                        MqttQualityOfServiceLevel qos = _subscribedCustomTopicQos.TryGetValue(topic, out MqttQualityOfServiceLevel trackedQos)
                            ? trackedQos
                            : MqttQualityOfServiceLevel.AtLeastOnce;
                        topicFilters.Add(new MqttTopicFilter(topic, qos));
                    }
                }

                return topicFilters;
            }
        }

        void IDevicePresenceSubscriptionSource.OnDevicePresenceSubscribeCompleted(IReadOnlyList<MqttTopicFilter> topicFilters, IReadOnlyList<MqttSubscribeAckItem> results)
        {
            List<string> rejectedTopics = new();
            for (int i = 0; i < topicFilters.Count; i++)
            {
                if (!IsGranted(results[i].ReasonCode))
                {
                    Trace.TraceWarning("IoT hub rejected re-subscribing to custom topic {0} upon reconnecting with reason code {1}. It is no longer subscribed to.", topicFilters[i].Topic, results[i].ReasonCode);
                    rejectedTopics.Add(topicFilters[i].Topic);
                }
            }

            if (rejectedTopics.Count > 0)
            {
                UpdateSubscribedTopics((builder, _) => builder.RemoveAll(rejectedTopics.Contains));
            }
        }

        // The topics of the unsubscribe currently in flight, and whether a reconnection has skipped re-subscribing to them
        private sealed class PendingUnsubscribe
        {
            public PendingUnsubscribe(IEnumerable<string> topics)
            {
                Topics = new HashSet<string>(topics, StringComparer.Ordinal);
            }

            public HashSet<string> Topics { get; }

            // Guarded by _subscribedCustomTopicsLock
            public bool WasSkippedByResubscription { get; set; }
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
                _subscribedCustomTopicQos = ImmutableDictionary<string, MqttQualityOfServiceLevel>.Empty;
            }

            (_connection as AbstractConnectionClient)?.RemoveDevicePresenceSubscriptionSource(this);
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

                // Overlapping subscriptions (for example, through wildcards) can match one publish more than once, but the
                // publish is still only exposed to the user, and acknowledged, once
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
