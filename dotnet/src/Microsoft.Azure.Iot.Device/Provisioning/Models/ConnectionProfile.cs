// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using System.Runtime.Serialization;
using Microsoft.Azure.Iot.Device.Models;

namespace Microsoft.Azure.Iot.Device.Provisioning.Models
{
    /// <summary>
    /// Internal wire representation of the connection profile capability reported by IoT Hub. This is kept hidden from
    /// the public API surface (which uses <see cref="IotHubConnectionType"/>); the member names are deliberately
    /// preserved because the library's <c>JsonStringEnumConverter</c> derives the wire values ("classic", "mqttV5")
    /// from them via the camel-case naming policy.
    /// </summary>
    internal enum ConnectionProfile
    {
        /// <summary>
        /// Classic MQTT 3.x capable IoT Hub. This is the default when unspecified.
        /// </summary>
        [EnumMember(Value = "classic")]
        Classic,

        /// <summary>
        /// MQTT 5 capable IoT Hub.
        /// </summary>
        [EnumMember(Value = "mqttV5")]
        MqttV5
    }

    internal static class ConnectionProfileExtensions
    {
        /// <summary>Map the internal wire profile to the public connection type.</summary>
        public static IotHubConnectionType ToIotHubConnectionType(this ConnectionProfile connectionProfile) =>
            connectionProfile switch
            {
                ConnectionProfile.MqttV5 => IotHubConnectionType.Mqttv5,
                _ => IotHubConnectionType.Mqttv3,
            };

        /// <summary>Map the public connection type to the internal wire profile.</summary>
        public static ConnectionProfile ToConnectionProfile(this IotHubConnectionType connectionType) =>
            connectionType switch
            {
                IotHubConnectionType.Mqttv5 => ConnectionProfile.MqttV5,
                _ => ConnectionProfile.Classic,
            };
    }
}
