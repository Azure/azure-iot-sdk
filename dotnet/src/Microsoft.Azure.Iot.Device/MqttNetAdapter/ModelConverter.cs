using Microsoft.Azure.Iot.Device.Mqtt;
using MQTTnet;

namespace Microsoft.Azure.Iot.Device.MQTTnetAdapter
{
    internal class ModelConverter
    {
        internal static Mqtt.MqttConnectAck ToGeneric(MQTTnet.MqttClientConnectResult connectResult)
        {
            return new Mqtt.MqttConnectAck()
            {
                ServerKeepAlive = connectResult.ServerKeepAlive,
                IsSessionPresent = connectResult.IsSessionPresent,
                MaximumPacketSize = connectResult.MaximumPacketSize,
                ReasonString = connectResult.ReasonString,
                ReceiveMaximum = connectResult.ReceiveMaximum,
                ResponseInformation = connectResult.ResponseInformation,
                ResultCode = ToGeneric(connectResult.ResultCode),
                SessionExpiryInterval = connectResult.SessionExpiryInterval,
                UserProperties = ToGeneric(connectResult.UserProperties)
            };
        }

        private static List<Mqtt.MqttUserProperty> ToGeneric(List<MQTTnet.Packets.MqttUserProperty> userProperties)
        {
            List<Mqtt.MqttUserProperty> generic = new();

            if (userProperties != null)
            {
                foreach (var mqttNetUserProperty in userProperties)
                {
                    generic.Add(new Mqtt.MqttUserProperty(mqttNetUserProperty.Name, mqttNetUserProperty.ValueBuffer));
                }
            }

            return generic;
        }

        private static List<Mqtt.MqttUserProperty> ToGeneric(IReadOnlyCollection<MQTTnet.Packets.MqttUserProperty> userProperties)
        {
            List<Mqtt.MqttUserProperty> generic = new();

            if (userProperties != null)
            {
                foreach (var mqttNetUserProperty in userProperties)
                {
                    generic.Add(new Mqtt.MqttUserProperty(mqttNetUserProperty.Name, mqttNetUserProperty.ValueBuffer));
                }
            }

            return generic;
        }

