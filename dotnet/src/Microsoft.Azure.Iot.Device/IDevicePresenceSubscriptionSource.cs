// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Mqtt;

namespace Microsoft.Azure.Iot.Device
{
    /// <summary>
    /// A feature client whose subscriptions must be re-established each time the connection client re-establishes this
    /// device's presence on IoT hub without a resumed MQTT session.
    /// </summary>
    /// <remarks>
    /// Register with <see cref="AbstractConnectionClient.AddDevicePresenceSubscriptionSource"/>. The connection client
    /// sends these topic filters in the same SUBSCRIBE as its own device presence subscriptions, before the device
    /// presence flow completes, so they are in place before any operation waiting on the reconnection resumes.
    /// </remarks>
    internal interface IDevicePresenceSubscriptionSource
    {
        /// <summary>
        /// The topic filters to subscribe to. Called on the connection layer's thread just before the SUBSCRIBE is
        /// sent, so it must be fast and must not wait on any operation sent through the connection client.
        /// </summary>
        /// <remarks>
        /// Topics may contain the device ID placeholder, which the connection client replaces with the device's current ID.
        /// </remarks>
        /// <param name="isSessionPresent">
        /// Whether the broker resumed the previous MQTT session, in which case it may still hold subscriptions that this
        /// connection does not re-subscribe to.
        /// </param>
        IReadOnlyList<MqttTopicFilter> GetDevicePresenceTopicFilters(bool isSessionPresent);

        /// <summary>
        /// Called with the SUBACK result of each filter returned by <see cref="GetDevicePresenceTopicFilters"/>, in the
        /// same order, once the SUBSCRIBE that carried them has been acknowledged.
        /// </summary>
        /// <param name="topicFilters">The filters as returned by <see cref="GetDevicePresenceTopicFilters"/> (placeholders not replaced).</param>
        /// <param name="results">The SUBACK item for each filter in <paramref name="topicFilters"/>.</param>
        /// <param name="isSessionPresent">
        /// Whether the broker resumed the previous MQTT session, in which case a rejected filter's previous subscription
        /// may still be in place.
        /// </param>
        void OnDevicePresenceSubscribeCompleted(IReadOnlyList<MqttTopicFilter> topicFilters, IReadOnlyList<MqttSubscribeAckItem> results, bool isSessionPresent);
    }

    /// <summary>
    /// The topic filters one <see cref="IDevicePresenceSubscriptionSource"/> asked to have re-subscribed to.
    /// </summary>
    internal sealed record DevicePresenceSubscriptionRequest(IDevicePresenceSubscriptionSource Source, IReadOnlyList<MqttTopicFilter> TopicFilters);
}
