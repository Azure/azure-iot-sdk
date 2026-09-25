// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using Microsoft.Azure.Iot.Device.MQTTnetAdapter;
using Microsoft.Azure.Iot.Device.Mqtt;
using Xunit;
using MqttNetUserProperty = MQTTnet.Packets.MqttUserProperty;
using MqttNetTopicFilter = MQTTnet.Packets.MqttTopicFilter;

namespace Microsoft.Azure.Iot.Device.UnitTests
{
    /// <summary>
    /// Unit tests for <see cref="ModelConverter"/>, which maps MQTTnet types onto the SDK's
    /// transport-agnostic MQTT types (and back).
    /// </summary>
    public class ModelConverterUnitTests
    {
        #region Helpers

        private static TGeneric ExpectedByName<TSource, TGeneric>(TSource source)
            where TSource : struct, Enum
            where TGeneric : struct, Enum
        {
            string name = source.ToString();
            Assert.True(
                Enum.TryParse(name, out TGeneric generic),
                $"Expected {typeof(TGeneric).Name} to contain a member named '{name}'.");
            return generic;
        }

        private static void AssertUserPropertiesMatch(
            IReadOnlyCollection<MqttUserProperty> actual,
            params (string Name, string Value)[] expected)
        {
            Assert.NotNull(actual);
            Assert.Equal(expected.Length, actual.Count);

            List<MqttUserProperty> actualList = actual.ToList();
            for (int i = 0; i < expected.Length; i++)
            {
                Assert.Equal(expected[i].Name, actualList[i].Name);
                Assert.Equal(expected[i].Value, Encoding.UTF8.GetString(actualList[i].Value.ToArray()));
            }
        }

        #endregion Helpers

        #region MqttClientConnectResult -> MqttConnectAck

        [Fact]
        public void ToGeneric_ConnectResult_MapsAllFields()
        {
            var connectResult = new MQTTnet.MqttClientConnectResult
            {
                ServerKeepAlive = 45,
                IsSessionPresent = true,
                MaximumPacketSize = 1024,
                ReasonString = "some reason",
                ReceiveMaximum = 12,
                ResponseInformation = "some response info",
                ResultCode = MQTTnet.MqttClientConnectResultCode.Success,
                SessionExpiryInterval = 3600,
                UserProperties = new List<MqttNetUserProperty>
                {
                    new("key1", "value1"),
                    new("key2", "value2"),
                },
            };

            MqttConnectAck connack = ModelConverter.ToGeneric(connectResult);

            Assert.Equal((ushort)45, connack.ServerKeepAlive);
            Assert.True(connack.IsSessionPresent);
            Assert.Equal((uint?)1024, connack.MaximumPacketSize);
            Assert.Equal("some reason", connack.ReasonString);
            Assert.Equal((ushort?)12, connack.ReceiveMaximum);
            Assert.Equal("some response info", connack.ResponseInformation);
            Assert.Equal(MqttConnectReasonCode.Success, connack.ResultCode);
            Assert.Equal((uint?)3600, connack.SessionExpiryInterval);
            AssertUserPropertiesMatch(connack.UserProperties, ("key1", "value1"), ("key2", "value2"));
        }

        [Fact]
        public void ToGeneric_ConnectResult_WithNullOptionalFields_MapsToNullsAndEmptyUserProperties()
        {
            var connectResult = new MQTTnet.MqttClientConnectResult
            {
                MaximumPacketSize = null,
                ReasonString = null,
                ReceiveMaximum = null,
                ResponseInformation = null,
                SessionExpiryInterval = null,
                UserProperties = null!,
            };

            MqttConnectAck connack = ModelConverter.ToGeneric(connectResult);

            Assert.Null(connack.MaximumPacketSize);
            Assert.Null(connack.ReasonString);
            Assert.Null(connack.ReceiveMaximum);
            Assert.Null(connack.ResponseInformation);
            Assert.Null(connack.SessionExpiryInterval);
            Assert.NotNull(connack.UserProperties);
            Assert.Empty(connack.UserProperties);
        }

