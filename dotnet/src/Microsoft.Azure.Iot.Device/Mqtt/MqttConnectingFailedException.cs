// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

namespace Microsoft.Azure.Iot.Device.Mqtt
{
    public sealed class MqttConnectingFailedException : Exception
    {
        public MqttConnectingFailedException(string message, MqttConnectAck connack)
            : base(message)
        {
            ConnectAck = connack;
        }

        public MqttConnectAck ConnectAck { get; }

        public MqttConnectReasonCode ResultCode => ConnectAck?.ResultCode ?? MqttConnectReasonCode.UnspecifiedError;
    }
}
