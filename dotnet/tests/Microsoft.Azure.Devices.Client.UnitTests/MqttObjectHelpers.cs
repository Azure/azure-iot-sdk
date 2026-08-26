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
                    ResultCode = subscribeItem.QualityOfServiceLevel == MqttQualityOfServiceLevel.ExactlyOnce ? MqttClientSubscribeResultCode.GrantedQoS2 : subscribeItem.QualityOfServiceLevel == MqttQualityOfServiceLevel.AtLeastOnce ? MqttClientSubscribeResultCode.GrantedQoS1 : MqttClientSubscribeResultCode.GrantedQoS0,
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
                    ResultCode = MqttClientUnsubscribeResultCode.Success,
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
