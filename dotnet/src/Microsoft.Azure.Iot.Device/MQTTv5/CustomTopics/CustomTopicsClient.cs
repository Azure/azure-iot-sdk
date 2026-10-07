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
    // Other variables besides {deviceId} ?
    // Does AEG allow sending publishes to self? Would be useful for testing/samples but users probably wouldn't

    // Usha is PM, check for terms I suppose
    public class CustomTopicsClient : IDisposable
    {
        private bool _isDisposed = false;

        private IConnectionClient _connection;

        // Immutable so that readers (such as the publish received handler) can safely enumerate a snapshot while
        // subscribe/unsubscribe calls concurrently replace it. Writers must hold _subscribedCustomTopicsLock.
        private volatile ImmutableList<string> _subscribedCustomTopics = ImmutableList<string>.Empty;

        private readonly object _subscribedCustomTopicsLock = new();

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

            var suback = await _connection.SubscribeAsync(subscribe, cancellationToken);
            lock (_subscribedCustomTopicsLock)
            {
                var builder = _subscribedCustomTopics.ToBuilder();
                foreach (var subackItem in suback.Items)
                {
                    if (subackItem.ReasonCode == MqttClientSubscribeReasonCode.GrantedQoS0 || subackItem.ReasonCode == MqttClientSubscribeReasonCode.GrantedQoS1 || subackItem.ReasonCode == MqttClientSubscribeReasonCode.GrantedQoS2)
                    {
                        builder.Add(subackItem.TopicFilter.Topic);
                    }
                }

                _subscribedCustomTopics = builder.ToImmutable();
            }

            return suback;
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

            var unsuback = await _connection.UnsubscribeAsync(unsubscribe, cancellationToken);
            lock (_subscribedCustomTopicsLock)
            {
                var builder = _subscribedCustomTopics.ToBuilder();
                foreach (var unsubackItem in unsuback.Items)
                {
                    // NoSubscriptionExisted means the service no longer considers this client subscribed, so stop tracking it too
                    if (unsubackItem.ReasonCode == MqttClientUnsubscribeReasonCode.Success || unsubackItem.ReasonCode == MqttClientUnsubscribeReasonCode.NoSubscriptionExisted)
                    {
                        builder.RemoveAll(subscribedTopic => subscribedTopic == unsubackItem.TopicFilter);
                    }
                }

                _subscribedCustomTopics = builder.ToImmutable();
            }

            return unsuback;
        }

        //TODO unsubscribe

        /// <summary>
        /// Releases the unmanaged resources used by this client and optionally disposes of the managed resources.
        /// </summary>
        /// <param name="disposing">true to release both managed and unmanaged resources; false to releases only unmanaged resources.</param>
        public void Dispose(bool disposing)
        {
            if (disposing)
            {
                _connection.Dispose();
            }

            _connection.PublishReceivedAsync -= HandleReceivedMqttPublish;

            _isDisposed = true;
        }

        /// <summary>
        /// Releases the unmanaged resources and disposes of the managed resources used by this client 
        /// </summary>
        public void Dispose()
        {
            _connection.Dispose();

            _connection.PublishReceivedAsync -= HandleReceivedMqttPublish;

            _isDisposed = true;
        }

        private async Task HandleReceivedMqttPublish(MqttPublishReceivedEventArgs args)
        {
            // Need matching logic since subscribed topics may include wildcard but the actual sent publish will expand from that wildcard
            // Enumerating the immutable snapshot is safe even if a subscribe/unsubscribe replaces the list concurrently
            foreach (string subscribedCustomTopic in _subscribedCustomTopics)
            {
                // TODO probably want to test scenarios where user subscribes to two custom topics and receives a publish that satisfies both topics (due to wildcard rules)
                //  This scenario should be fine for ACK'ing since we still only expose the publish to the user once even if they were subscribed to it in two ways
                if (DoesTopicMatchFilter(args.Publish.Topic, subscribedCustomTopic))
                {
                    if (CustomTopicPublishReceivedAsync != null)
                    {
                        await CustomTopicPublishReceivedAsync.Invoke(args);
                    }
                    else
                    {
                        Trace.TraceError("Received a publish on a custom topic, but user did not setup CustomTopicPublishReceivedAsync to receive it. The publish will be acknowledged and will not be re-delivered.");
                    }

                    // For now, just ack for the user (regardless of if they have callback set). Probably needs more thought
                    await args.AcknowledgeAsync(CancellationToken.None);

                    // Don't execute message received callback again even if that same message satisfies multiple topic strings
                    break;
                }
            }
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
