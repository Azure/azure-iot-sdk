using Microsoft.Azure.Devices.Client.Mqtt;
using System;
using System.Collections.Generic;
using System.Text;

namespace Microsoft.Azure.Devices.Client
{
    /// <summary>
    /// This is thrown by <see cref="ConnectionClient"/> whenever it attempts to send an MQTT CONNECT and it 
    /// fails with a fatal unsuccessful MQTT CONNACK code. Included in this exception are both the CONNACK reason code and the CONNACK reason string (if one was provided by the MQTT broker).
    /// </summary>
    /// <remarks>
    /// This error signals that the connect request made it to IoT Hub, but there was something wrong with the request or the request could not be accepted at this time.
    /// </remarks>
    public class ConnectRejectedException : Exception
    {
        public ConnectRejectedException(string message) : base(message)
        {

        }

        public static void ThrowIfUnsuccessfulConnack(MqttConnectAck connack, string errorMessage)
        {
            if (connack.ResultCode != MqttConnectResultCode.Success)
            {
                throw new ConnectRejectedException(errorMessage + $" Result code: {connack.ResultCode}")
                {
                    ReasonCode = connack.ResultCode,
                    ReasonString = connack.ReasonString,
                };
            }
        }

        /// <summary>
        /// The MQTT-level reason code for why this connect failed.
        /// </summary>
        public MqttConnectResultCode ReasonCode { get; internal set; }

        /// <summary>
        /// The human-readable reason for why this connect failed if one was provided.
        /// </summary>
        public string? ReasonString { get; internal set; }
    }
}
