// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Exceptions;
using Microsoft.Azure.Iot.Device.Mqtt;
using Xunit;

namespace Microsoft.Azure.Iot.Device.UnitTests
{
    public class MqttAckValidatorUnitTests
    {
        [Theory]
        [InlineData(MqttClientSubscribeReasonCode.UnspecifiedError, ErrorRetryability.Retryable, true)]
        [InlineData(MqttClientSubscribeReasonCode.ImplementationSpecificError, ErrorRetryability.Retryable, true)]
        [InlineData(MqttClientSubscribeReasonCode.QuotaExceeded, ErrorRetryability.Retryable, true)]
        [InlineData(MqttClientSubscribeReasonCode.NotAuthorized, ErrorRetryability.Terminal, true)]
        [InlineData(MqttClientSubscribeReasonCode.TopicFilterInvalid, ErrorRetryability.Terminal, true)]
        [InlineData(MqttClientSubscribeReasonCode.SharedSubscriptionsNotSupported, ErrorRetryability.Terminal, true)]
        [InlineData(MqttClientSubscribeReasonCode.WildcardSubscriptionsNotSupported, ErrorRetryability.Terminal, true)]
        [InlineData(MqttClientSubscribeReasonCode.PacketIdentifierInUse, ErrorRetryability.Terminal, false)]
        [InlineData(MqttClientSubscribeReasonCode.SubscriptionIdentifiersNotSupported, ErrorRetryability.Terminal, false)]
        public void Suback_ReportsAllFailuresAfterSuccess(
            MqttClientSubscribeReasonCode reasonCode,
            ErrorRetryability expectedRetryability,
            bool expectedIsContained)
        {
            var suback = CreateSuback(
                MqttClientSubscribeReasonCode.GrantedQoS1,
                reasonCode,
                MqttClientSubscribeReasonCode.GrantedQoS0,
                MqttClientSubscribeReasonCode.UnspecifiedError,
                MqttClientSubscribeReasonCode.GrantedQoS2);

            DeviceException exception = Assert.Throws<DeviceException>(
                () => MqttAckValidator.ThrowIfSubackHasErrorCode(suback));

            Assert.Equal(
                $"Received MQTT suback code {reasonCode} when subscribing to topic topic/1"
                + Environment.NewLine
                + "Received MQTT suback code UnspecifiedError when subscribing to topic topic/3",
                exception.Message);
            Assert.Equal(expectedRetryability, exception.Retryability);
            Assert.Equal(expectedIsContained, exception.IsContained);
        }

        [Theory]
        [InlineData(false)]
        [InlineData(true)]
        public void Suback_MixedFailures_UsesStrongestClassificationRegardlessOfOrder(bool reverse)
        {
            MqttClientSubscribeReasonCode[] reasons =
            [
                MqttClientSubscribeReasonCode.UnspecifiedError,
                MqttClientSubscribeReasonCode.PacketIdentifierInUse,
                MqttClientSubscribeReasonCode.NotAuthorized,
                MqttClientSubscribeReasonCode.QuotaExceeded,
            ];
            if (reverse)
            {
                Array.Reverse(reasons);
            }

            DeviceException exception = Assert.Throws<DeviceException>(
                () => MqttAckValidator.ThrowIfSubackHasErrorCode(CreateSuback(reasons)));

            Assert.Equal(ErrorRetryability.Terminal, exception.Retryability);
            Assert.False(exception.IsContained);
            for (int i = 0; i < reasons.Length; i++)
            {
                Assert.Contains(
                    $"Received MQTT suback code {reasons[i]} when subscribing to topic topic/{i}",
                    exception.Message);
            }
        }

        [Fact]
        public void Suback_EmptyOrAllSuccessful_DoesNotThrow()
        {
            MqttAckValidator.ThrowIfSubackHasErrorCode(CreateSuback());
            MqttAckValidator.ThrowIfSubackHasErrorCode(CreateSuback(
                MqttClientSubscribeReasonCode.GrantedQoS0,
                MqttClientSubscribeReasonCode.GrantedQoS1,
                MqttClientSubscribeReasonCode.GrantedQoS2));
        }