        [Fact]
        public void ToGeneric_ConnectResult_PreservesNonUtf8UserPropertyBytes()
        {
            byte[] rawValue = [0x00, 0x01, 0xFF, 0x7F];
            var connectResult = new MQTTnet.MqttClientConnectResult
            {
                UserProperties = new List<MqttNetUserProperty> { new("binary", rawValue) },
            };

            MqttConnectAck connack = ModelConverter.ToGeneric(connectResult);

            MqttUserProperty actual = Assert.Single(connack.UserProperties);
            Assert.Equal("binary", actual.Name);
            Assert.Equal(rawValue, actual.Value.ToArray());
        }

        #endregion MqttClientConnectResult -> MqttConnectAck

        #region Enum conversions to generic

        [Fact]
        public void ToGeneric_ConnectResultCode_MapsEveryDefinedValue()
        {
            foreach (MQTTnet.MqttClientConnectResultCode resultCode in Enum.GetValues<MQTTnet.MqttClientConnectResultCode>())
            {
                MqttConnectReasonCode expected =
                    ExpectedByName<MQTTnet.MqttClientConnectResultCode, MqttConnectReasonCode>(resultCode);

                Assert.Equal(expected, ModelConverter.ToGeneric(resultCode));
            }
        }

        [Fact]
        public void ToGeneric_ConnectResultCode_UnrecognizedValue_DefaultsToConnectionRateExceeded()
        {
            Assert.Equal(
                MqttConnectReasonCode.ConnectionRateExceeded,
                ModelConverter.ToGeneric((MQTTnet.MqttClientConnectResultCode)250));
        }

        [Fact]
        public void ToGeneric_DisconnectReason_MapsEveryDefinedValue()
        {
            foreach (MQTTnet.MqttClientDisconnectReason reason in Enum.GetValues<MQTTnet.MqttClientDisconnectReason>())
            {
                // BadAuthenticationMethod is intentionally not mapped by ModelConverter and therefore
                // falls through to the default case.
                MqttDisconnectReason expected = reason == MQTTnet.MqttClientDisconnectReason.BadAuthenticationMethod
                    ? MqttDisconnectReason.UnspecifiedError
                    : ExpectedByName<MQTTnet.MqttClientDisconnectReason, MqttDisconnectReason>(reason);

                Assert.Equal(expected, ModelConverter.ToGeneric(reason));
            }
        }

        [Fact]
        public void ToGeneric_DisconnectReason_UnrecognizedValue_DefaultsToUnspecifiedError()
        {
            Assert.Equal(
                MqttDisconnectReason.UnspecifiedError,
                ModelConverter.ToGeneric((MQTTnet.MqttClientDisconnectReason)250));
        }

        [Theory]
        [InlineData(MQTTnet.Protocol.MqttPayloadFormatIndicator.Unspecified, MqttPayloadFormatIndicator.Unspecified)]
        [InlineData(MQTTnet.Protocol.MqttPayloadFormatIndicator.CharacterData, MqttPayloadFormatIndicator.CharacterData)]
        [InlineData((MQTTnet.Protocol.MqttPayloadFormatIndicator)99, MqttPayloadFormatIndicator.Unspecified)]
        public void ToGeneric_PayloadFormatIndicator_Maps(
            MQTTnet.Protocol.MqttPayloadFormatIndicator source,
            MqttPayloadFormatIndicator expected)
        {
            Assert.Equal(expected, ModelConverter.ToGeneric(source));
        }

        [Theory]
        [InlineData(MQTTnet.Protocol.MqttQualityOfServiceLevel.AtMostOnce, MqttQualityOfServiceLevel.AtMostOnce)]
        [InlineData(MQTTnet.Protocol.MqttQualityOfServiceLevel.AtLeastOnce, MqttQualityOfServiceLevel.AtLeastOnce)]
        [InlineData(MQTTnet.Protocol.MqttQualityOfServiceLevel.ExactlyOnce, MqttQualityOfServiceLevel.ExactlyOnce)]
        [InlineData((MQTTnet.Protocol.MqttQualityOfServiceLevel)99, MqttQualityOfServiceLevel.ExactlyOnce)]
        public void ToGeneric_QualityOfServiceLevel_Maps(
            MQTTnet.Protocol.MqttQualityOfServiceLevel source,
            MqttQualityOfServiceLevel expected)
        {
            Assert.Equal(expected, ModelConverter.ToGeneric(source));
        }

