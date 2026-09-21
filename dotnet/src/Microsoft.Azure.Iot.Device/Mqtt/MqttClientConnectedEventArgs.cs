// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

namespace Microsoft.Azure.Iot.Device.Mqtt
{
    public class MqttClientConnectedEventArgs : EventArgs
    {
        /// <summary>
        ///     Gets the authentication result.
        ///     <remarks>MQTT 5.0.0+ feature.</remarks>
        /// </summary>
        public required MqttConnectAck ConnectAck { get; set; }
    }
}