        internal static Mqtt.MqttDisconnectReason ToGeneric(MQTTnet.MqttClientDisconnectReason reason)
        {
            switch (reason)
            {
                case MQTTnet.MqttClientDisconnectReason.NormalDisconnection:
                    return Mqtt.MqttDisconnectReason.NormalDisconnection;
                case MQTTnet.MqttClientDisconnectReason.DisconnectWithWillMessage:
                    return Mqtt.MqttDisconnectReason.DisconnectWithWillMessage;
                case MQTTnet.MqttClientDisconnectReason.UnspecifiedError:
                    return Mqtt.MqttDisconnectReason.UnspecifiedError;
                case MQTTnet.MqttClientDisconnectReason.MalformedPacket:
                    return Mqtt.MqttDisconnectReason.MalformedPacket;
                case MQTTnet.MqttClientDisconnectReason.ProtocolError:
                    return Mqtt.MqttDisconnectReason.ProtocolError;
                case MQTTnet.MqttClientDisconnectReason.ImplementationSpecificError:
                    return Mqtt.MqttDisconnectReason.ImplementationSpecificError;
                case MQTTnet.MqttClientDisconnectReason.NotAuthorized:
                    return Mqtt.MqttDisconnectReason.NotAuthorized;
                case MQTTnet.MqttClientDisconnectReason.ServerBusy:
                    return Mqtt.MqttDisconnectReason.ServerBusy;
                case MQTTnet.MqttClientDisconnectReason.ServerShuttingDown:
                    return Mqtt.MqttDisconnectReason.ServerShuttingDown;
                case MQTTnet.MqttClientDisconnectReason.KeepAliveTimeout:
                    return Mqtt.MqttDisconnectReason.KeepAliveTimeout;
                case MQTTnet.MqttClientDisconnectReason.SessionTakenOver:
                    return Mqtt.MqttDisconnectReason.SessionTakenOver;
                case MQTTnet.MqttClientDisconnectReason.TopicFilterInvalid:
                    return Mqtt.MqttDisconnectReason.TopicFilterInvalid;
                case MQTTnet.MqttClientDisconnectReason.TopicNameInvalid:
                    return Mqtt.MqttDisconnectReason.TopicNameInvalid;
                case MQTTnet.MqttClientDisconnectReason.ReceiveMaximumExceeded:
                    return Mqtt.MqttDisconnectReason.ReceiveMaximumExceeded;
                case MQTTnet.MqttClientDisconnectReason.TopicAliasInvalid:
                    return Mqtt.MqttDisconnectReason.TopicAliasInvalid;
                case MQTTnet.MqttClientDisconnectReason.PacketTooLarge:
                    return Mqtt.MqttDisconnectReason.PacketTooLarge;
                case MQTTnet.MqttClientDisconnectReason.MessageRateTooHigh:
                    return Mqtt.MqttDisconnectReason.MessageRateTooHigh;
                case MQTTnet.MqttClientDisconnectReason.QuotaExceeded:
                    return Mqtt.MqttDisconnectReason.QuotaExceeded;
                case MQTTnet.MqttClientDisconnectReason.AdministrativeAction:
                    return Mqtt.MqttDisconnectReason.AdministrativeAction;
                case MQTTnet.MqttClientDisconnectReason.PayloadFormatInvalid:
                    return Mqtt.MqttDisconnectReason.PayloadFormatInvalid;
                case MQTTnet.MqttClientDisconnectReason.RetainNotSupported:
                    return Mqtt.MqttDisconnectReason.RetainNotSupported;
                case MQTTnet.MqttClientDisconnectReason.QosNotSupported:
                    return Mqtt.MqttDisconnectReason.QosNotSupported;
                case MQTTnet.MqttClientDisconnectReason.UseAnotherServer:
                    return Mqtt.MqttDisconnectReason.UseAnotherServer;
                case MQTTnet.MqttClientDisconnectReason.ServerMoved:
                    return Mqtt.MqttDisconnectReason.ServerMoved;
                case MQTTnet.MqttClientDisconnectReason.SharedSubscriptionsNotSupported:
                    return Mqtt.MqttDisconnectReason.SharedSubscriptionsNotSupported;
                case MQTTnet.MqttClientDisconnectReason.ConnectionRateExceeded:
                    return Mqtt.MqttDisconnectReason.ConnectionRateExceeded;
                case MQTTnet.MqttClientDisconnectReason.MaximumConnectTime:
                    return Mqtt.MqttDisconnectReason.MaximumConnectTime;
                case MQTTnet.MqttClientDisconnectReason.SubscriptionIdentifiersNotSupported:
                    return Mqtt.MqttDisconnectReason.SubscriptionIdentifiersNotSupported;
                case MQTTnet.MqttClientDisconnectReason.WildcardSubscriptionsNotSupported:
                    return Mqtt.MqttDisconnectReason.WildcardSubscriptionsNotSupported;
                default:
                    return Mqtt.MqttDisconnectReason.UnspecifiedError;
            }
        }

        internal static Mqtt.MqttPayloadFormatIndicator ToGeneric(MQTTnet.Protocol.MqttPayloadFormatIndicator payloadFormatIndicator)
        {
            switch (payloadFormatIndicator)
            {
                case MQTTnet.Protocol.MqttPayloadFormatIndicator.CharacterData:
                    return Mqtt.MqttPayloadFormatIndicator.CharacterData;
                case MQTTnet.Protocol.MqttPayloadFormatIndicator.Unspecified:
                default:
                    return Mqtt.MqttPayloadFormatIndicator.Unspecified;
            }
        }