        #endregion Enum conversions to generic

        #region MqttClientPublishResult -> MqttPublishAck

        [Fact]
        public void ToGeneric_PublishResult_MapsAllFields()
        {
            var puback = new MQTTnet.MqttClientPublishResult(
                packetIdentifier: 7,
                reasonCode: MQTTnet.MqttClientPublishReasonCode.Success,
                reasonString: "published",
                userProperties: new List<MqttNetUserProperty> { new("pubKey", "pubValue") });

            MqttPublishAck generic = ModelConverter.ToGeneric(puback);

            Assert.Equal(MqttPublishAckReasonCode.Success, generic.ReasonCode);
            Assert.Equal("published", generic.ReasonString);
            AssertUserPropertiesMatch(generic.UserProperties, ("pubKey", "pubValue"));
        }

        [Fact]
        public void ToGeneric_PublishResult_WithNullUserProperties_ReturnsEmptyCollection()
        {
            var puback = new MQTTnet.MqttClientPublishResult(
                packetIdentifier: null,
                reasonCode: MQTTnet.MqttClientPublishReasonCode.Success,
                reasonString: null,
                userProperties: null!);

            MqttPublishAck generic = ModelConverter.ToGeneric(puback);

            Assert.Null(generic.ReasonString);
            Assert.NotNull(generic.UserProperties);
            Assert.Empty(generic.UserProperties);
        }

        [Fact]
        public void ToGeneric_PublishResult_MapsEveryDefinedReasonCode()
        {
            foreach (MQTTnet.MqttClientPublishReasonCode reasonCode in Enum.GetValues<MQTTnet.MqttClientPublishReasonCode>())
            {
                var puback = new MQTTnet.MqttClientPublishResult(null, reasonCode, null, []);

                MqttPublishAckReasonCode expected =
                    ExpectedByName<MQTTnet.MqttClientPublishReasonCode, MqttPublishAckReasonCode>(reasonCode);

                Assert.Equal(expected, ModelConverter.ToGeneric(puback).ReasonCode);
            }
        }

        [Fact]
        public void ToGeneric_PublishResult_UnrecognizedReasonCode_DefaultsToPayloadFormatInvalid()
        {
            var puback = new MQTTnet.MqttClientPublishResult(null, (MQTTnet.MqttClientPublishReasonCode)250, null, []);

            Assert.Equal(MqttPublishAckReasonCode.PayloadFormatInvalid, ModelConverter.ToGeneric(puback).ReasonCode);
        }

        #endregion MqttClientPublishResult -> MqttPublishAck

        #region MqttClientSubscribeResult -> MqttSubscribeAck

        [Fact]
        public void ToGeneric_SubscribeResult_MapsAllFieldsAndItems()
        {
            var items = new List<MQTTnet.MqttClientSubscribeResultItem>
            {
                new(
                    new MqttNetTopicFilter
                    {
                        Topic = "topic/one",
                        QualityOfServiceLevel = MQTTnet.Protocol.MqttQualityOfServiceLevel.AtLeastOnce,
                    },
                    MQTTnet.MqttClientSubscribeResultCode.GrantedQoS1),
                new(
                    new MqttNetTopicFilter
                    {
                        Topic = "topic/two",
                        QualityOfServiceLevel = MQTTnet.Protocol.MqttQualityOfServiceLevel.AtMostOnce,
                    },
                    MQTTnet.MqttClientSubscribeResultCode.NotAuthorized),
            };

            var suback = new MQTTnet.MqttClientSubscribeResult(
                packetIdentifier: 3,
                items: items,
                reasonString: "subscribed",
                userProperties: new List<MqttNetUserProperty> { new("subKey", "subValue") });

            MqttSubscribeAck generic = ModelConverter.ToGeneric(suback);

            Assert.Equal("subscribed", generic.ReasonString);
            AssertUserPropertiesMatch(generic.UserProperties, ("subKey", "subValue"));

            List<MqttSubscribeAckItem> genericItems = generic.Items.ToList();
            Assert.Equal(2, genericItems.Count);

            Assert.Equal(MqttClientSubscribeReasonCode.GrantedQoS1, genericItems[0].ReasonCode);
            Assert.Equal("topic/one", genericItems[0].TopicFilter.Topic);
            Assert.Equal(MqttQualityOfServiceLevel.AtLeastOnce, genericItems[0].TopicFilter.QualityOfServiceLevel);

            Assert.Equal(MqttClientSubscribeReasonCode.NotAuthorized, genericItems[1].ReasonCode);
            Assert.Equal("topic/two", genericItems[1].TopicFilter.Topic);
            Assert.Equal(MqttQualityOfServiceLevel.AtMostOnce, genericItems[1].TopicFilter.QualityOfServiceLevel);
        }

