// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Mqtt;
using Microsoft.Azure.Iot.Device.Retry;

namespace Microsoft.Azure.Iot.Device.MQTTv5.Models
{
    /// <summary>
    /// Configures the MQTTv5-only connection client.
    /// </summary>
    public class ConnectionClientOptions
    {
        /// <inheritdoc cref="Unified.Models.ConnectionClientOptions.ConnectionRetryPolicy"/>
        public IRetryPolicy ConnectionRetryPolicy { get; set; } = new ExponentialBackoffRetryPolicy(uint.MaxValue, TimeSpan.FromSeconds(60));

        /// <inheritdoc cref="Unified.Models.ConnectionClientOptions.ConnectionAttemptTimeout"/>
        public TimeSpan ConnectionAttemptTimeout { get; set; } = TimeSpan.FromSeconds(10);

        /// <inheritdoc cref="Unified.Models.ConnectionClientOptions.EnableMqttLogging"/>
        public bool EnableMqttLogging { get; set; }

        /// <inheritdoc cref="Unified.Models.ConnectionClientOptions.MqttClient"/>
        public IMqttClient? MqttClient { get; set; }

        /// <inheritdoc cref="Unified.Models.ConnectionClientOptions.UseCleanSession"/>
        public bool UseCleanSession { get; set; }

        /// <summary>
        /// How many seconds an IoT hub session should persist after the connection is lost.
        /// Defaults to 3600 (one hour). Zero expires the session immediately; <see cref="uint.MaxValue"/> requests that it never expire.
        /// </summary>
        /// <remarks>
        /// A persisted session lets IoT hub redeliver unacknowledged QoS 1 messages after a transient disconnection, and
        /// lets the device presence flow skip re-subscribing when this process already subscribed on the resumed session.
        /// The broker may override the requested interval in its CONNACK. The interval also applies after this client
        /// disconnects, including when it is closed. Session resumption also requires <see cref="UseCleanSession"/> to be false.
        /// This value is read once when the connection client is constructed.
        /// </remarks>
        public uint SessionExpiryInterval { get; set; } = 3600;

        /// <summary>
        /// True (the default) to request desired properties upon connecting and subsequent desired property updates.
        /// Maps to the birth message's push_desired flag.
        /// </summary>
        /// <remarks>
        /// This value is read once when the connection client is constructed. It does not disable explicit twin requests.
        /// </remarks>
        public bool ReceiveDesiredPropertyUpdates { get; set; } = true;

        /// <summary>
        /// True (the default) to request reported properties upon connecting or reconnecting.
        /// Maps to the birth message's push_reported flag.
        /// </summary>
        /// <remarks>
        /// This value is read once when the connection client is constructed. It does not disable explicit twin requests.
        /// </remarks>
        public bool ReceiveReportedPropertiesUponConnect { get; set; } = true;

        internal Unified.Models.ConnectionClientOptions ToCommonOptions()
        {
            return new()
            {
                ConnectionRetryPolicy = ConnectionRetryPolicy,
                ConnectionAttemptTimeout = ConnectionAttemptTimeout,
                EnableMqttLogging = EnableMqttLogging,
                MqttClient = MqttClient,
                UseCleanSession = UseCleanSession,
            };
        }
    }
}