        internal static Mqtt.MqttQualityOfServiceLevel ToGeneric(MQTTnet.Protocol.MqttQualityOfServiceLevel qualityOfServiceLevel)
        {
            switch (qualityOfServiceLevel)
            {
                case MQTTnet.Protocol.MqttQualityOfServiceLevel.AtMostOnce:
                    return Mqtt.MqttQualityOfServiceLevel.AtMostOnce;
                case MQTTnet.Protocol.MqttQualityOfServiceLevel.AtLeastOnce:
                    return Mqtt.MqttQualityOfServiceLevel.AtLeastOnce;
                case MQTTnet.Protocol.MqttQualityOfServiceLevel.ExactlyOnce:
                default:
                    return Mqtt.MqttQualityOfServiceLevel.ExactlyOnce;
            }
        }

        internal static Mqtt.MqttConnectReasonCode ToGeneric(MQTTnet.MqttClientConnectResultCode resultCode)
        {
            switch (resultCode)
            {
                case MQTTnet.MqttClientConnectResultCode.Success:
                    return Mqtt.MqttConnectReasonCode.Success;
                case MQTTnet.MqttClientConnectResultCode.UnspecifiedError:
                    return Mqtt.MqttConnectReasonCode.UnspecifiedError;
                case MQTTnet.MqttClientConnectResultCode.MalformedPacket:
                    return Mqtt.MqttConnectReasonCode.MalformedPacket;
                case MQTTnet.MqttClientConnectResultCode.ProtocolError:
                    return Mqtt.MqttConnectReasonCode.ProtocolError;
                case MQTTnet.MqttClientConnectResultCode.ImplementationSpecificError:
                    return Mqtt.MqttConnectReasonCode.ImplementationSpecificError;
                case MQTTnet.MqttClientConnectResultCode.UnsupportedProtocolVersion:
                    return Mqtt.MqttConnectReasonCode.UnsupportedProtocolVersion;
                case MQTTnet.MqttClientConnectResultCode.ClientIdentifierNotValid:
                    return Mqtt.MqttConnectReasonCode.ClientIdentifierNotValid;
                case MQTTnet.MqttClientConnectResultCode.BadUserNameOrPassword:
                    return Mqtt.MqttConnectReasonCode.BadUserNameOrPassword;
                case MQTTnet.MqttClientConnectResultCode.NotAuthorized:
                    return Mqtt.MqttConnectReasonCode.NotAuthorized;
                case MQTTnet.MqttClientConnectResultCode.ServerUnavailable:
                    return Mqtt.MqttConnectReasonCode.ServerUnavailable;
                case MQTTnet.MqttClientConnectResultCode.ServerBusy:
                    return Mqtt.MqttConnectReasonCode.ServerBusy;
                case MQTTnet.MqttClientConnectResultCode.Banned:
                    return Mqtt.MqttConnectReasonCode.Banned;
                case MQTTnet.MqttClientConnectResultCode.BadAuthenticationMethod:
                    return Mqtt.MqttConnectReasonCode.BadAuthenticationMethod;
                case MQTTnet.MqttClientConnectResultCode.TopicNameInvalid:
                    return Mqtt.MqttConnectReasonCode.TopicNameInvalid;
                case MQTTnet.MqttClientConnectResultCode.PacketTooLarge:
                    return Mqtt.MqttConnectReasonCode.PacketTooLarge;
                case MQTTnet.MqttClientConnectResultCode.QuotaExceeded:
                    return Mqtt.MqttConnectReasonCode.QuotaExceeded;
                case MQTTnet.MqttClientConnectResultCode.PayloadFormatInvalid:
                    return Mqtt.MqttConnectReasonCode.PayloadFormatInvalid;
                case MQTTnet.MqttClientConnectResultCode.RetainNotSupported:
                    return Mqtt.MqttConnectReasonCode.RetainNotSupported;
                case MQTTnet.MqttClientConnectResultCode.QoSNotSupported:
                    return Mqtt.MqttConnectReasonCode.QoSNotSupported;
                case MQTTnet.MqttClientConnectResultCode.UseAnotherServer:
                    return Mqtt.MqttConnectReasonCode.UseAnotherServer;
                case MQTTnet.MqttClientConnectResultCode.ServerMoved:
                    return Mqtt.MqttConnectReasonCode.ServerMoved;
                case MQTTnet.MqttClientConnectResultCode.ConnectionRateExceeded:
                default:
                    return Mqtt.MqttConnectReasonCode.ConnectionRateExceeded;

            }
        }

