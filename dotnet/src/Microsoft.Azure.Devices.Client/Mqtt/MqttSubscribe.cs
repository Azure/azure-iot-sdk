using System;
using System.Collections.Generic;
using System.Text;

namespace Microsoft.Azure.Devices.Client.Mqtt
{
    public class MqttSubscribe
    {
        public MqttSubscribe()
        { 
        
        }

        public MqttSubscribe(string topic, MqttQualityOfServiceLevel qos)
        {
            TopicFilters.Add(new(topic, qos));
        }

        public List<MqttTopicFilter> TopicFilters { get; set; } = new();

        public List<MqttUserProperty> UserProperties { get; set; } = new();
    }
}
