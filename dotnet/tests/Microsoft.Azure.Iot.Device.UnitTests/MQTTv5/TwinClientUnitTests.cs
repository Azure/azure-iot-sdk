// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Google.Protobuf;
using Microsoft.Azure.Iot.Device.Exceptions;
using Microsoft.Azure.Iot.Device.MQTTv5.Connection;
using Microsoft.Azure.Iot.Device.MQTTv5.Twin;
using Microsoft.Azure.Iot.Device.Models.Twin;
using Microsoft.Azure.Iot.Device.Mqtt;
using Microsoft.Azure.Iot.Device.Models;
using System.Text;
using System.Text.Json.Nodes;
using Xunit;

namespace Microsoft.Azure.Iot.Device.UnitTests.MQTTv5
{
    public class TwinClientUnitTests
    {
        private const string DeviceId = "someDeviceId";
        private const string IncomingTwinTopic = "ih/someDeviceId/dev/twin";

        [Fact]
        public async Task UpdateReportedPropertiesAsync_SendsOnceWithGivenIfMatchAndReturnsResponse()
        {
            (MockFeatureConnectionClient connection, _) = CreateConnectionWithNonce();
            using TwinClient twinClient = new(connection);

            Task<ReportedPatchResponse> update = twinClient.UpdateReportedPropertiesAsync(
                new() { ReportedProperties = new JsonObject { ["temperature"] = 21 } },
                ifMatch: 7,
                TestContext.Current.CancellationToken);

            // Sent right away, without waiting for a birth ack.
            MqttPublish patchPublish = Assert.Single(connection.PublishedMessages);
            ReportedPatch patch = ReportedPatch.Parser.ParseFrom(patchPublish.Payload);
            Assert.Equal(7ul, patch.IfMatch);
            Assert.Equal(21, (int)JsonNode.Parse(patch.Payload.ToStringUtf8())!["temperature"]!);

            var response = new ReportedPatchResponse() { Result = Result.VersionMismatch, Version = 9 };
            await connection.SimulateReceiveAsync(CreateInboundTwinPublish("reported-patch-response:1", patchPublish.CorrelationData!, response.ToByteArray()));

            // The rejection is returned as is, rather than retried.
            ReportedPatchResponse result = await update;
            Assert.Equal(Result.VersionMismatch, result.Result);
            Assert.Equal(9ul, result.Version);
            Assert.Single(connection.PublishedMessages);
        }

        [Fact]
        public async Task ReceivedTwinMessages_AreRaisedAsReceived()
        {
            (MockFeatureConnectionClient connection, Guid nonce) = CreateConnectionWithNonce();
            using TwinClient twinClient = new(connection);
            List<ulong> patchVersions = new();
            List<ulong> probeVersions = new();
            List<ulong> pushVersions = new();
            twinClient.DesiredPatchReceived += args => patchVersions.Add(args.DesiredPropertiesVersion);
            twinClient.DesiredVersionProbeReceived += args => probeVersions.Add(args.DesiredPropertiesVersion);
            twinClient.TwinPushReceived += args => pushVersions.Add(args.Desired!.PropertiesVersion);

            // Out of order, and with gaps, none of which this client corrects.
            await ReceiveDesiredPatchAsync(connection, nonce, 5, "{\"a\":1}");
            await ReceiveDesiredPatchAsync(connection, nonce, 3, "{\"a\":2}");
            await ReceiveDesiredPatchAsync(connection, nonce, 8, payload: null);
            await ReceiveTwinPushAsync(connection, nonce, 2, "{\"a\":3}");

            // Messages for an earlier connection are still dropped.
            await ReceiveDesiredPatchAsync(connection, Guid.NewGuid(), 9, "{\"a\":4}");

            Assert.Equal([5ul, 3ul], patchVersions);
            Assert.Equal([8ul], probeVersions);
            Assert.Equal([2ul], pushVersions);
            Assert.Empty(connection.PublishedMessages);
        }

        [Fact]
        public async Task GetTwinAsync_FailsWithRetryableErrorWhenConnectionIsLost()
        {
            (MockFeatureConnectionClient connection, _) = CreateConnectionWithNonce();
            using TwinClient twinClient = new(connection);

            Task<DeviceTwin> getTwin = twinClient.GetTwinAsync(cancellationToken: TestContext.Current.CancellationToken);
            Assert.Single(connection.PublishedMessages);

            connection.SimulateConnectionLost();

            DeviceException e = await Assert.ThrowsAsync<DeviceException>(() => getTwin);
            Assert.Equal(ErrorRetryability.Retryable, e.Retryability);
            Assert.Single(connection.PublishedMessages);
        }

        [Fact]
        public async Task Requests_FailWithRetryableErrorWithoutSendingWhenConnectionIsLostWhileRegistering()
        {
            (MockFeatureConnectionClient connection, _) = CreateConnectionWithNonce();
            using TwinClient twinClient = new(connection);

            // The connection is lost after the request checks that it is connected, but before it is registered.
            connection.OnGetCurrentConnectionContext = connection.SimulateConnectionLost;

            DeviceException getError = await Assert.ThrowsAsync<DeviceException>(() => twinClient.GetTwinAsync(cancellationToken: TestContext.Current.CancellationToken));
            Assert.Equal(ErrorRetryability.Retryable, getError.Retryability);
            DeviceException patchError = await Assert.ThrowsAsync<DeviceException>(() => twinClient.UpdateReportedPropertiesAsync(
                new() { ReportedProperties = new JsonObject() },
                cancellationToken: TestContext.Current.CancellationToken));
            Assert.Equal(ErrorRetryability.Retryable, patchError.Retryability);
            Assert.Empty(connection.PublishedMessages);
        }

        [Fact]
        public async Task Requests_ThrowWhenNotConnected()
        {
            MockFeatureConnectionClient connection = new();
            using TwinClient twinClient = new(connection);

            await Assert.ThrowsAsync<NotSupportedException>(() => twinClient.GetTwinAsync(cancellationToken: TestContext.Current.CancellationToken));
            await Assert.ThrowsAsync<NotSupportedException>(() => twinClient.UpdateReportedPropertiesAsync(
                new() { ReportedProperties = new JsonObject() },
                cancellationToken: TestContext.Current.CancellationToken));
            Assert.Empty(connection.PublishedMessages);
        }

        private static (MockFeatureConnectionClient, Guid) CreateConnectionWithNonce()
        {
            Guid nonce = Guid.NewGuid();
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv5, DeviceId),
                ConnectionNonce = nonce,
            };
            return (connection, nonce);
        }

        private static Task ReceiveDesiredPatchAsync(MockFeatureConnectionClient connection, Guid nonce, ulong version, string? payload)
        {
            var patch = new DesiredPatch() { Version = version };
            if (payload != null)
            {
                patch.Payload = ByteString.CopyFromUtf8(payload);
            }

            return connection.SimulateReceiveAsync(CreateInboundTwinPublish("desired-patch:1", nonce.ToByteArray(bigEndian: true), patch.ToByteArray()));
        }

        private static Task ReceiveTwinPushAsync(MockFeatureConnectionClient connection, Guid nonce, ulong desiredVersion, string desiredPayload)
        {
            var push = new TwinPush()
            {
                Desired = new Section { Version = desiredVersion, Payload = ByteString.CopyFromUtf8(desiredPayload) },
            };

            return connection.SimulateReceiveAsync(CreateInboundTwinPublish("twin-push:1", nonce.ToByteArray(bigEndian: true), push.ToByteArray()));
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
    }
}
