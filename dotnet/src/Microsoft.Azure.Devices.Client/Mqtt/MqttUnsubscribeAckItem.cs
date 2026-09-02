using System;
using System.Collections.Generic;
using System.Text;

namespace Microsoft.Azure.Devices.Client.Mqtt
{
    /// <summary>
    /// A single entry in the list of all unsubscribe ack entries
    /// </summary>
    public class MqttUnsubscribeAckItem
    {
        /// <summary>
        ///     Gets or sets the result code.
        ///     <remarks>MQTT 5.0.0+ feature.</remarks>
        /// </summary>
        public required MqttClientUnsubscribeReasonCode ReasonCode { get; set;  }

        /// <summary>
        ///     Gets or sets the topic filter.
        ///     The topic filter can contain topics and wildcards.
        /// </summary>
        public required string TopicFilter { get; set; }
    }
}
