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
        /// <remarks>
        /// Websockets are required for this client to connect through <see cref="Proxy"/>.
        /// </remarks>
        public bool UseWebsocket { get; set; }

        /// <summary>
        /// Whether to enable logs from the underlying MQTT client
        /// </summary>
        public bool EnableMqttLogs { get; set; }

        /// <summary>
        /// The HTTP proxy to connect through. Only used if <see cref="UseWebsocket"/> is set to true.
        /// </summary>
        /// <remarks>
        /// This only applies to this client's MQTT connections. When this client is supplied to a connection client through
        /// <see cref="Unified.Models.ConnectionClientOptions.MqttClient"/>, this proxy and <see cref="UseWebsocket"/> take precedence over
        /// <see cref="Unified.Models.ConnectionClientOptions.Proxy"/> and <see cref="Unified.Models.ConnectionClientOptions.UseWebsocket"/>
        /// for MQTT connections, while the connection client's HTTPS requests still use <see cref="Unified.Models.ConnectionClientOptions.Proxy"/>.
        /// </remarks>
        public IWebProxy? Proxy { get; set; }

        /// <summary>
        /// The period at which to send keep alive pings on the MQTT connection.
        /// </summary>
        public TimeSpan KeepAlivePeriod { get; set; } = TimeSpan.FromSeconds(60);
    }
}