        [Fact]
        public void ToGeneric_SubscribeResult_WithMultipleTopicFilters_MapsEveryItemInOrder()
        {
            var items = new List<MQTTnet.MqttClientSubscribeResultItem>
            {
                new(
                    new MqttNetTopicFilter
                    {
                        Topic = "devices/dev1/messages/#",
                        QualityOfServiceLevel = MQTTnet.Protocol.MqttQualityOfServiceLevel.AtMostOnce,
                    },
                    MQTTnet.MqttClientSubscribeResultCode.GrantedQoS0),
                new(
                    new MqttNetTopicFilter
                    {
                        Topic = "devices/dev1/methods/+",
                        QualityOfServiceLevel = MQTTnet.Protocol.MqttQualityOfServiceLevel.AtLeastOnce,
                    },
                    MQTTnet.MqttClientSubscribeResultCode.GrantedQoS1),
                new(
                    new MqttNetTopicFilter
                    {
                        Topic = "devices/dev1/twin/res",
                        QualityOfServiceLevel = MQTTnet.Protocol.MqttQualityOfServiceLevel.ExactlyOnce,
                    },
                    MQTTnet.MqttClientSubscribeResultCode.GrantedQoS2),
                new(
                    new MqttNetTopicFilter
                    {
                        Topic = "devices/dev1/denied",
                        QualityOfServiceLevel = MQTTnet.Protocol.MqttQualityOfServiceLevel.AtLeastOnce,
                    },
                    MQTTnet.MqttClientSubscribeResultCode.NotAuthorized),
            };

            var suback = new MQTTnet.MqttClientSubscribeResult(
                packetIdentifier: 11,
                items: items,
                reasonString: "partially granted",
                userProperties: new List<MqttNetUserProperty> { new("k1", "v1"), new("k2", "v2") });

            MqttSubscribeAck generic = ModelConverter.ToGeneric(suback);

            Assert.Equal("partially granted", generic.ReasonString);
            AssertUserPropertiesMatch(generic.UserProperties, ("k1", "v1"), ("k2", "v2"));

            List<MqttSubscribeAckItem> genericItems = generic.Items.ToList();
            Assert.Equal(items.Count, genericItems.Count);

            (string Topic, MqttQualityOfServiceLevel Qos, MqttClientSubscribeReasonCode ResultCode)[] expected =
            [
                ("devices/dev1/messages/#", MqttQualityOfServiceLevel.AtMostOnce, MqttClientSubscribeReasonCode.GrantedQoS0),
                ("devices/dev1/methods/+", MqttQualityOfServiceLevel.AtLeastOnce, MqttClientSubscribeReasonCode.GrantedQoS1),
                ("devices/dev1/twin/res", MqttQualityOfServiceLevel.ExactlyOnce, MqttClientSubscribeReasonCode.GrantedQoS2),
                ("devices/dev1/denied", MqttQualityOfServiceLevel.AtLeastOnce, MqttClientSubscribeReasonCode.NotAuthorized),
            ];

            for (int i = 0; i < expected.Length; i++)
            {
                Assert.Equal(expected[i].Topic, genericItems[i].TopicFilter.Topic);
                Assert.Equal(expected[i].Qos, genericItems[i].TopicFilter.QualityOfServiceLevel);
                Assert.Equal(expected[i].ResultCode, genericItems[i].ReasonCode);
            }
        }

