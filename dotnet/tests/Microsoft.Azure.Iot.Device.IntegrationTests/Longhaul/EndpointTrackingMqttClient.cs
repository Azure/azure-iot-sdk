// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Mqtt;
using Microsoft.Azure.Iot.Device.MQTTnetAdapter;

namespace Microsoft.Azure.Iot.Device.IntegrationTests.Longhaul
{
    /// <summary>
    /// An <see cref="MqttNetClient"/> that remembers whether its latest connection targets IoT hub or Device Provisioning
    /// Service. The connection client re-provisions over the same MQTT client, so its connect and disconnect callbacks
    /// alone can't tell a hub outage from a DPS connection.
    /// </summary>
    internal sealed class EndpointTrackingMqttClient : MqttNetClient
    {
        private readonly string _dpsHostName;
        private volatile bool _targetsHub;

        public EndpointTrackingMqttClient(string dpsHostName)
        {
            _dpsHostName = dpsHostName;
        }

        /// <summary>
        /// Whether the latest connection, or connection attempt, targets IoT hub rather than Device Provisioning Service.
        /// </summary>
        public bool TargetsHub => _targetsHub;

        public override Task<MqttConnectAck> ConnectAsync(MqttConnect connect, CancellationToken cancellationToken = default)
        {
            // Set before connecting, so the ConnectedAsync callback raised during the connect already sees it.
            _targetsHub = !string.Equals(connect.HostName, _dpsHostName, StringComparison.OrdinalIgnoreCase);
            return base.ConnectAsync(connect, cancellationToken);
        }
    }
}
