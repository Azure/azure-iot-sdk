// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using System;
using System.Collections.Generic;
using System.Text;

namespace Microsoft.Azure.Iot.Device.Mqtt
{
    public class MqttSubscribeAckItem
    {
        /// <summary>
        ///     Gets or sets the result code.
        ///     <remarks>MQTT 5.0.0+ feature.</remarks>
        /// </summary>
        public required MqttClientSubscribeReasonCode ReasonCode { get; set; }

        /// <summary>
        ///     Gets or sets the topic filter.
        ///     The topic filter can contain topics and wildcards.
        /// </summary>
        public required MqttTopicFilter TopicFilter { get; set; }
    }
}
