using System;
using System.Collections.Generic;
using System.Text;

namespace Microsoft.Azure.Devices.Client.Mqtt
{
    public class MqttSubscribeAckItem
    {
        /// <summary>
        ///     Gets or sets the result code.
        ///     <remarks>MQTT 5.0.0+ feature.</remarks>
        /// </summary>
        public required MqttClientSubscribeResultCode ResultCode { get; set; }

        /// <summary>
        ///     Gets or sets the topic filter.
        ///     The topic filter can contain topics and wildcards.
        /// </summary>
        public required MqttTopicFilter TopicFilter { get; set; }
    }
}
