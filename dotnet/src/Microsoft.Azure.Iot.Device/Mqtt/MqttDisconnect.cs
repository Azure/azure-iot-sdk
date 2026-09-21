// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

namespace Microsoft.Azure.Iot.Device.Mqtt
{
    public class MqttDisconnect
    {
        /// <summary>
        ///     Gets or sets the reason code.
        ///     <remarks>MQTT 5.0.0+ feature.</remarks>
        /// </summary>
        public MqttClientDisconnectOptionsReason Reason { get; set; }

        /// <summary>
        ///     Gets or sets the reason string.
        ///     <remarks>MQTT 5.0.0+ feature.</remarks>
        /// </summary>
        public string? ReasonString { get; set; }

        /// <summary>
        ///     Gets or sets the session expiry interval.
        ///     <remarks>MQTT 5.0.0+ feature.</remarks>
        /// </summary>
        public uint SessionExpiryInterval { get; set; }

        /// <summary>
        ///     Gets or sets the user properties.
        ///     <remarks>MQTT 5.0.0+ feature.</remarks>
        /// </summary>
        public List<MqttUserProperty> UserProperties { get; set; } = new();

        public void AddUserProperty(string key, string value)
        {
            UserProperties ??= [];
            UserProperties.Add(new MqttUserProperty(key, value));
        }

        /// <summary>
        /// Adds a user property with a pre-encoded UTF-8 byte value.
        /// This overload is more performant when the value is already available as bytes.
        /// </summary>
        /// <param name="key">The property name.</param>
        /// <param name="value">The property value as ReadOnlyMemory of bytes.</param>
        public void AddUserProperty(string key, ReadOnlyMemory<byte> value)
        {
            UserProperties ??= [];
            UserProperties.Add(new MqttUserProperty(key, value));
        }

        /// <summary>
        /// Adds a user property with a pre-encoded UTF-8 byte value.
        /// This overload is more performant when the value is already available as bytes.
        /// </summary>
        /// <param name="key">The property name.</param>
        /// <param name="value">The property value as an ArraySegment of bytes.</param>
        public void AddUserProperty(string key, ArraySegment<byte> value)
        {
            UserProperties ??= [];
            UserProperties.Add(new MqttUserProperty(key, value));
        }
    }
}
