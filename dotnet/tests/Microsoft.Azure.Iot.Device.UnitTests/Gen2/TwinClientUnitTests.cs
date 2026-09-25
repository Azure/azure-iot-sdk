// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Google.Protobuf;
using Microsoft.Azure.Iot.Device.Gen2.Twin;
using Microsoft.Azure.Iot.Device.Models.Twin;
using Microsoft.Azure.Iot.Device.Mqtt;
using Microsoft.Azure.Iot.Device.Provisioning.Models;
using System.Text;
using System.Text.Json.Nodes;
using Xunit;

namespace Microsoft.Azure.Iot.Device.UnitTests.Gen2
{
    public class TwinClientUnitTests
    {
        private const string DeviceId = "someDeviceId";
        private const string OutgoingTwinTopic = "ih/someDeviceId/srv/twin";
        private const string IncomingTwinTopic = "ih/someDeviceId/dev/twin";

        [Fact]
        public async Task GetTwinAsync_PublishesGetRequestAndReturnsResponse()
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(ConnectionProfile.MqttV5, DeviceId),
            };
            using TwinClient twinClient = new(connection);

            // When the client publishes its get request, respond with a matching get-response message.
            connection.OnPublish += async (publish) =>
            {
                var response = new TwinGetResponse()
                {
                    DesiredVersion = 5,
                    ReportedVersion = 7,
                    DesiredPayload = ByteString.CopyFromUtf8("{\"fanSpeed\":10}"),
                    ReportedPayload = ByteString.CopyFromUtf8("{\"temperature\":21}"),
                };

                await connection.SimulateReceiveAsync(CreateInboundTwinPublish("get-response:1", publish.CorrelationData!, response.ToByteArray()));
                return new MqttPublishAck() { ReasonCode = MqttPublishAckReasonCode.Success };
            };

            DeviceTwin twin = await twinClient.GetTwinAsync(cancellationToken: TestContext.Current.CancellationToken);

            MqttPublish getPublish = Assert.Single(connection.PublishedMessages);
            Assert.Equal(OutgoingTwinTopic, getPublish.Topic);
            Assert.Equal("get:1", GetType(getPublish));

            Assert.Equal(5ul, twin.DesiredVersion);
            Assert.Equal(7ul, twin.ReportedVersion);
            Assert.Equal(10, (int)twin.Desired!["fanSpeed"]!);
            Assert.Equal(21, (int)twin.Reported!["temperature"]!);
        }

        [Fact]
        public async Task UpdateReportedPropertiesAsync_PublishesPatchAndReturnsResponse()
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(ConnectionProfile.MqttV5, DeviceId),
            };
            using TwinClient twinClient = new(connection);

            connection.OnPublish += async (publish) =>
            {
                var response = new ReportedPatchResponse()
                {
                    Result = Result.Ok,
                    Version = 12,
                };

                await connection.SimulateReceiveAsync(CreateInboundTwinPublish("reported-patch-response:1", publish.CorrelationData!, response.ToByteArray()));
                return new MqttPublishAck() { ReasonCode = MqttPublishAckReasonCode.Success };
            };

            ReportedPatchRequest patch = new()
            {
                ReportedProperties = new JsonObject() { ["temperature"] = 25 },
                IfMatch = 11,
            };

            ReportedPatchResponse response = await twinClient.UpdateReportedPropertiesAsync(patch, TestContext.Current.CancellationToken);

            MqttPublish patchPublish = Assert.Single(connection.PublishedMessages);
            Assert.Equal(OutgoingTwinTopic, patchPublish.Topic);
            Assert.Equal("reported-patch:1", GetType(patchPublish));

            Assert.Equal(Result.Ok, response.Result);
            Assert.Equal(12ul, response.Version);
        }

        [Fact]
        public async Task DesiredPatchReceived_RaisedOnInboundDesiredPatch()
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(ConnectionProfile.MqttV5, DeviceId),
            };
            using TwinClient twinClient = new(connection);

            DesiredPatchReceivedEventArgs? received = null;
            twinClient.DesiredPatchReceived += (args) => received = args;

            var desiredPatch = new DesiredPatch()
            {
                Version = 3,
                Payload = ByteString.CopyFromUtf8("{\"fanSpeed\":40}"),
            };

            await connection.SimulateReceiveAsync(CreateInboundTwinPublish("desired-patch:1", Guid.NewGuid().ToByteArray(), desiredPatch.ToByteArray()));

            Assert.NotNull(received);
            Assert.Equal(3ul, received!.DesiredPropertiesVersion);
            Assert.Equal(40, (int)received.DesiredProperties["fanSpeed"]!);
        }

        [Fact]
        public async Task HandleReceivedMqttPublish_IgnoresUnrelatedTopics()
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(ConnectionProfile.MqttV5, DeviceId),
            };
            using TwinClient twinClient = new(connection);

            bool eventRaised = false;
            twinClient.DesiredPatchReceived += (_) => eventRaised = true;

            await connection.SimulateReceiveAsync(new MqttPublish()
            {
                Topic = "ih/someDeviceId/dev/methods",
                Payload = Encoding.UTF8.GetBytes("irrelevant"),
            });

            Assert.False(eventRaised);
        }

        [Fact]
        public async Task GetTwinAsync_ThrowsWhenDisconnected()
        {
            MockFeatureConnectionClient connection = new();
            using TwinClient twinClient = new(connection);

            await Assert.ThrowsAsync<NotSupportedException>(
                async () => await twinClient.GetTwinAsync(cancellationToken: TestContext.Current.CancellationToken));
        }

        [Fact]
        public async Task GetTwinAsync_ThrowsAfterDispose()
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(ConnectionProfile.MqttV5, DeviceId),
            };
            TwinClient twinClient = new(connection);
            twinClient.Dispose();

            await Assert.ThrowsAsync<ObjectDisposedException>(
                async () => await twinClient.GetTwinAsync(cancellationToken: TestContext.Current.CancellationToken));
        }

        private static MqttPublish CreateInboundTwinPublish(string type, byte[] correlationData, byte[] payload)
        {
            var publish = new MqttPublish()
            {
                Topic = IncomingTwinTopic,
                Payload = payload,
                CorrelationData = correlationData,
                QualityOfServiceLevel = MqttQualityOfServiceLevel.AtMostOnce,
            };
            publish.UserProperties.Add(new("type", Encoding.UTF8.GetBytes(type)));
            return publish;
        }

        private static string GetType(MqttPublish publish)
        {
            return Assert.Single(publish.UserProperties, p => p.Name == "type").ReadValueAsString();
        }
    }
}
