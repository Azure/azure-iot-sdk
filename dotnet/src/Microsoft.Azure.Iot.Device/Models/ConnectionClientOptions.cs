// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Mqtt;
using Microsoft.Azure.Iot.Device.MQTTnetAdapter;
using Microsoft.Azure.Iot.Device.Retry;
using System;
using System.Collections.Generic;
using System.Text;

namespace Microsoft.Azure.Iot.Device.Models
{
    public class ConnectionClientOptions
    {
        /// <summary>
        /// The retry policy that the connection client will consult each time it attempts to reconnect and/or each time it attempts the initial connect.
        /// </summary>
        /// By default, this is an <see cref="ExponentialBackoffRetryPolicy"/> with jitter. To prevent any reconnection, you may set this to an instance of <see cref="NoRetry"/>.
        /// </remarks>
        public IRetryPolicy ConnectionRetryPolicy { get; set; } = new ExponentialBackoffRetryPolicy(uint.MaxValue, TimeSpan.FromSeconds(60));

        /// <summary>
        /// How long to wait for a single connection attempt to finish before abandoning it.
        /// </summary>
        /// <remarks>
        /// This value allows for you to configure the connection attempt timeout for both initial
        /// connection and reconnection scenarios. Note that this value is ignored for the initial 
        /// connect attempt if <see cref="RetryOnFirstConnect"/> is false.
        /// </remarks>
        public TimeSpan ConnectionAttemptTimeout { get; set; } = TimeSpan.FromSeconds(10);

        /// <summary>
        /// True if you want to enable MQTT-level logs. False if you do not want these logs.
        /// </summary>
        public bool EnableMqttLogging { get; set; }

        /// <summary>
        /// The MQTT client to use. If null, a default MQTT client will be created for you.
        /// </summary>
        /// <remarks>
        /// This MQTT client will be managed for you such that this package will re-connect as necessary. If no MQTT client is provided, an instance of
        /// <see cref="MqttNetClient"/> will be created for you.
        /// </remarks>
        public IMqttClient? MqttClient { get; set; }

        /// <summary>
        /// True (the default) if this device uses twins. False to opt out of twins.
        /// </summary>
        /// <remarks>
        /// When false, the device presence flow does not subscribe to the twin response or desired property update
        /// topics, and constructing a <see cref="Unified.Twin.TwinClient"/> with this connection client throws an
        /// <see cref="InvalidOperationException"/>. This value is read once when the connection client is constructed.
        /// </remarks>
        public bool EnableTwin { get; set; } = true;

        /// <summary>
        /// True (the default) if this device receives direct methods. False to opt out of direct methods.
        /// </summary>
        /// <remarks>
        /// When false, the device presence flow does not subscribe to the direct method request topic, and constructing a
        /// <see cref="Unified.DirectMethods.DirectMethodClient"/> with this connection client throws an
        /// <see cref="InvalidOperationException"/>. This value is read once when the connection client is constructed.
        /// </remarks>
        public bool EnableDirectMethods { get; set; } = true;

        /// <summary>
        /// True (the default) if this device receives cloud-to-device messages. False to opt out of cloud-to-device messages.
        /// </summary>
        /// <remarks>
        /// When false, the device presence flow does not subscribe to the cloud-to-device message topic, and adding a handler
        /// to <see cref="Unified.Telemetry.TelemetryClient.CloudToDeviceTelemetryReceivedAsync"/> on a telemetry client that
        /// uses this connection client throws an <see cref="InvalidOperationException"/>. Sending device-to-cloud telemetry
        /// is unaffected. This value is read once when the connection client is constructed.
        /// </remarks>
        public bool EnableCloudToDeviceMessages { get; set; } = true;

        /// <summary>
        /// False (the default) to ask IoT hub to resume the previous MQTT session when the connection is re-established.
        /// True to start a new session each time this client connects to IoT hub.
        /// </summary>
        /// <remarks>
        /// When IoT hub reports that it resumed the session, the subscriptions made by the device presence flow are still
        /// active, so they are not sent again. To guarantee that a resumed session holds those subscriptions, resumption
        /// is only requested on the first reconnect attempt after a presence flow completed on the same IoT hub; the first
        /// connection to an IoT hub and any further reconnect attempts start a new session. This only applies to IoT hub
        /// connections because Device Provisioning Service does not support persistent sessions. This value is read once
        /// when the connection client is constructed.
        /// </remarks>
        public bool UseCleanSession { get; set; }

        /// <summary>
        /// How long IoT hub keeps this device's MQTT 5 session after the connection closes. Defaults to one hour.
        /// </summary>
        /// <remarks>
        /// A session can only be resumed on reconnect if it has not expired, so <see cref="TimeSpan.Zero"/> effectively
        /// disables session resumption. The value is sent in whole seconds (any fraction is dropped) and must be between
        /// zero and <see cref="uint.MaxValue"/> seconds; the maximum means the session never expires. Deliberate
        /// disconnects still end the session immediately. This only applies to MQTT 5 IoT hub connections because MQTT
        /// 3.1.1 has no session expiry. This value is read once when the connection client is constructed.
        /// </remarks>
        /// <exception cref="ArgumentOutOfRangeException">The value is negative or longer than <see cref="uint.MaxValue"/> seconds.</exception>
        public TimeSpan MqttV5SessionExpiryInterval
        {
            get => _mqttV5SessionExpiryInterval;
            set
            {
                if (value < TimeSpan.Zero || value.TotalSeconds >= (double)uint.MaxValue + 1)
                {
                    throw new ArgumentOutOfRangeException(nameof(value), value, $"The MQTT 5 session expiry interval must be between zero and {uint.MaxValue} seconds.");
                }

                _mqttV5SessionExpiryInterval = value;
            }
        }

        private TimeSpan _mqttV5SessionExpiryInterval = TimeSpan.FromHours(1);
    }
}
