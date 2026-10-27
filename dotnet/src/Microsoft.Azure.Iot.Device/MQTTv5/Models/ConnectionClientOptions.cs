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
        /// <inheritdoc cref="Device.Models.ConnectionClientOptions.ConnectionRetryPolicy"/>
        public IRetryPolicy ConnectionRetryPolicy { get; set; } = new ExponentialBackoffRetryPolicy(uint.MaxValue, TimeSpan.FromSeconds(60));

        /// <inheritdoc cref="Device.Models.ConnectionClientOptions.ConnectionAttemptTimeout"/>
        public TimeSpan ConnectionAttemptTimeout { get; set; } = TimeSpan.FromSeconds(10);

        /// <inheritdoc cref="Device.Models.ConnectionClientOptions.EnableMqttLogging"/>
        public bool EnableMqttLogging { get; set; }

        /// <inheritdoc cref="Device.Models.ConnectionClientOptions.MqttClient"/>
        public IMqttClient? MqttClient { get; set; }

        /// <inheritdoc cref="Device.Models.ConnectionClientOptions.EnableTwin"/>
        public bool EnableTwin { get; set; } = true;

        /// <inheritdoc cref="Device.Models.ConnectionClientOptions.EnableDirectMethods"/>
        public bool EnableDirectMethods { get; set; } = true;

        /// <inheritdoc cref="Device.Models.ConnectionClientOptions.EnableCloudToDeviceMessages"/>
        public bool EnableCloudToDeviceMessages { get; set; } = true;

        /// <inheritdoc cref="Device.Models.ConnectionClientOptions.UseCleanSession"/>
        public bool UseCleanSession { get; set; }

        /// <summary>
        /// How many seconds an IoT hub session should persist after the connection is lost.
        /// Zero (the default) expires the session immediately; <see cref="uint.MaxValue"/> requests that it never expire.
        /// </summary>
        /// <remarks>
        /// The broker may override the requested interval in its CONNACK. Explicit disconnections and session resets
        /// still expire the session immediately. Session resumption also requires <see cref="UseCleanSession"/> to be false.
        /// This value is read once when the connection client is constructed.
        /// </remarks>
        public uint SessionExpiryInterval { get; set; }

        /// <summary>
        /// True (the default) to request desired properties upon connecting and subsequent desired property updates.
        /// Maps to the birth message's push_desired flag.
        /// </summary>
        /// <remarks>
        /// Ignored when <see cref="EnableTwin"/> is false. This value is read once when the connection client is constructed.
        /// </remarks>
        public bool ReceiveDesiredPropertyUpdates { get; set; } = true;

        /// <summary>
        /// True (the default) to request reported properties upon connecting or reconnecting.
        /// Maps to the birth message's push_reported flag.
        /// </summary>
        /// <remarks>
        /// Ignored when <see cref="EnableTwin"/> is false. This value is read once when the connection client is constructed.
        /// </remarks>
        public bool ReceiveReportedPropertiesUponConnect { get; set; } = true;

        internal Device.Models.ConnectionClientOptions ToCommonOptions()
        {
            return new()
            {
                ConnectionRetryPolicy = ConnectionRetryPolicy,
                ConnectionAttemptTimeout = ConnectionAttemptTimeout,
                EnableMqttLogging = EnableMqttLogging,
                MqttClient = MqttClient,
                EnableTwin = EnableTwin,
                EnableDirectMethods = EnableDirectMethods,
                EnableCloudToDeviceMessages = EnableCloudToDeviceMessages,
                UseCleanSession = UseCleanSession,
            };
        }
    }
}
