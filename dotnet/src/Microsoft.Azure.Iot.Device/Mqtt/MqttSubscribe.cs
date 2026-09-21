using System;
using System.Collections.Generic;
using System.Text;

namespace Microsoft.Azure.Iot.Device.Mqtt
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

        public void AddUserProperty(string key, string value)
        {
            UserProperties ??= [];
            UserProperties.Add(new MqttUserProperty(key, value));
        }

        /// <summary>
        /// Adds a user property with a pre-encoded UTF-8 byte value.
        /// This overload is more performant when the value is already available as bytes.
        /// </summary>
        /// <param name="key">The property name.</param>
        /// <param name="value">The property value as ReadOnlyMemory of bytes.</param>
        public void AddUserProperty(string key, ReadOnlyMemory<byte> value)
        {
            UserProperties ??= [];
            UserProperties.Add(new MqttUserProperty(key, value));
        }

        /// <summary>
        /// Adds a user property with a pre-encoded UTF-8 byte value.
        /// This overload is more performant when the value is already available as bytes.
        /// </summary>
        /// <param name="key">The property name.</param>
        /// <param name="value">The property value as an ArraySegment of bytes.</param>
        public void AddUserProperty(string key, ArraySegment<byte> value)
        {
            UserProperties ??= [];
            UserProperties.Add(new MqttUserProperty(key, value));
        }
    }
}
