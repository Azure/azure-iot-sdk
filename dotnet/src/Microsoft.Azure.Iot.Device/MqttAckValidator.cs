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
            var errors = new List<string>();
            ErrorRetryability retryability = ErrorRetryability.Retryable;
            bool isContained = true;

            foreach (var subackItem in suback.Items)
            {
                switch (subackItem.ReasonCode)
                {
                    case MqttClientSubscribeReasonCode.UnspecifiedError:
                    case MqttClientSubscribeReasonCode.ImplementationSpecificError:
                    case MqttClientSubscribeReasonCode.QuotaExceeded:
                        break;

                    case MqttClientSubscribeReasonCode.NotAuthorized:
                    case MqttClientSubscribeReasonCode.TopicFilterInvalid:
                    case MqttClientSubscribeReasonCode.SharedSubscriptionsNotSupported:
                    case MqttClientSubscribeReasonCode.WildcardSubscriptionsNotSupported:
                        retryability = ErrorRetryability.Terminal;
                        break;

                    case MqttClientSubscribeReasonCode.PacketIdentifierInUse:
                    case MqttClientSubscribeReasonCode.SubscriptionIdentifiersNotSupported:
                        retryability = ErrorRetryability.Terminal;
                        isContained = false;
                        break;

                    default:
                        continue;
                }

                errors.Add($"Received MQTT suback code {subackItem.ReasonCode} when subscribing to topic {subackItem.TopicFilter.Topic}");
            }

            if (errors.Count > 0)
            {
                throw new DeviceException(string.Join(Environment.NewLine, errors))
                {
                    Retryability = retryability,
                    IsContained = isContained,
                };
            }
        }

        internal static void ThrowIfUnsubackHasErrorCode(MqttUnsubscribeAck unsuback)
        {
            var errors = new List<string>();
            ErrorRetryability retryability = ErrorRetryability.Retryable;

            foreach (var unsubackItem in unsuback.Items)
            {
                switch (unsubackItem.ReasonCode)
                {
                    case MqttClientUnsubscribeReasonCode.UnspecifiedError:
                    case MqttClientUnsubscribeReasonCode.ImplementationSpecificError:
                        break;

                    case MqttClientUnsubscribeReasonCode.NotAuthorized:
                    case MqttClientUnsubscribeReasonCode.TopicFilterInvalid:
                    case MqttClientUnsubscribeReasonCode.PacketIdentifierInUse:
                        retryability = ErrorRetryability.Terminal;
                        break;

                    default:
                        continue;
                }

                errors.Add($"Received MQTT unsuback code {unsubackItem.ReasonCode} when unsubscribing from topic {unsubackItem.TopicFilter}");
            }

            if (errors.Count > 0)
            {
                throw new DeviceException(string.Join(Environment.NewLine, errors))
                {
                    Retryability = retryability,
                    IsContained = true,
                };
            }
        }
    }
}
