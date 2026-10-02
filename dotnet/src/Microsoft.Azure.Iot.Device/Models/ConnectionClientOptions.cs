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
        /// The number of consecutive failed IoT hub connection attempts, during an automatic reconnection, after which
        /// a device that was provisioned through Device Provisioning Service re-provisions rather than continuing to
        /// retry an unreachable hub forever.
        /// </summary>
        /// <remarks>
        /// A hub that was vacated service-side may stop answering rather than rejecting the device's identity, in which
        /// case nothing else would ever send the device back to DPS. Once this many consecutive reconnect attempts
        /// fail, the client asks DPS for a fresh assignment and connects to whichever hub it is assigned. This only
        /// applies to a device that was provisioned through DPS, since there is otherwise no registration to renew.
        /// Set to 0 to disable this fallback and retry the hub indefinitely. Defaults to 50.
        /// </remarks>
        public uint MaxHubConnectAttemptsBeforeReprovision { get; set; } = 50;
    }
}