        [Fact]
        public void ToGeneric_SubscribeResult_WithDuplicateTopicFilters_MapsEachItemSeparately()
        {
            var suback = new MQTTnet.MqttClientSubscribeResult(
                packetIdentifier: 12,
                items:
                [
                    new(
                        new MqttNetTopicFilter
                        {
                            Topic = "same/topic",
                            QualityOfServiceLevel = MQTTnet.Protocol.MqttQualityOfServiceLevel.AtLeastOnce,
                        },
                        MQTTnet.MqttClientSubscribeResultCode.GrantedQoS1),
                    new(
                        new MqttNetTopicFilter
                        {
                            Topic = "same/topic",
                            QualityOfServiceLevel = MQTTnet.Protocol.MqttQualityOfServiceLevel.AtLeastOnce,
                        },
                        MQTTnet.MqttClientSubscribeResultCode.QuotaExceeded),
                ],
                reasonString: null,
                userProperties: []);

            List<MqttSubscribeAckItem> genericItems = ModelConverter.ToGeneric(suback).Items.ToList();

            Assert.Equal(2, genericItems.Count);
            Assert.All(genericItems, item => Assert.Equal("same/topic", item.TopicFilter.Topic));
            Assert.Equal(MqttClientSubscribeReasonCode.GrantedQoS1, genericItems[0].ReasonCode);
            Assert.Equal(MqttClientSubscribeReasonCode.QuotaExceeded, genericItems[1].ReasonCode);
            Assert.NotSame(genericItems[0].TopicFilter, genericItems[1].TopicFilter);
        }

        [Fact]
        public void ToGeneric_SubscribeResult_WithManyTopicFilters_MapsEveryDefinedResultCodeInOneAck()
        {
            MQTTnet.MqttClientSubscribeResultCode[] resultCodes = Enum.GetValues<MQTTnet.MqttClientSubscribeResultCode>();

            var items = resultCodes
                .Select((resultCode, index) => new MQTTnet.MqttClientSubscribeResultItem(
                    new MqttNetTopicFilter
                    {
                        Topic = $"topic/{index}",
                        QualityOfServiceLevel = MQTTnet.Protocol.MqttQualityOfServiceLevel.AtLeastOnce,
                    },
                    resultCode))
                .ToList();

            var suback = new MQTTnet.MqttClientSubscribeResult(13, items, null, []);

            List<MqttSubscribeAckItem> genericItems = ModelConverter.ToGeneric(suback).Items.ToList();

            Assert.Equal(resultCodes.Length, genericItems.Count);
            for (int i = 0; i < resultCodes.Length; i++)
            {
                Assert.Equal($"topic/{i}", genericItems[i].TopicFilter.Topic);
                Assert.Equal(
                    ExpectedByName<MQTTnet.MqttClientSubscribeResultCode, MqttClientSubscribeReasonCode>(resultCodes[i]),
                    genericItems[i].ReasonCode);
            }
        }

        [Fact]
        public void ToGeneric_SubscribeResult_WithNoItems_ReturnsEmptyItems()
        {
            var suback = new MQTTnet.MqttClientSubscribeResult(0, [], null, []);

            MqttSubscribeAck generic = ModelConverter.ToGeneric(suback);

            Assert.Empty(generic.Items);
            Assert.Empty(generic.UserProperties);
            Assert.Null(generic.ReasonString);
        }

        [Fact]
        public void ToGeneric_SubscribeResult_MapsEveryDefinedResultCode()
        {
            foreach (MQTTnet.MqttClientSubscribeResultCode resultCode in Enum.GetValues<MQTTnet.MqttClientSubscribeResultCode>())
            {
                var suback = new MQTTnet.MqttClientSubscribeResult(
                    0,
                    [new(new MqttNetTopicFilter { Topic = "some/topic" }, resultCode)],
                    null,
                    []);

                MqttClientSubscribeReasonCode expected =
                    ExpectedByName<MQTTnet.MqttClientSubscribeResultCode, MqttClientSubscribeReasonCode>(resultCode);

                Assert.Equal(expected, Assert.Single(ModelConverter.ToGeneric(suback).Items).ReasonCode);
            }
        }