        internal static MqttPublishAck ToGeneric(MqttClientPublishResult puback)
        {
            return new MqttPublishAck()
            {
                ReasonCode = ToGeneric(puback.ReasonCode),
                UserProperties = ToGeneric(puback.UserProperties),
                ReasonString = puback.ReasonString,
            };
        }

        private static Mqtt.MqttPublishAckReasonCode ToGeneric(MQTTnet.MqttClientPublishReasonCode reasonCode)
        {
            switch (reasonCode)
            {
                case MQTTnet.MqttClientPublishReasonCode.Success:
                    return Mqtt.MqttPublishAckReasonCode.Success;
                case MQTTnet.MqttClientPublishReasonCode.NoMatchingSubscribers:
                    return Mqtt.MqttPublishAckReasonCode.NoMatchingSubscribers;
                case MQTTnet.MqttClientPublishReasonCode.UnspecifiedError:
                    return Mqtt.MqttPublishAckReasonCode.UnspecifiedError;
                case MQTTnet.MqttClientPublishReasonCode.ImplementationSpecificError:
                    return Mqtt.MqttPublishAckReasonCode.ImplementationSpecificError;
                case MQTTnet.MqttClientPublishReasonCode.NotAuthorized:
                    return Mqtt.MqttPublishAckReasonCode.NotAuthorized;
                case MQTTnet.MqttClientPublishReasonCode.TopicNameInvalid:
                    return Mqtt.MqttPublishAckReasonCode.TopicNameInvalid;
                case MQTTnet.MqttClientPublishReasonCode.PacketIdentifierInUse:
                    return Mqtt.MqttPublishAckReasonCode.PacketIdentifierInUse;
                case MQTTnet.MqttClientPublishReasonCode.QuotaExceeded:
                    return Mqtt.MqttPublishAckReasonCode.QuotaExceeded;
                case MQTTnet.MqttClientPublishReasonCode.PayloadFormatInvalid:
                default:
                    return Mqtt.MqttPublishAckReasonCode.PayloadFormatInvalid;

            }
        }

        internal static MqttSubscribeAck ToGeneric(MqttClientSubscribeResult suback)
        {
            List<MqttSubscribeAckItem> genericItems = new();
            if (suback.Items != null)
            {
                foreach (var mqttnetSubscribeResultItem in suback.Items)
                {
                    genericItems.Add(new()
                    {
                        ReasonCode = ToGeneric(mqttnetSubscribeResultItem.ResultCode),
                        TopicFilter = new(mqttnetSubscribeResultItem.TopicFilter.Topic, ToGeneric(mqttnetSubscribeResultItem.TopicFilter.QualityOfServiceLevel))
                    });
                }
            }

            MqttSubscribeAck generic = new()
            {
                UserProperties = ToGeneric(suback.UserProperties),
                ReasonString = suback.ReasonString,
                Items = genericItems,
            };

            generic.Items = genericItems;

            return generic;
        }

