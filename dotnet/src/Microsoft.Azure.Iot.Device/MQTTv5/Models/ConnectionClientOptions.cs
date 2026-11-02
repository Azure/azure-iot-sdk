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
        /// True to have IoT hub push the desired properties upon connecting (when they changed since the version this device
        /// holds) and each desired properties patch while connected. False (the default) for pull mode.
        /// Maps to the birth message's push_desired flag.
        /// </summary>
        /// <remarks>
        /// In pull mode, IoT hub sends no desired properties patches while the device is connected, so
        /// <see cref="Twin.TwinClient.DesiredPatchReceived"/> is never raised. Instead, upon each connection the twin client
        /// fetches the desired properties if IoT hub's version is newer than the one the device holds, and raises
        /// <see cref="Twin.TwinClient.TwinPushReceived"/> with them. Later changes are only seen upon reconnecting or by calling
        /// <see cref="Twin.TwinClient.GetTwinAsync"/>.
        /// This value is read once when the connection client is constructed. It does not disable explicit twin requests.
        /// </remarks>
        public bool ReceiveDesiredPropertyUpdates { get; set; }

        /// <summary>
        /// True to have IoT hub push the reported properties upon connecting or reconnecting, when they changed since the version
        /// this device holds. False (the default) otherwise. Maps to the birth message's push_reported flag.
        /// </summary>
        /// <remarks>
        /// This is meant for devices that don't persist their reported properties and use IoT hub to restore them.
        /// This value is read once when the connection client is constructed. It does not disable explicit twin requests.
        /// </remarks>
        public bool ReceiveReportedPropertiesUponConnect { get; set; }

        internal Unified.Models.ConnectionClientOptions ToCommonOptions()
        {
            return new()
            {
                ConnectionRetryPolicy = ConnectionRetryPolicy,
                ConnectionAttemptTimeout = ConnectionAttemptTimeout,
                EnableMqttLogging = EnableMqttLogging,
                MqttClient = MqttClient,
                UseCleanSession = UseCleanSession,
                SessionExpiryInterval = SessionExpiryInterval,
            };
        }
    }
}