        [Fact]
        public void ToGeneric_SubscribeResult_UnrecognizedResultCode_DefaultsToWildcardSubscriptionsNotSupported()
        {
            var suback = new MQTTnet.MqttClientSubscribeResult(
                0,
                [new(new MqttNetTopicFilter { Topic = "some/topic" }, (MQTTnet.MqttClientSubscribeResultCode)250)],
                null,
                []);

            Assert.Equal(
                MqttClientSubscribeReasonCode.WildcardSubscriptionsNotSupported,
                Assert.Single(ModelConverter.ToGeneric(suback).Items).ReasonCode);
        }

        #endregion MqttClientSubscribeResult -> MqttSubscribeAck

        #region MqttClientUnsubscribeResult -> MqttUnsubscribeAck

        [Fact]
        public void ToGeneric_UnsubscribeResult_MapsAllFieldsAndItems()
        {
            var unsuback = new MQTTnet.MqttClientUnsubscribeResult(
                packetIdentifier: 9,
                items: [new("topic/one", MQTTnet.MqttClientUnsubscribeResultCode.Success)],
                reasonString: "unsubscribed",
                userProperties: new List<MqttNetUserProperty> { new("unsubKey", "unsubValue") });

            MqttUnsubscribeAck generic = ModelConverter.ToGeneric(unsuback);

            Assert.Equal("unsubscribed", generic.ReasonString);
            AssertUserPropertiesMatch(generic.UserProperties, ("unsubKey", "unsubValue"));

            MqttUnsubscribeAckItem item = Assert.Single(generic.Items);
            Assert.Equal(MqttClientUnsubscribeReasonCode.Success, item.ReasonCode);
            Assert.Equal("topic/one", item.TopicFilter);
        }

        [Fact]
        public void ToGeneric_UnsubscribeResult_MapsEveryDefinedResultCode()
        {
            foreach (MQTTnet.MqttClientUnsubscribeResultCode resultCode in Enum.GetValues<MQTTnet.MqttClientUnsubscribeResultCode>())
            {
                var unsuback = new MQTTnet.MqttClientUnsubscribeResult(
                    0,
                    [new("some/topic", resultCode)],
                    null,
                    []);

                MqttClientUnsubscribeReasonCode expected =
                    ExpectedByName<MQTTnet.MqttClientUnsubscribeResultCode, MqttClientUnsubscribeReasonCode>(resultCode);

                Assert.Equal(expected, Assert.Single(ModelConverter.ToGeneric(unsuback).Items).ReasonCode);
            }
        }

        [Fact]
        public void ToGeneric_UnsubscribeResult_UnrecognizedResultCode_DefaultsToPacketIdentifierInUse()
        {
            var unsuback = new MQTTnet.MqttClientUnsubscribeResult(
                0,
                [new("some/topic", (MQTTnet.MqttClientUnsubscribeResultCode)250)],
                null,
                []);

            Assert.Equal(
                MqttClientUnsubscribeReasonCode.PacketIdentifierInUse,
                Assert.Single(ModelConverter.ToGeneric(unsuback).Items).ReasonCode);
        }

        [Fact]
        public void ToGeneric_UnsubscribeResult_WithNoItems_ReturnsEmptyItems()
        {
            var unsuback = new MQTTnet.MqttClientUnsubscribeResult(0, [], null, []);

            MqttUnsubscribeAck generic = ModelConverter.ToGeneric(unsuback);

            Assert.Empty(generic.Items);
            Assert.Empty(generic.UserProperties);
            Assert.Null(generic.ReasonString);
        }

