using Microsoft.Azure.Devices.Client.Mqtt;
using System;
using System.Collections.Generic;
using System.Text;

namespace Microsoft.Azure.Devices.Client.UnitTests
{
    public class MqttObjectHelpers
    {
        public static MqttSubscribeAck CreateSuccessfulSuback(MqttSubscribe subscribe)
        {
            List<MqttSubscribeAckItem> subackItems = new();
            foreach (var subscribeItem in subscribe.TopicFilters)
            {
                subackItems.Add(new MqttSubscribeAckItem()
                {
                    ReasonCode = subscribeItem.QualityOfServiceLevel == MqttQualityOfServiceLevel.ExactlyOnce ? MqttClientSubscribeReasonCode.GrantedQoS2 : subscribeItem.QualityOfServiceLevel == MqttQualityOfServiceLevel.AtLeastOnce ? MqttClientSubscribeReasonCode.GrantedQoS1 : MqttClientSubscribeReasonCode.GrantedQoS0,
                    TopicFilter = new(subscribeItem.Topic, subscribeItem.QualityOfServiceLevel)
                });
            }

            return new MqttSubscribeAck()
            {
                Items = subackItems,
            };
        }

        public static MqttUnsubscribeAck CreateSuccessfulUnsuback(MqttUnsubscribe unsubscribe)
        {
            List<MqttUnsubscribeAckItem> unsubackItems = new();
            foreach (var unsubscribeItem in unsubscribe.TopicFilters)
            {
                unsubackItems.Add(new MqttUnsubscribeAckItem()
                {
                    ReasonCode = MqttClientUnsubscribeReasonCode.Success,
                    TopicFilter = unsubscribeItem,
                });
            }

            return new MqttUnsubscribeAck()
            {
                Items = unsubackItems,
            };
        }
    }
}
