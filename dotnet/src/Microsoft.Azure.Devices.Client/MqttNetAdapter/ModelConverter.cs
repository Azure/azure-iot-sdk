using Microsoft.Azure.Devices.Client.Mqtt;
using MQTTnet;

namespace Microsoft.Azure.Devices.Client.MQTTnetAdapter
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
                    generic.Add(new Mqtt.MqttUserProperty() { Name = mqttNetUserProperty.Name, Value = mqttNetUserProperty.ValueBuffer });
                }
            }

            return generic;
        }

        private static IReadOnlyCollection<Mqtt.MqttUserProperty> ToGeneric(IReadOnlyCollection<MQTTnet.Packets.MqttUserProperty> userProperties)
        {
            List<Mqtt.MqttUserProperty> generic = new();

            if (userProperties != null)
            {
                foreach (var mqttNetUserProperty in userProperties)
                {
                    generic.Add(new Mqtt.MqttUserProperty() { Name = mqttNetUserProperty.Name, Value = mqttNetUserProperty.ValueBuffer });
                }
            }

            return generic;
        }

        internal static Mqtt.MqttClientDisconnectReason ToGeneric(MQTTnet.MqttClientDisconnectReason reason)
        {
            switch (reason)
            {
                case MQTTnet.MqttClientDisconnectReason.NormalDisconnection:
                    return Mqtt.MqttClientDisconnectReason.NormalDisconnection;
                case MQTTnet.MqttClientDisconnectReason.DisconnectWithWillMessage:
                    return Mqtt.MqttClientDisconnectReason.DisconnectWithWillMessage;
                case MQTTnet.MqttClientDisconnectReason.UnspecifiedError:
                    return Mqtt.MqttClientDisconnectReason.UnspecifiedError;
                case MQTTnet.MqttClientDisconnectReason.MalformedPacket:
                    return Mqtt.MqttClientDisconnectReason.MalformedPacket;
                case MQTTnet.MqttClientDisconnectReason.ProtocolError:
                    return Mqtt.MqttClientDisconnectReason.ProtocolError;
                case MQTTnet.MqttClientDisconnectReason.ImplementationSpecificError:
                    return Mqtt.MqttClientDisconnectReason.ImplementationSpecificError;
                case MQTTnet.MqttClientDisconnectReason.NotAuthorized:
                    return Mqtt.MqttClientDisconnectReason.NotAuthorized;
                case MQTTnet.MqttClientDisconnectReason.ServerBusy:
                    return Mqtt.MqttClientDisconnectReason.ServerBusy;
                case MQTTnet.MqttClientDisconnectReason.ServerShuttingDown:
                    return Mqtt.MqttClientDisconnectReason.ServerShuttingDown;
                case MQTTnet.MqttClientDisconnectReason.KeepAliveTimeout:
                    return Mqtt.MqttClientDisconnectReason.KeepAliveTimeout;
                case MQTTnet.MqttClientDisconnectReason.SessionTakenOver:
                    return Mqtt.MqttClientDisconnectReason.SessionTakenOver;
                case MQTTnet.MqttClientDisconnectReason.TopicFilterInvalid:
                    return Mqtt.MqttClientDisconnectReason.TopicFilterInvalid;
                case MQTTnet.MqttClientDisconnectReason.TopicNameInvalid:
                    return Mqtt.MqttClientDisconnectReason.TopicNameInvalid;
                case MQTTnet.MqttClientDisconnectReason.ReceiveMaximumExceeded:
                    return Mqtt.MqttClientDisconnectReason.ReceiveMaximumExceeded;
                case MQTTnet.MqttClientDisconnectReason.TopicAliasInvalid:
                    return Mqtt.MqttClientDisconnectReason.TopicAliasInvalid;
                case MQTTnet.MqttClientDisconnectReason.PacketTooLarge:
                    return Mqtt.MqttClientDisconnectReason.PacketTooLarge;
                case MQTTnet.MqttClientDisconnectReason.MessageRateTooHigh:
                    return Mqtt.MqttClientDisconnectReason.MessageRateTooHigh;
                case MQTTnet.MqttClientDisconnectReason.QuotaExceeded:
                    return Mqtt.MqttClientDisconnectReason.QuotaExceeded;
                case MQTTnet.MqttClientDisconnectReason.AdministrativeAction:
                    return Mqtt.MqttClientDisconnectReason.AdministrativeAction;
                case MQTTnet.MqttClientDisconnectReason.PayloadFormatInvalid:
                    return Mqtt.MqttClientDisconnectReason.PayloadFormatInvalid;
                case MQTTnet.MqttClientDisconnectReason.RetainNotSupported:
                    return Mqtt.MqttClientDisconnectReason.RetainNotSupported;
                case MQTTnet.MqttClientDisconnectReason.QosNotSupported:
                    return Mqtt.MqttClientDisconnectReason.QosNotSupported;
                case MQTTnet.MqttClientDisconnectReason.UseAnotherServer:
                    return Mqtt.MqttClientDisconnectReason.UseAnotherServer;
                case MQTTnet.MqttClientDisconnectReason.ServerMoved:
                    return Mqtt.MqttClientDisconnectReason.ServerMoved;
                case MQTTnet.MqttClientDisconnectReason.SharedSubscriptionsNotSupported:
                    return Mqtt.MqttClientDisconnectReason.SharedSubscriptionsNotSupported;
                case MQTTnet.MqttClientDisconnectReason.ConnectionRateExceeded:
                    return Mqtt.MqttClientDisconnectReason.ConnectionRateExceeded;
                case MQTTnet.MqttClientDisconnectReason.MaximumConnectTime:
                    return Mqtt.MqttClientDisconnectReason.MaximumConnectTime;
                case MQTTnet.MqttClientDisconnectReason.SubscriptionIdentifiersNotSupported:
                    return Mqtt.MqttClientDisconnectReason.SubscriptionIdentifiersNotSupported;
                case MQTTnet.MqttClientDisconnectReason.WildcardSubscriptionsNotSupported:
                    return Mqtt.MqttClientDisconnectReason.WildcardSubscriptionsNotSupported;
                default:
                    return Mqtt.MqttClientDisconnectReason.UnspecifiedError;
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

        internal static Mqtt.MqttClientConnectResultCode ToGeneric(MQTTnet.MqttClientConnectResultCode resultCode)
        {
            switch (resultCode)
            {
                case MQTTnet.MqttClientConnectResultCode.Success:
                    return Mqtt.MqttClientConnectResultCode.Success;
                case MQTTnet.MqttClientConnectResultCode.UnspecifiedError:
                    return Mqtt.MqttClientConnectResultCode.UnspecifiedError;
                case MQTTnet.MqttClientConnectResultCode.MalformedPacket:
                    return Mqtt.MqttClientConnectResultCode.MalformedPacket;
                case MQTTnet.MqttClientConnectResultCode.ProtocolError:
                    return Mqtt.MqttClientConnectResultCode.ProtocolError;
                case MQTTnet.MqttClientConnectResultCode.ImplementationSpecificError:
                    return Mqtt.MqttClientConnectResultCode.ImplementationSpecificError;
                case MQTTnet.MqttClientConnectResultCode.UnsupportedProtocolVersion:
                    return Mqtt.MqttClientConnectResultCode.UnsupportedProtocolVersion;
                case MQTTnet.MqttClientConnectResultCode.ClientIdentifierNotValid:
                    return Mqtt.MqttClientConnectResultCode.ClientIdentifierNotValid;
                case MQTTnet.MqttClientConnectResultCode.BadUserNameOrPassword:
                    return Mqtt.MqttClientConnectResultCode.BadUserNameOrPassword;
                case MQTTnet.MqttClientConnectResultCode.NotAuthorized:
                    return Mqtt.MqttClientConnectResultCode.NotAuthorized;
                case MQTTnet.MqttClientConnectResultCode.ServerUnavailable:
                    return Mqtt.MqttClientConnectResultCode.ServerUnavailable;
                case MQTTnet.MqttClientConnectResultCode.ServerBusy:
                    return Mqtt.MqttClientConnectResultCode.ServerBusy;
                case MQTTnet.MqttClientConnectResultCode.Banned:
                    return Mqtt.MqttClientConnectResultCode.Banned;
                case MQTTnet.MqttClientConnectResultCode.BadAuthenticationMethod:
                    return Mqtt.MqttClientConnectResultCode.BadAuthenticationMethod;
                case MQTTnet.MqttClientConnectResultCode.TopicNameInvalid:
                    return Mqtt.MqttClientConnectResultCode.TopicNameInvalid;
                case MQTTnet.MqttClientConnectResultCode.PacketTooLarge:
                    return Mqtt.MqttClientConnectResultCode.PacketTooLarge;
                case MQTTnet.MqttClientConnectResultCode.QuotaExceeded:
                    return Mqtt.MqttClientConnectResultCode.QuotaExceeded;
                case MQTTnet.MqttClientConnectResultCode.PayloadFormatInvalid:
                    return Mqtt.MqttClientConnectResultCode.PayloadFormatInvalid;
                case MQTTnet.MqttClientConnectResultCode.RetainNotSupported:
                    return Mqtt.MqttClientConnectResultCode.RetainNotSupported;
                case MQTTnet.MqttClientConnectResultCode.QoSNotSupported:
                    return Mqtt.MqttClientConnectResultCode.QoSNotSupported;
                case MQTTnet.MqttClientConnectResultCode.UseAnotherServer:
                    return Mqtt.MqttClientConnectResultCode.UseAnotherServer;
                case MQTTnet.MqttClientConnectResultCode.ServerMoved:
                    return Mqtt.MqttClientConnectResultCode.ServerMoved;
                case MQTTnet.MqttClientConnectResultCode.ConnectionRateExceeded:
                default:
                    return Mqtt.MqttClientConnectResultCode.ConnectionRateExceeded;

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

        private static Mqtt.MqttClientPublishReasonCode ToGeneric(MQTTnet.MqttClientPublishReasonCode reasonCode)
        {
            switch (reasonCode)
            {
                case MQTTnet.MqttClientPublishReasonCode.Success:
                    return Mqtt.MqttClientPublishReasonCode.Success;
                case MQTTnet.MqttClientPublishReasonCode.NoMatchingSubscribers:
                    return Mqtt.MqttClientPublishReasonCode.NoMatchingSubscribers;
                case MQTTnet.MqttClientPublishReasonCode.UnspecifiedError:
                    return Mqtt.MqttClientPublishReasonCode.UnspecifiedError;
                case MQTTnet.MqttClientPublishReasonCode.ImplementationSpecificError:
                    return Mqtt.MqttClientPublishReasonCode.ImplementationSpecificError;
                case MQTTnet.MqttClientPublishReasonCode.NotAuthorized:
                    return Mqtt.MqttClientPublishReasonCode.NotAuthorized;
                case MQTTnet.MqttClientPublishReasonCode.TopicNameInvalid:
                    return Mqtt.MqttClientPublishReasonCode.TopicNameInvalid;
                case MQTTnet.MqttClientPublishReasonCode.PacketIdentifierInUse:
                    return Mqtt.MqttClientPublishReasonCode.PacketIdentifierInUse;
                case MQTTnet.MqttClientPublishReasonCode.QuotaExceeded:
                    return Mqtt.MqttClientPublishReasonCode.QuotaExceeded;
                case MQTTnet.MqttClientPublishReasonCode.PayloadFormatInvalid:
                default:
                    return Mqtt.MqttClientPublishReasonCode.PayloadFormatInvalid;

            }
        }

        internal static MqttSubscribeAck ToGeneric(MqttClientSubscribeResult suback)
        {
            MqttSubscribeAck generic = new()
            {
                UserProperties = ToGeneric(suback.UserProperties),
                ReasonString = suback.ReasonString
            };

            List<MqttSubscribeResult> genericItems = new();
            if (suback.Items != null)
            {
                foreach (var mqttnetSubscribeResultItem in suback.Items)
                {
                    genericItems.Add(new()
                    {
                        ResultCode = ToGeneric(mqttnetSubscribeResultItem.ResultCode),
                        TopicFilter = new(mqttnetSubscribeResultItem.TopicFilter.Topic, ToGeneric(mqttnetSubscribeResultItem.TopicFilter.QualityOfServiceLevel))
                    });
                }
            }

            generic.Items = genericItems;

            return generic;
        }

        private static Mqtt.MqttClientSubscribeResultCode ToGeneric(MQTTnet.MqttClientSubscribeResultCode resultCode)
        {
            switch (resultCode)
            {
                case MQTTnet.MqttClientSubscribeResultCode.GrantedQoS0:
                    return Mqtt.MqttClientSubscribeResultCode.GrantedQoS0;
                case MQTTnet.MqttClientSubscribeResultCode.GrantedQoS1:
                    return Mqtt.MqttClientSubscribeResultCode.GrantedQoS1;
                case MQTTnet.MqttClientSubscribeResultCode.GrantedQoS2:
                    return Mqtt.MqttClientSubscribeResultCode.GrantedQoS2;
                case MQTTnet.MqttClientSubscribeResultCode.UnspecifiedError:
                    return Mqtt.MqttClientSubscribeResultCode.UnspecifiedError;
                case MQTTnet.MqttClientSubscribeResultCode.ImplementationSpecificError:
                    return Mqtt.MqttClientSubscribeResultCode.ImplementationSpecificError;
                case MQTTnet.MqttClientSubscribeResultCode.NotAuthorized:
                    return Mqtt.MqttClientSubscribeResultCode.NotAuthorized;
                case MQTTnet.MqttClientSubscribeResultCode.TopicFilterInvalid:
                    return Mqtt.MqttClientSubscribeResultCode.TopicFilterInvalid;
                case MQTTnet.MqttClientSubscribeResultCode.PacketIdentifierInUse:
                    return Mqtt.MqttClientSubscribeResultCode.PacketIdentifierInUse;
                case MQTTnet.MqttClientSubscribeResultCode.QuotaExceeded:
                    return Mqtt.MqttClientSubscribeResultCode.QuotaExceeded;
                case MQTTnet.MqttClientSubscribeResultCode.SharedSubscriptionsNotSupported:
                    return Mqtt.MqttClientSubscribeResultCode.SharedSubscriptionsNotSupported;
                case MQTTnet.MqttClientSubscribeResultCode.SubscriptionIdentifiersNotSupported:
                    return Mqtt.MqttClientSubscribeResultCode.SubscriptionIdentifiersNotSupported;
                case MQTTnet.MqttClientSubscribeResultCode.WildcardSubscriptionsNotSupported:
                default:
                    return Mqtt.MqttClientSubscribeResultCode.WildcardSubscriptionsNotSupported;
            }
        }

        internal static MqttUnsubscribeAck ToGeneric(MqttClientUnsubscribeResult unsuback)
        {
            if (unsuback.Items.Count != 1)
            {
                throw new Exception("TODO");
            }


            return new()
            {
                ReasonString = unsuback.ReasonString,
                ResultCode = toGeneric(unsuback.Items.FirstOrDefault().ResultCode),
                UserProperties = ToGeneric(unsuback.UserProperties)
            };
        }

        private static Mqtt.MqttClientUnsubscribeResultCode toGeneric(MQTTnet.MqttClientUnsubscribeResultCode resultCode)
        {
            switch (resultCode)
            {
                case MQTTnet.MqttClientUnsubscribeResultCode.Success:
                    return Mqtt.MqttClientUnsubscribeResultCode.Success;
                case MQTTnet.MqttClientUnsubscribeResultCode.NoSubscriptionExisted:
                    return Mqtt.MqttClientUnsubscribeResultCode.NoSubscriptionExisted;
                case MQTTnet.MqttClientUnsubscribeResultCode.UnspecifiedError:
                    return Mqtt.MqttClientUnsubscribeResultCode.UnspecifiedError;
                case MQTTnet.MqttClientUnsubscribeResultCode.ImplementationSpecificError:
                    return Mqtt.MqttClientUnsubscribeResultCode.ImplementationSpecificError;
                case MQTTnet.MqttClientUnsubscribeResultCode.NotAuthorized:
                    return Mqtt.MqttClientUnsubscribeResultCode.NotAuthorized;
                case MQTTnet.MqttClientUnsubscribeResultCode.TopicFilterInvalid:
                    return Mqtt.MqttClientUnsubscribeResultCode.TopicFilterInvalid;
                case MQTTnet.MqttClientUnsubscribeResultCode.PacketIdentifierInUse:
                default:
                    return Mqtt.MqttClientUnsubscribeResultCode.PacketIdentifierInUse;
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