        [Theory]
        [InlineData(2)]
        [InlineData(3)]
        [InlineData(10)]
        public void ToGeneric_UnsubscribeResult_WithMultipleTopicFilters_MapsEveryItemInOrder(int topicFilterCount)
        {
            var items = Enumerable
                .Range(0, topicFilterCount)
                .Select(i => new MQTTnet.MqttClientUnsubscribeResultItem(
                    $"topic/{i}",
                    MQTTnet.MqttClientUnsubscribeResultCode.Success))
                .ToList();

            var unsuback = new MQTTnet.MqttClientUnsubscribeResult(
                packetIdentifier: 14,
                items: items,
                reasonString: "unsubscribed",
                userProperties: new List<MqttNetUserProperty> { new("k1", "v1"), new("k2", "v2") });

            MqttUnsubscribeAck generic = ModelConverter.ToGeneric(unsuback);

            Assert.Equal("unsubscribed", generic.ReasonString);
            AssertUserPropertiesMatch(generic.UserProperties, ("k1", "v1"), ("k2", "v2"));

            List<MqttUnsubscribeAckItem> genericItems = generic.Items.ToList();
            Assert.Equal(topicFilterCount, genericItems.Count);
            for (int i = 0; i < topicFilterCount; i++)
            {
                Assert.Equal($"topic/{i}", genericItems[i].TopicFilter);
                Assert.Equal(MqttClientUnsubscribeReasonCode.Success, genericItems[i].ReasonCode);
            }
        }

        [Fact]
        public void ToGeneric_UnsubscribeResult_WithMultipleTopicFiltersAndMixedResultCodes_MapsEachResultCode()
        {
            var unsuback = new MQTTnet.MqttClientUnsubscribeResult(
                15,
                [
                    new("topic/one", MQTTnet.MqttClientUnsubscribeResultCode.Success),
                    new("topic/two", MQTTnet.MqttClientUnsubscribeResultCode.NoSubscriptionExisted),
                    new("topic/three", MQTTnet.MqttClientUnsubscribeResultCode.NotAuthorized),
                    new("topic/four", MQTTnet.MqttClientUnsubscribeResultCode.TopicFilterInvalid),
                ],
                "mixed results",
                new List<MqttNetUserProperty> { new("k1", "v1") });

            List<MqttUnsubscribeAckItem> genericItems = ModelConverter.ToGeneric(unsuback).Items.ToList();

            (string Topic, MqttClientUnsubscribeReasonCode ResultCode)[] expected =
            [
                ("topic/one", MqttClientUnsubscribeReasonCode.Success),
                ("topic/two", MqttClientUnsubscribeReasonCode.NoSubscriptionExisted),
                ("topic/three", MqttClientUnsubscribeReasonCode.NotAuthorized),
                ("topic/four", MqttClientUnsubscribeReasonCode.TopicFilterInvalid),
            ];

            Assert.Equal(expected.Length, genericItems.Count);
            for (int i = 0; i < expected.Length; i++)
            {
                Assert.Equal(expected[i].Topic, genericItems[i].TopicFilter);
                Assert.Equal(expected[i].ResultCode, genericItems[i].ReasonCode);
            }
        }

        [Fact]
        public void ToGeneric_UnsubscribeResult_WithManyTopicFilters_MapsEveryDefinedResultCodeInOneAck()
        {
            MQTTnet.MqttClientUnsubscribeResultCode[] resultCodes = Enum.GetValues<MQTTnet.MqttClientUnsubscribeResultCode>();

            var items = resultCodes
                .Select((resultCode, index) => new MQTTnet.MqttClientUnsubscribeResultItem($"topic/{index}", resultCode))
                .ToList();

            var unsuback = new MQTTnet.MqttClientUnsubscribeResult(17, items, null, []);

            List<MqttUnsubscribeAckItem> genericItems = ModelConverter.ToGeneric(unsuback).Items.ToList();

            Assert.Equal(resultCodes.Length, genericItems.Count);
            for (int i = 0; i < resultCodes.Length; i++)
            {
                Assert.Equal($"topic/{i}", genericItems[i].TopicFilter);
                Assert.Equal(
                    ExpectedByName<MQTTnet.MqttClientUnsubscribeResultCode, MqttClientUnsubscribeReasonCode>(resultCodes[i]),
                    genericItems[i].ReasonCode);
            }
        }

