// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.MQTTnetAdapter;
using System.Net;

namespace Microsoft.Azure.Iot.Device.MqttNetAdapter
{
    public class MqttNetClientOptions
    {
        /// <summary>
        /// Whether to connect this mqtt client using websockets or not.
        /// </summary>
        public bool UseWebsocket { get; set; }

        /// <summary>
        /// Whether to enable logs from the underlying MQTT client
        /// </summary>
        public bool EnableMqttLogs { get; set; }

        /// <summary>
        /// The HTTP proxy to connect through. Only used if <see cref="UseWebsocket"/> is set to true.
        /// </summary>
        public IWebProxy? Proxy { get; set; }

        /// <summary>
        /// The period at which to send keep alive pings on the MQTT connection.
        /// </summary>
        public TimeSpan KeepAlivePeriod { get; set; } = TimeSpan.FromSeconds(60);
    }
}
