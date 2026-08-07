using Microsoft.Azure.Devices.Client.Mqtt;
using System;
using System.Collections.Generic;
using System.Text;

namespace Microsoft.Azure.Devices.Client.Models
{
    /// <summary>
    /// This is thrown by <see cref="Unified.Telemetry.TelemetryClient"/> and <see cref="Unified.Twin.TwinClient"/> whenever it attempts to send an MQTT PUBLISH and it 
    /// fails with an unsuccessful MQTT PUBACK code. Included in this exception are both the PUBACK reason code and the PUBACK reason string (if one was provided by the MQTT broker).
    /// </summary>
    /// <remarks>
    /// This error signals that the message made it to IoT Hub, but there was something wrong with the message or the message could not be accepted at this time.
    /// </remarks>
    public class PublishRejectedException : Exception
    {
        public PublishRejectedException(string message) : base(message)
        { 
        
        }

        public static void ThrowIfUnsuccessfulPuback(MqttPublishAck puback, string errorMessage)
        {
            if (puback.ReasonCode != MqttPublishAckReasonCode.Success)
            {
                throw new PublishRejectedException(errorMessage)
                {
                    ReasonCode = puback.ReasonCode,
                    ReasonString = puback.ReasonString,
                };
            }
        }

        /// <summary>
        /// The MQTT-level reason code for why this publish failed.
        /// </summary>
        public MqttPublishAckReasonCode ReasonCode { get; internal set; }

        /// <summary>
        /// The human-readable reason for why this publish failed.
        /// </summary>
        public string? ReasonString { get; internal set; }
    }
}