        [Fact]
        public void ToGeneric_UnsubscribeResult_WithDuplicateTopicFilters_MapsEachItemSeparately()
        {
            var unsuback = new MQTTnet.MqttClientUnsubscribeResult(
                16,
                [
                    new("same/topic", MQTTnet.MqttClientUnsubscribeResultCode.Success),
                    new("same/topic", MQTTnet.MqttClientUnsubscribeResultCode.NoSubscriptionExisted),
                ],
                null,
                []);

            List<MqttUnsubscribeAckItem> genericItems = ModelConverter.ToGeneric(unsuback).Items.ToList();

            Assert.Equal(2, genericItems.Count);
            Assert.All(genericItems, item => Assert.Equal("same/topic", item.TopicFilter));
            Assert.Equal(MqttClientUnsubscribeReasonCode.Success, genericItems[0].ReasonCode);
            Assert.Equal(MqttClientUnsubscribeReasonCode.NoSubscriptionExisted, genericItems[1].ReasonCode);
        }

        #endregion MqttClientUnsubscribeResult -> MqttUnsubscribeAck

        #region Generic -> MQTTnet

        [Fact]
        public void ToMqttNet_DisconnectOptionsReason_MapsEveryDefinedValue()
        {
            foreach (MqttDisconnectReasonCode reason in Enum.GetValues<MqttDisconnectReasonCode>())
            {
                MQTTnet.MqttClientDisconnectOptionsReason expected =
                    ExpectedByName<MqttDisconnectReasonCode, MQTTnet.MqttClientDisconnectOptionsReason>(reason);

                Assert.Equal(expected, ModelConverter.ToMqttNet(reason));
            }
        }

        [Fact]
        public void ToMqttNet_DisconnectOptionsReason_UnrecognizedValue_DefaultsToPayloadFormatInvalid()
        {
            Assert.Equal(
                MQTTnet.MqttClientDisconnectOptionsReason.PayloadFormatInvalid,
                ModelConverter.ToMqttNet((MqttDisconnectReasonCode)250));
        }

        [Theory]
        [InlineData(MqttPayloadFormatIndicator.Unspecified, MQTTnet.Protocol.MqttPayloadFormatIndicator.Unspecified)]
        [InlineData(MqttPayloadFormatIndicator.CharacterData, MQTTnet.Protocol.MqttPayloadFormatIndicator.CharacterData)]
        [InlineData((MqttPayloadFormatIndicator)99, MQTTnet.Protocol.MqttPayloadFormatIndicator.Unspecified)]
        public void ToMqttNet_PayloadFormatIndicator_Maps(
            MqttPayloadFormatIndicator source,
            MQTTnet.Protocol.MqttPayloadFormatIndicator expected)
        {
            Assert.Equal(expected, ModelConverter.ToMqttNet(source));
        }

        [Theory]
        [InlineData(MqttQualityOfServiceLevel.AtMostOnce, MQTTnet.Protocol.MqttQualityOfServiceLevel.AtMostOnce)]
        [InlineData(MqttQualityOfServiceLevel.AtLeastOnce, MQTTnet.Protocol.MqttQualityOfServiceLevel.AtLeastOnce)]
        [InlineData(MqttQualityOfServiceLevel.ExactlyOnce, MQTTnet.Protocol.MqttQualityOfServiceLevel.ExactlyOnce)]
        [InlineData((MqttQualityOfServiceLevel)99, MQTTnet.Protocol.MqttQualityOfServiceLevel.ExactlyOnce)]
        public void ToMqttNet_QualityOfServiceLevel_Maps(
            MqttQualityOfServiceLevel source,
            MQTTnet.Protocol.MqttQualityOfServiceLevel expected)
        {
            Assert.Equal(expected, ModelConverter.ToMqttNet(source));
        }

        [Fact]
        public void QualityOfServiceLevel_RoundTripsThroughBothConverters()
        {
            foreach (MqttQualityOfServiceLevel qos in Enum.GetValues<MqttQualityOfServiceLevel>())
            {
                Assert.Equal(qos, ModelConverter.ToGeneric(ModelConverter.ToMqttNet(qos)));
            }
        }

        [Fact]
        public void PayloadFormatIndicator_RoundTripsThroughBothConverters()
        {
            foreach (MqttPayloadFormatIndicator indicator in Enum.GetValues<MqttPayloadFormatIndicator>())
            {
                Assert.Equal(indicator, ModelConverter.ToGeneric(ModelConverter.ToMqttNet(indicator)));
            }
        }

        #endregion Generic -> MQTTnet
    }
}