        private static Mqtt.MqttClientSubscribeReasonCode ToGeneric(MQTTnet.MqttClientSubscribeResultCode resultCode)
        {
            switch (resultCode)
            {
                case MQTTnet.MqttClientSubscribeResultCode.GrantedQoS0:
                    return Mqtt.MqttClientSubscribeReasonCode.GrantedQoS0;
                case MQTTnet.MqttClientSubscribeResultCode.GrantedQoS1:
                    return Mqtt.MqttClientSubscribeReasonCode.GrantedQoS1;
                case MQTTnet.MqttClientSubscribeResultCode.GrantedQoS2:
                    return Mqtt.MqttClientSubscribeReasonCode.GrantedQoS2;
                case MQTTnet.MqttClientSubscribeResultCode.UnspecifiedError:
                    return Mqtt.MqttClientSubscribeReasonCode.UnspecifiedError;
                case MQTTnet.MqttClientSubscribeResultCode.ImplementationSpecificError:
                    return Mqtt.MqttClientSubscribeReasonCode.ImplementationSpecificError;
                case MQTTnet.MqttClientSubscribeResultCode.NotAuthorized:
                    return Mqtt.MqttClientSubscribeReasonCode.NotAuthorized;
                case MQTTnet.MqttClientSubscribeResultCode.TopicFilterInvalid:
                    return Mqtt.MqttClientSubscribeReasonCode.TopicFilterInvalid;
                case MQTTnet.MqttClientSubscribeResultCode.PacketIdentifierInUse:
                    return Mqtt.MqttClientSubscribeReasonCode.PacketIdentifierInUse;
                case MQTTnet.MqttClientSubscribeResultCode.QuotaExceeded:
                    return Mqtt.MqttClientSubscribeReasonCode.QuotaExceeded;
                case MQTTnet.MqttClientSubscribeResultCode.SharedSubscriptionsNotSupported:
                    return Mqtt.MqttClientSubscribeReasonCode.SharedSubscriptionsNotSupported;
                case MQTTnet.MqttClientSubscribeResultCode.SubscriptionIdentifiersNotSupported:
                    return Mqtt.MqttClientSubscribeReasonCode.SubscriptionIdentifiersNotSupported;
                case MQTTnet.MqttClientSubscribeResultCode.WildcardSubscriptionsNotSupported:
                default:
                    return Mqtt.MqttClientSubscribeReasonCode.WildcardSubscriptionsNotSupported;
            }
        }

        internal static MqttUnsubscribeAck ToGeneric(MqttClientUnsubscribeResult unsuback)
        {
            return new()
            {
                ReasonString = unsuback.ReasonString,
                Items = toGeneric(unsuback.Items),
                UserProperties = ToGeneric(unsuback.UserProperties)
            };
        }

        private static IReadOnlyCollection<MqttUnsubscribeAckItem> toGeneric(IReadOnlyCollection<MqttClientUnsubscribeResultItem> unsubackItem)
        {
            List<MqttUnsubscribeAckItem> generic = new();
            foreach (MqttClientUnsubscribeResultItem item in unsubackItem)
            {
                generic.Add(new()
                { 
                    ReasonCode = toGeneric(item.ResultCode),
                    TopicFilter = item.TopicFilter,
                });
            }

            return generic;
        }

        private static Mqtt.MqttClientUnsubscribeReasonCode toGeneric(MQTTnet.MqttClientUnsubscribeResultCode resultCode)
        {
            switch (resultCode)
            {
                case MQTTnet.MqttClientUnsubscribeResultCode.Success:
                    return Mqtt.MqttClientUnsubscribeReasonCode.Success;
                case MQTTnet.MqttClientUnsubscribeResultCode.NoSubscriptionExisted:
                    return Mqtt.MqttClientUnsubscribeReasonCode.NoSubscriptionExisted;
                case MQTTnet.MqttClientUnsubscribeResultCode.UnspecifiedError:
                    return Mqtt.MqttClientUnsubscribeReasonCode.UnspecifiedError;
                case MQTTnet.MqttClientUnsubscribeResultCode.ImplementationSpecificError:
                    return Mqtt.MqttClientUnsubscribeReasonCode.ImplementationSpecificError;
                case MQTTnet.MqttClientUnsubscribeResultCode.NotAuthorized:
                    return Mqtt.MqttClientUnsubscribeReasonCode.NotAuthorized;
                case MQTTnet.MqttClientUnsubscribeResultCode.TopicFilterInvalid:
                    return Mqtt.MqttClientUnsubscribeReasonCode.TopicFilterInvalid;
                case MQTTnet.MqttClientUnsubscribeResultCode.PacketIdentifierInUse:
                default:
                    return Mqtt.MqttClientUnsubscribeReasonCode.PacketIdentifierInUse;
            }
        }

