using System;
using System.Collections.Generic;
using System.Text;

namespace Microsoft.Azure.Devices.Client.Mqtt
{
    public class MqttUnsubscribe
    {
        public MqttUnsubscribe()
        { 
        
        }

        public MqttUnsubscribe(string topic)
        {
            TopicFilters.Add(topic);
        }

        public List<string> TopicFilters { get; set; } = new();

        public List<MqttUserProperty> UserProperties { get; set; } = new();
    }
}
