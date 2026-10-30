// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Exceptions;
using Microsoft.Azure.Iot.Device.Mqtt;

namespace Microsoft.Azure.Iot.Device
{
    /// <summary>
    /// Converts error reason codes in MQTT acknowledgements into classified <see cref="DeviceException"/>s.
    /// </summary>
    internal static class MqttAckValidator
    {
        internal static void ThrowIfPubackHasErrorCode(MqttPublishAck puback)
        {
            switch (puback.ReasonCode)
            {
                case MqttPublishAckReasonCode.UnspecifiedError:
                case MqttPublishAckReasonCode.ImplementationSpecificError:
                case MqttPublishAckReasonCode.QuotaExceeded:
                    throw new DeviceException($"Received MQTT puback code {puback.ReasonCode}")
                    {
                        Retryability = ErrorRetryability.Retryable,
                        IsContained = true,
                    };

                case MqttPublishAckReasonCode.NotAuthorized:
                case MqttPublishAckReasonCode.PacketIdentifierInUse:
                case MqttPublishAckReasonCode.TopicNameInvalid:
                case MqttPublishAckReasonCode.PayloadFormatInvalid:
                    throw new DeviceException($"Received MQTT puback code {puback.ReasonCode}")
                    {
                        Retryability = ErrorRetryability.Terminal,
                        IsContained = true,
                    };

                default:
                    return; // benign case, do not throw any exception
            }
        }

        internal static void ThrowIfSubackHasErrorCode(MqttSubscribeAck suback)
        {
            foreach (var subackItem in suback.Items)
            {
                switch (subackItem.ReasonCode)
                {
                    case MqttClientSubscribeReasonCode.UnspecifiedError:
                    case MqttClientSubscribeReasonCode.ImplementationSpecificError:
                    case MqttClientSubscribeReasonCode.QuotaExceeded:
                        throw new DeviceException($"Received MQTT suback code {subackItem.ReasonCode} when subscribing to topic {subackItem.TopicFilter}")
                        {
                            Retryability = ErrorRetryability.Retryable,
                            IsContained = true,
                        };

                    case MqttClientSubscribeReasonCode.NotAuthorized:
                    case MqttClientSubscribeReasonCode.TopicFilterInvalid:
                    case MqttClientSubscribeReasonCode.SharedSubscriptionsNotSupported:
                    case MqttClientSubscribeReasonCode.WildcardSubscriptionsNotSupported:
                        throw new DeviceException($"Received MQTT suback code {subackItem.ReasonCode} when subscribing to topic {subackItem.TopicFilter}")
                        {
                            Retryability = ErrorRetryability.Terminal,
                            IsContained = true,
                        };

                    case MqttClientSubscribeReasonCode.PacketIdentifierInUse:
                    case MqttClientSubscribeReasonCode.SubscriptionIdentifiersNotSupported:
                        throw new DeviceException($"Received MQTT suback code {subackItem.ReasonCode} when subscribing to topic {subackItem.TopicFilter}")
                        {
                            Retryability = ErrorRetryability.Terminal,
                            IsContained = false,
                        };

                    default:
                        return; // benign case, do not throw any exception
                }
            }
        }

        internal static void ThrowIfUnsubackHasErrorCode(MqttUnsubscribeAck unsuback)
        {
            foreach (var unsubackItem in unsuback.Items)
            {
                switch (unsubackItem.ReasonCode)
                {
                    case MqttClientUnsubscribeReasonCode.UnspecifiedError:
                    case MqttClientUnsubscribeReasonCode.ImplementationSpecificError:
                        throw new DeviceException($"Received MQTT unsuback code {unsubackItem.ReasonCode} when subscribing to topic {unsubackItem.TopicFilter}")
                        {
                            Retryability = ErrorRetryability.Retryable,
                            IsContained = true,
                        };

                    case MqttClientUnsubscribeReasonCode.NotAuthorized:
                    case MqttClientUnsubscribeReasonCode.TopicFilterInvalid:
                    case MqttClientUnsubscribeReasonCode.PacketIdentifierInUse:
                        throw new DeviceException($"Received MQTT unsuback code {unsubackItem.ReasonCode} when subscribing to topic {unsubackItem.TopicFilter}")
                        {
                            Retryability = ErrorRetryability.Terminal,
                            IsContained = true,
                        };

                    default:
                        return; // benign case, do not throw any exception
                }
            }
        }
    }
}