        internal static MQTTnet.MqttClientDisconnectOptionsReason ToMqttNet(Mqtt.MqttClientDisconnectOptionsReason reason)
        {
            switch (reason)
            {
                case Mqtt.MqttClientDisconnectOptionsReason.NormalDisconnection:
                    return MQTTnet.MqttClientDisconnectOptionsReason.NormalDisconnection;
                case Mqtt.MqttClientDisconnectOptionsReason.DisconnectWithWillMessage:
                    return MQTTnet.MqttClientDisconnectOptionsReason.DisconnectWithWillMessage;
                case Mqtt.MqttClientDisconnectOptionsReason.UnspecifiedError:
                    return MQTTnet.MqttClientDisconnectOptionsReason.UnspecifiedError;
                case Mqtt.MqttClientDisconnectOptionsReason.MalformedPacket:
                    return MQTTnet.MqttClientDisconnectOptionsReason.MalformedPacket;
                case Mqtt.MqttClientDisconnectOptionsReason.ProtocolError:
                    return MQTTnet.MqttClientDisconnectOptionsReason.ProtocolError;
                case Mqtt.MqttClientDisconnectOptionsReason.ImplementationSpecificError:
                    return MQTTnet.MqttClientDisconnectOptionsReason.ImplementationSpecificError;
                case Mqtt.MqttClientDisconnectOptionsReason.TopicNameInvalid:
                    return MQTTnet.MqttClientDisconnectOptionsReason.TopicNameInvalid;
                case Mqtt.MqttClientDisconnectOptionsReason.ReceiveMaximumExceeded:
                    return MQTTnet.MqttClientDisconnectOptionsReason.ReceiveMaximumExceeded;
                case Mqtt.MqttClientDisconnectOptionsReason.TopicAliasInvalid:
                    return MQTTnet.MqttClientDisconnectOptionsReason.TopicAliasInvalid;
                case Mqtt.MqttClientDisconnectOptionsReason.PacketTooLarge:
                    return MQTTnet.MqttClientDisconnectOptionsReason.PacketTooLarge;
                case Mqtt.MqttClientDisconnectOptionsReason.MessageRateTooHigh:
                    return MQTTnet.MqttClientDisconnectOptionsReason.MessageRateTooHigh;
                case Mqtt.MqttClientDisconnectOptionsReason.QuotaExceeded:
                    return MQTTnet.MqttClientDisconnectOptionsReason.QuotaExceeded;
                case Mqtt.MqttClientDisconnectOptionsReason.AdministrativeAction:
                    return MQTTnet.MqttClientDisconnectOptionsReason.AdministrativeAction;
                case Mqtt.MqttClientDisconnectOptionsReason.PayloadFormatInvalid:
                default:
                    return MQTTnet.MqttClientDisconnectOptionsReason.PayloadFormatInvalid;
            }
        }

        internal static MQTTnet.Protocol.MqttPayloadFormatIndicator ToMqttNet(Mqtt.MqttPayloadFormatIndicator payloadFormatIndicator)
        {
            switch (payloadFormatIndicator)
            {
                case MqttPayloadFormatIndicator.CharacterData:
                    return MQTTnet.Protocol.MqttPayloadFormatIndicator.CharacterData;
                case MqttPayloadFormatIndicator.Unspecified:
                default:
                    return MQTTnet.Protocol.MqttPayloadFormatIndicator.Unspecified;
            }
        }

        internal static MQTTnet.Protocol.MqttQualityOfServiceLevel ToMqttNet(Mqtt.MqttQualityOfServiceLevel qualityOfServiceLevel)
        {
            switch (qualityOfServiceLevel)
            {
                case MqttQualityOfServiceLevel.AtMostOnce:
                    return MQTTnet.Protocol.MqttQualityOfServiceLevel.AtMostOnce;
                case MqttQualityOfServiceLevel.AtLeastOnce:
                    return MQTTnet.Protocol.MqttQualityOfServiceLevel.AtLeastOnce;
                case MqttQualityOfServiceLevel.ExactlyOnce:
                default:
                    return MQTTnet.Protocol.MqttQualityOfServiceLevel.ExactlyOnce;
            }
        }
    }
}
