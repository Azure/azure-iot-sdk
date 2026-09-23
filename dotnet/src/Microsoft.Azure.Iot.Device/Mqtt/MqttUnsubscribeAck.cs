// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

namespace Microsoft.Azure.Iot.Device.Mqtt
{
    public class MqttUnsubscribeAck
    {
        /// <summary>
        ///     Gets the result for every topic filter item.
        /// </summary>
        public required IReadOnlyCollection<MqttUnsubscribeAckItem> Items { get; set; }

        /// <summary>
        ///     Gets the reason string.
        ///     <remarks>MQTT 5.0.0+ feature.</remarks>
        /// </summary>
        public string? ReasonString { get; set; }

        /// <summary>
        ///     Gets the user properties which were part of the UNSUBACK packet.
        ///     <remarks>MQTT 5.0.0+ feature.</remarks>
        /// </summary>
        public IReadOnlyCollection<MqttUserProperty> UserProperties { get; set; } = new List<MqttUserProperty>();
    }
}
