using Microsoft.Azure.Devices.Client.Mqtt;
using Microsoft.Azure.Devices.Client.MqttNetAdapter.Session.Retry;
using System;
using System.Collections.Generic;
using System.Text;

namespace Microsoft.Azure.Devices.Client.Models
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
        /// True if you want to enable MQTT-level logs. False if you do not want these logs.
        /// </summary>
        public bool EnableMqttLogging { get; set; }

        /// <summary>
        /// The MQTT client to use. If null, a default MQTT client will be created for you.
        /// </summary>
        public IMqttClient? MqttClient { get; set; }
    }
}
