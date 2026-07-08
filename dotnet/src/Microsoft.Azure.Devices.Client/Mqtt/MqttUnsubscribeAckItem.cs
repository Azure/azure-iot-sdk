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
        public MqttUnsubscribeAckItem(string topicFilter, MqttClientUnsubscribeResultCode resultCode)
        {
            TopicFilter = topicFilter ?? throw new ArgumentNullException(nameof(topicFilter));
            ResultCode = resultCode;
        }

        /// <summary>
        ///     Gets or sets the result code.
        ///     <remarks>MQTT 5.0.0+ feature.</remarks>
        /// </summary>
        public MqttClientUnsubscribeResultCode ResultCode { get; }

        /// <summary>
        ///     Gets or sets the topic filter.
        ///     The topic filter can contain topics and wildcards.
        /// </summary>
        public string TopicFilter { get; }
    }
}
