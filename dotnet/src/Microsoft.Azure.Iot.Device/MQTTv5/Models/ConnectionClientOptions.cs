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

        /// <summary>
        /// The version of the desired properties that this device already holds, for example from a copy of the twin that
        /// the application persisted. Zero (the default) means this device holds no desired properties.
        /// </summary>
        /// <remarks>
        /// This SDK does not persist the twin. Applications that do can restore the version of the desired properties they
        /// persisted here, so that IoT hub only pushes the desired properties upon connecting if they have changed since.
        /// The desired properties version is included in each <see cref="Models.Twin.DesiredPatchReceivedEventArgs"/> and
        /// <see cref="Twin.TwinPushReceivedEventArgs"/>.
        /// This value is read once when the connection client is constructed. Afterwards, the connection client tells
        /// IoT hub the latest desired properties version that the device has received.
        /// </remarks>
        public ulong InitialDesiredPropertiesVersion { get; set; }

        /// <summary>
        /// The version of the reported properties that this device already holds, for example from a copy of the twin that
        /// the application persisted. Zero (the default) means this device holds no reported properties.
        /// </summary>
        /// <remarks>
        /// This SDK does not persist the twin. Applications that do can restore the version of the reported properties they
        /// persisted here, so that IoT hub only pushes the reported properties upon connecting (see
        /// <see cref="ReceiveReportedPropertiesUponConnect"/>) if they have changed since.
        /// This value is read once when the connection client is constructed. Afterwards, the connection client tells
        /// IoT hub the latest reported properties version that the device has received in full.
        /// </remarks>
        public ulong InitialReportedPropertiesVersion { get; set; }

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