        [Theory]
        [InlineData(MqttClientUnsubscribeReasonCode.UnspecifiedError, ErrorRetryability.Retryable)]
        [InlineData(MqttClientUnsubscribeReasonCode.ImplementationSpecificError, ErrorRetryability.Retryable)]
        [InlineData(MqttClientUnsubscribeReasonCode.NotAuthorized, ErrorRetryability.Terminal)]
        [InlineData(MqttClientUnsubscribeReasonCode.TopicFilterInvalid, ErrorRetryability.Terminal)]
        [InlineData(MqttClientUnsubscribeReasonCode.PacketIdentifierInUse, ErrorRetryability.Terminal)]
        public void Unsuback_ReportsAllFailuresAfterSuccess(
            MqttClientUnsubscribeReasonCode reasonCode,
            ErrorRetryability expectedRetryability)
        {
            var unsuback = CreateUnsuback(
                MqttClientUnsubscribeReasonCode.Success,
                reasonCode,
                MqttClientUnsubscribeReasonCode.NoSubscriptionExisted,
                MqttClientUnsubscribeReasonCode.UnspecifiedError,
                MqttClientUnsubscribeReasonCode.Success);

            DeviceException exception = Assert.Throws<DeviceException>(
                () => MqttAckValidator.ThrowIfUnsubackHasErrorCode(unsuback));

            Assert.Equal(
                $"Received MQTT unsuback code {reasonCode} when unsubscribing from topic topic/1"
                + Environment.NewLine
                + "Received MQTT unsuback code UnspecifiedError when unsubscribing from topic topic/3",
                exception.Message);
            Assert.Equal(expectedRetryability, exception.Retryability);
            Assert.True(exception.IsContained);
        }

        [Theory]
        [InlineData(false)]
        [InlineData(true)]
        public void Unsuback_MixedFailures_UsesStrongestClassificationRegardlessOfOrder(bool reverse)
        {
            MqttClientUnsubscribeReasonCode[] reasons =
            [
                MqttClientUnsubscribeReasonCode.UnspecifiedError,
                MqttClientUnsubscribeReasonCode.NotAuthorized,
                MqttClientUnsubscribeReasonCode.ImplementationSpecificError,
            ];
            if (reverse)
            {
                Array.Reverse(reasons);
            }

            DeviceException exception = Assert.Throws<DeviceException>(
                () => MqttAckValidator.ThrowIfUnsubackHasErrorCode(CreateUnsuback(reasons)));

            Assert.Equal(ErrorRetryability.Terminal, exception.Retryability);
            Assert.True(exception.IsContained);
            for (int i = 0; i < reasons.Length; i++)
            {
                Assert.Contains(
                    $"Received MQTT unsuback code {reasons[i]} when unsubscribing from topic topic/{i}",
                    exception.Message);
            }
        }

        [Fact]
        public void Unsuback_EmptyOrAllSuccessful_DoesNotThrow()
        {
            MqttAckValidator.ThrowIfUnsubackHasErrorCode(CreateUnsuback());
            MqttAckValidator.ThrowIfUnsubackHasErrorCode(CreateUnsuback(
                MqttClientUnsubscribeReasonCode.Success,
                MqttClientUnsubscribeReasonCode.NoSubscriptionExisted));
        }

        private static MqttSubscribeAck CreateSuback(params MqttClientSubscribeReasonCode[] reasons)
        {
            return new MqttSubscribeAck
            {
                Items = reasons.Select((reason, index) => new MqttSubscribeAckItem
                {
                    ReasonCode = reason,
                    TopicFilter = new MqttTopicFilter($"topic/{index}", MqttQualityOfServiceLevel.AtLeastOnce),
                }).ToArray(),
            };
        }

        private static MqttUnsubscribeAck CreateUnsuback(params MqttClientUnsubscribeReasonCode[] reasons)
        {
            return new MqttUnsubscribeAck
            {
                Items = reasons.Select((reason, index) => new MqttUnsubscribeAckItem
                {
                    ReasonCode = reason,
                    TopicFilter = $"topic/{index}",
                }).ToArray(),
            };
        }
    }
}
