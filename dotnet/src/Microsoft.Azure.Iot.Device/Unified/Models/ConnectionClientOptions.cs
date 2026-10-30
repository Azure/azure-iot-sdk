// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Mqtt;
using Microsoft.Azure.Iot.Device.MQTTnetAdapter;
using Microsoft.Azure.Iot.Device.Retry;
using System;
using System.Collections.Generic;
using System.Text;

namespace Microsoft.Azure.Iot.Device.Unified.Models
{
    /// <summary>
    /// Configures the unified connection client using settings common to both MQTT versions.
    /// </summary>
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
        /// When false, the device presence flow does not subscribe to the twin topics, and constructing a <see cref="Twin.TwinClient"/> with
        /// this connection client throws an <see cref="InvalidOperationException"/>.
        /// This value is read once when the connection client is constructed.
        /// </remarks>
        public bool EnableTwin { get; set; } = true;

        /// <summary>
        /// True (the default) if this device receives direct methods. False to opt out of direct methods.
        /// </summary>
        /// <remarks>
        /// When false, the device presence flow does not subscribe to the direct method request topic, and constructing a
        /// <see cref="DirectMethods.DirectMethodClient"/>
        /// with this connection client throws an <see cref="InvalidOperationException"/>.
        /// This value is read once when the connection client is constructed.
        /// </remarks>
        public bool EnableDirectMethods { get; set; } = true;

        /// <summary>
        /// True (the default) if this device receives cloud-to-device messages. False to opt out of cloud-to-device messages.
        /// </summary>
        /// <remarks>
        /// When false, the device presence flow does not subscribe to the cloud-to-device message topic, and adding a handler
        /// to <see cref="Telemetry.TelemetryClient.CloudToDeviceTelemetryReceivedAsync"/> on a telemetry client that
        /// uses this connection client throws an <see cref="InvalidOperationException"/>. Sending device-to-cloud telemetry
        /// is unaffected.
        /// This value is read once when the connection client is constructed.
        /// </remarks>
        public bool EnableCloudToDeviceMessages { get; set; } = true;

        /// <summary>
        /// False (the default) to ask IoT hub to resume the previous MQTT session when the connection is re-established.
        /// True to start a new session each time this client connects to IoT hub.
        /// </summary>
        /// <remarks>
        /// Resuming a session preserves IoT hub's redelivery of unacknowledged QoS 1 messages across transient disconnections.
        /// The device presence flow skips re-sending its SUBSCRIBE only when IoT hub reports that it resumed the session and
        /// this process already received a SUBACK for those subscriptions on that session, with no failed connection attempt
        /// since. Otherwise it subscribes again, so the first connection made by each process always
        /// subscribes. This only applies to IoT hub connections because Device Provisioning Service does not support
        /// persistent sessions. This value is read once when the connection client is constructed.
        /// </remarks>
        public bool UseCleanSession { get; set; }
    }
}
