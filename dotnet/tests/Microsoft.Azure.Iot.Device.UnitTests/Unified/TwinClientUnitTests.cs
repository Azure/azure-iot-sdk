// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Google.Protobuf;
using Microsoft.Azure.Iot.Device.Models.Twin;
using Microsoft.Azure.Iot.Device.Mqtt;
using Microsoft.Azure.Iot.Device.Provisioning.Models;
using Microsoft.Azure.Iot.Device.Unified.Twin;
using System.Text;
using System.Text.Json.Nodes;
using Xunit;

namespace Microsoft.Azure.Iot.Device.UnitTests.Unified
{
    public class TwinClientUnitTests
    {
        private const string DeviceId = "someDeviceId";

        [Fact]
        public async Task GetTwinAsync_Classic_PublishesGetAndReturnsResponse()
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(ConnectionProfile.Classic, DeviceId),
            };
            using TwinClient twinClient = new(connection);

            connection.OnPublish += async (publish) =>
            {
                string rid = ExtractRid(publish.Topic);
                string payload = "{\"desired\":{\"fanSpeed\":10,\"$version\":5},\"reported\":{\"temperature\":21,\"$version\":7}}";
                await connection.SimulateReceiveAsync(new MqttPublish()
                {
                    Topic = $"$iothub/twin/res/200/?$rid={rid}",
                    Payload = Encoding.UTF8.GetBytes(payload),
                    QualityOfServiceLevel = MqttQualityOfServiceLevel.AtMostOnce,
                });
                return new MqttPublishAck() { ReasonCode = MqttPublishAckReasonCode.Success };
            };

            DeviceTwin twin = await twinClient.GetTwinAsync(TestContext.Current.CancellationToken);

            MqttPublish getPublish = Assert.Single(connection.PublishedMessages);
            Assert.StartsWith("$iothub/twin/GET/?$rid=", getPublish.Topic);

            Assert.Equal(5ul, twin.DesiredVersion);
            Assert.Equal(7ul, twin.ReportedVersion);
            Assert.Equal(10, (int)twin.Desired!["fanSpeed"]!);
            Assert.Equal(21, (int)twin.Reported!["temperature"]!);
        }

        [Fact]
        public async Task UpdateReportedPropertiesAsync_Classic_PublishesPatchAndReturnsResponse()
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(ConnectionProfile.Classic, DeviceId),
            };
            using TwinClient twinClient = new(connection);

            connection.OnPublish += async (publish) =>
            {
                string rid = ExtractRid(publish.Topic);
                await connection.SimulateReceiveAsync(new MqttPublish()
                {
                    Topic = $"$iothub/twin/res/204/?$rid={rid}&$version=12",
                    QualityOfServiceLevel = MqttQualityOfServiceLevel.AtMostOnce,
                });
                return new MqttPublishAck() { ReasonCode = MqttPublishAckReasonCode.Success };
            };

            JsonObject reportedProperties = new() { ["temperature"] = 25 };
            ReportedPatchResponse response = await twinClient.UpdateReportedPropertiesAsync(reportedProperties, TestContext.Current.CancellationToken);

            MqttPublish patchPublish = Assert.Single(connection.PublishedMessages);
            Assert.StartsWith("$iothub/twin/PATCH/properties/reported/?$rid=", patchPublish.Topic);

            Assert.Equal(Result.Ok, response.Result);
            Assert.Equal(12ul, response.Version);
        }

        [Fact]
        public async Task DesiredPatchReceived_Classic_RaisedOnInboundPatch()
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(ConnectionProfile.Classic, DeviceId),
            };
            using TwinClient twinClient = new(connection);

            DesiredPatchReceivedEventArgs? received = null;
            twinClient.DesiredPatchReceived += (args) => received = args;

            await connection.SimulateReceiveAsync(new MqttPublish()
            {
                Topic = "$iothub/twin/PATCH/properties/desired/?$version=3",
                Payload = Encoding.UTF8.GetBytes("{\"fanSpeed\":40,\"$version\":3}"),
                QualityOfServiceLevel = MqttQualityOfServiceLevel.AtMostOnce,
            });

            Assert.NotNull(received);
            Assert.Equal(3ul, received!.DesiredPropertiesVersion);
            Assert.Equal(40, (int)received.DesiredProperties["fanSpeed"]!);
            Assert.Null(received.DesiredProperties[TwinClient.VersionKey]); // $version stripped
        }

        [Fact]
        public async Task GetTwinAsync_MqttV5_DelegatesToMqttv5Path()
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(ConnectionProfile.MqttV5, DeviceId),
            };
            using TwinClient twinClient = new(connection);

            connection.OnPublish += async (publish) =>
            {
                var response = new TwinGetResponse()
                {
                    DesiredVersion = 1,
                    ReportedVersion = 2,
                    DesiredPayload = ByteString.CopyFromUtf8("{\"fanSpeed\":10}"),
                    ReportedPayload = ByteString.CopyFromUtf8("{\"temperature\":21}"),
                };

                var inbound = new MqttPublish()
                {
                    Topic = $"ih/{DeviceId}/dev/twin",
                    Payload = response.ToByteArray(),
                    CorrelationData = publish.CorrelationData,
                    QualityOfServiceLevel = MqttQualityOfServiceLevel.AtMostOnce,
                };
                inbound.UserProperties.Add(new("type", Encoding.UTF8.GetBytes("get-response:1")));
                await connection.SimulateReceiveAsync(inbound);
                return new MqttPublishAck() { ReasonCode = MqttPublishAckReasonCode.Success };
            };

            DeviceTwin twin = await twinClient.GetTwinAsync(TestContext.Current.CancellationToken);

            MqttPublish getPublish = Assert.Single(connection.PublishedMessages);
            Assert.Equal($"ih/{DeviceId}/srv/twin", getPublish.Topic);
            Assert.Equal(1ul, twin.DesiredVersion);
            Assert.Equal(2ul, twin.ReportedVersion);
        }

        [Fact]
        public async Task GetTwinAsync_ThrowsWhenDisconnected()
        {
            MockFeatureConnectionClient connection = new();
            using TwinClient twinClient = new(connection);

            await Assert.ThrowsAsync<NotSupportedException>(
                async () => await twinClient.GetTwinAsync(TestContext.Current.CancellationToken));
        }

        [Fact]
        public async Task GetTwinAsync_ThrowsAfterDispose()
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(ConnectionProfile.Classic, DeviceId),
            };
            TwinClient twinClient = new(connection);
            twinClient.Dispose();

            await Assert.ThrowsAsync<ObjectDisposedException>(
                async () => await twinClient.GetTwinAsync(TestContext.Current.CancellationToken));
        }

        private static string ExtractRid(string topic)
        {
            int index = topic.IndexOf("$rid=", StringComparison.Ordinal);
            string rest = topic.Substring(index + "$rid=".Length);
            int ampersand = rest.IndexOf('&');
            return ampersand >= 0 ? rest.Substring(0, ampersand) : rest;
        }
    }
}
