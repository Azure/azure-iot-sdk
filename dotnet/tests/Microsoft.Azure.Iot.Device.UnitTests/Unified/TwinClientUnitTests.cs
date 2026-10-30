// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Exceptions;
using Microsoft.Azure.Iot.Device.Models.Twin;
using Microsoft.Azure.Iot.Device.Mqtt;
using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Unified.Connection;
using System.Text;
using System.Text.Json.Nodes;
using Xunit;
using TwinClient = Microsoft.Azure.Iot.Device.Unified.Twin.TwinClient;

namespace Microsoft.Azure.Iot.Device.UnitTests.Unified
{
    public class TwinClientUnitTests
    {
        private const string DeviceId = "someDeviceId";
        private const string ClassicGetTopicPrefix = "$iothub/twin/GET/?$rid=";
        private const string ClassicReportedPatchTopicPrefix = "$iothub/twin/PATCH/properties/reported/?$rid=";

        [Fact]
        public async Task GetTwinAsync_Classic_PublishesGetAndReturnsResponse()
        {
            MockFeatureConnectionClient connection = CreateClassicConnection();
            using TwinClient twinClient = new(connection);

            Task<DeviceTwin> getTask = twinClient.GetTwinAsync(TestContext.Current.CancellationToken);

            MqttPublish getPublish = Assert.Single(connection.PublishedMessages);
            Assert.StartsWith(ClassicGetTopicPrefix, getPublish.Topic);
            Assert.Equal(MqttQualityOfServiceLevel.AtMostOnce, getPublish.QualityOfServiceLevel);
            await RespondClassicAsync(connection, getPublish, 200, payload: ClassicTwin(5, "{\"fanSpeed\":10}", 7, "{\"temperature\":21}"));

            DeviceTwin twin = await getTask;
            Assert.Equal(5ul, twin.DesiredVersion);
            Assert.Equal(7ul, twin.ReportedVersion);
            Assert.Equal("{\"fanSpeed\":10}", twin.Desired!.ToJsonString());
            Assert.Equal("{\"temperature\":21}", twin.Reported!.ToJsonString());
        }

        [Theory]
        [InlineData(429, ErrorRetryability.Retryable)]
        [InlineData(503, ErrorRetryability.Retryable)]
        [InlineData(404, ErrorRetryability.Terminal)]
        public async Task GetTwinAsync_Classic_ThrowsWhenIoTHubRejectsTheRequest(int status, ErrorRetryability expectedRetryability)
        {
            MockFeatureConnectionClient connection = CreateClassicConnection();
            using TwinClient twinClient = new(connection);

            Task<DeviceTwin> getTask = twinClient.GetTwinAsync(TestContext.Current.CancellationToken);
            await RespondClassicAsync(connection, Assert.Single(connection.PublishedMessages), status);

            DeviceException exception = await Assert.ThrowsAsync<DeviceException>(() => getTask);
            Assert.Equal(expectedRetryability, exception.Retryability);
        }

        [Fact]
        public async Task GetTwinAsync_Classic_ConnectionLossBeforeTheRequestIsSentDoesNotFailIt()
        {
            MockFeatureConnectionClient connection = CreateClassicConnection();
            TaskCompletionSource<MqttPublishAck> sent = new(TaskCreationOptions.RunContinuationsAsynchronously);
            connection.OnPublish = _ => sent.Task;
            using TwinClient twinClient = new(connection);

            // The connection client holds the request until it has reconnected.
            Task<DeviceTwin> getTask = twinClient.GetTwinAsync(TestContext.Current.CancellationToken);
            MqttPublish getPublish = Assert.Single(connection.PublishedMessages);
            connection.SimulateConnectionLost();
            Assert.False(getTask.IsCompleted);

            sent.SetResult(new MqttPublishAck() { ReasonCode = MqttPublishAckReasonCode.Success });
            await RespondClassicAsync(connection, getPublish, 200, payload: ClassicTwin(5, "{}", 7, "{}"));
            Assert.Equal(5ul, (await getTask).DesiredVersion);
        }

        [Fact]
        public async Task GetTwinAsync_Classic_ConnectionLossAfterTheRequestIsSentFailsItAsRetryable()
        {
            MockFeatureConnectionClient connection = CreateClassicConnection();
            using TwinClient twinClient = new(connection);

            Task<DeviceTwin> getTask = twinClient.GetTwinAsync(TestContext.Current.CancellationToken);
            Assert.Single(connection.PublishedMessages);
            connection.SimulateConnectionLost();

            DeviceException exception = await Assert.ThrowsAsync<DeviceException>(() => getTask);
            Assert.Equal(ErrorRetryability.Retryable, exception.Retryability);
        }

        [Fact]
        public async Task ConnectSync_Classic_GetsTwinAndRaisesTwinPushBeforeSendingHeldPatches()
        {
            MockFeatureConnectionClient connection = CreateClassicConnection();
            using TwinClient twinClient = new(connection);
            List<TwinPushReceivedEventArgs> pushes = new();
            twinClient.TwinPushReceived += pushes.Add;

            // Patches made before the device connects are held and coalesced.
            twinClient.EnqueueReportedPropertiesUpdate(new JsonObject() { ["a"] = 1 });
            Task<ReportedPatchResponse> patchTask = twinClient.UpdateReportedPropertiesAsync(new JsonObject() { ["b"] = 2 }, TestContext.Current.CancellationToken);
            Assert.Empty(connection.PublishedMessages);

            connection.SimulateClassicDevicePresenceEstablished();

            // The patches wait for the connect sync to complete.
            MqttPublish getPublish = Assert.Single(connection.PublishedMessages);
            Assert.StartsWith(ClassicGetTopicPrefix, getPublish.Topic);

            await RespondClassicAsync(connection, getPublish, 200, payload: ClassicTwin(3, "{\"fanSpeed\":1}", 5, "{\"x\":1}"));

            TwinPushReceivedEventArgs push = Assert.Single(pushes);
            Assert.Equal(3ul, push.Desired!.PropertiesVersion);
            Assert.Equal("{\"fanSpeed\":1}", push.Desired.Properties.ToJsonString());
            Assert.Equal(5ul, push.Reported!.PropertiesVersion);
            Assert.Equal("{\"x\":1}", push.Reported.Properties.ToJsonString());

            Assert.Equal(2, connection.PublishedMessages.Count);
            MqttPublish patchPublish = connection.PublishedMessages[1];
            Assert.StartsWith(ClassicReportedPatchTopicPrefix, patchPublish.Topic);
            Assert.Equal("{\"a\":1,\"b\":2}", Encoding.UTF8.GetString(patchPublish.Payload));

            await RespondClassicAsync(connection, patchPublish, 204, version: 6);

            ReportedPatchResponse response = await patchTask;
            Assert.Equal(Result.Ok, response.Result);
            Assert.Equal(6ul, response.Version);
            TwinState state = twinClient.GetTwinState();
            Assert.Equal(6ul, state.ReportedVersion);
            Assert.Equal("{\"x\":1,\"a\":1,\"b\":2}", state.Reported.ToJsonString());
            Assert.Equal(3ul, state.DesiredVersion);
            Assert.Empty(state.PendingReportedPatches);
        }

        [Fact]
        public void ConnectSync_Classic_StartsWhenTheTwinClientIsCreatedAfterPresenceIsEstablished()
        {
            MockFeatureConnectionClient connection = CreateClassicConnection();
            connection.IsClassicDevicePresenceEstablished = true;

            using TwinClient twinClient = new(connection);

            Assert.StartsWith(ClassicGetTopicPrefix, Assert.Single(connection.PublishedMessages).Topic);
        }

        [Fact]
        public async Task ConnectSync_Classic_BuffersDesiredPatchesUntilTheTwinArrives()
        {
            MockFeatureConnectionClient connection = CreateClassicConnection();
            using TwinClient twinClient = new(connection);
            List<string> events = new();
            twinClient.TwinPushReceived += args => events.Add($"push:{args.Desired?.PropertiesVersion}");
            twinClient.DesiredPatchReceived += args => events.Add($"patch:{args.DesiredPropertiesVersion}");

            connection.SimulateClassicDevicePresenceEstablished();
            MqttPublish getPublish = Assert.Single(connection.PublishedMessages);

            await ReceiveClassicDesiredPatchAsync(connection, 4, "{\"fanSpeed\":4}");
            Assert.Empty(events);

            await RespondClassicAsync(connection, getPublish, 200, payload: ClassicTwin(3, "{\"fanSpeed\":3}", 1, "{}"));

            Assert.Equal(new[] { "push:3", "patch:4" }, events);
            Assert.Equal("{\"fanSpeed\":4}", twinClient.GetTwinState().Desired.ToJsonString());
        }

        [Fact]
        public async Task DesiredPatchReceived_Classic_RaisedForTheNextVersionWithVersionStripped()
        {
            MockFeatureConnectionClient connection = CreateClassicConnection();
            using TwinClient twinClient = new(connection);
            await CompleteConnectSyncAsync(connection, desiredVersion: 1);

            DesiredPatchReceivedEventArgs? received = null;
            twinClient.DesiredPatchReceived += (args) => received = args;

            await ReceiveClassicDesiredPatchAsync(connection, 2, "{\"fanSpeed\":40}");

            Assert.NotNull(received);
            Assert.Equal(2ul, received!.DesiredPropertiesVersion);
            Assert.Equal("{\"fanSpeed\":40}", received.DesiredProperties.ToJsonString());
        }

        [Fact]
        public async Task DesiredPatchReceived_Classic_GapStartsResync()
        {
            MockFeatureConnectionClient connection = CreateClassicConnection();
            using TwinClient twinClient = new(connection);
            await CompleteConnectSyncAsync(connection, desiredVersion: 1);
            List<TwinPushReceivedEventArgs> pushes = new();
            twinClient.TwinPushReceived += pushes.Add;
            int patchCount = 0;
            twinClient.DesiredPatchReceived += _ => patchCount++;
            int publishCount = connection.PublishedMessages.Count;

            await ReceiveClassicDesiredPatchAsync(connection, 3, "{\"fanSpeed\":3}");

            Assert.Equal(0, patchCount);
            MqttPublish resyncGet = Assert.Single(connection.PublishedMessages.Skip(publishCount));
            Assert.StartsWith(ClassicGetTopicPrefix, resyncGet.Topic);

            await RespondClassicAsync(connection, resyncGet, 200, payload: ClassicTwin(3, "{\"fanSpeed\":3,\"mode\":\"eco\"}", 1, "{}"));

            TwinPushReceivedEventArgs push = Assert.Single(pushes);
            Assert.Equal(3ul, push.Desired!.PropertiesVersion);
            Assert.Null(push.Reported);
            Assert.Equal(0, patchCount);
        }

        [Fact]
        public async Task UpdateReportedPropertiesAsync_Classic_ResentAfterTheNextConnectSyncWhenTheConnectionIsLost()
        {
            MockFeatureConnectionClient connection = CreateClassicConnection();
            using TwinClient twinClient = new(connection);
            await CompleteConnectSyncAsync(connection, reportedVersion: 2);

            Task<ReportedPatchResponse> patchTask = twinClient.UpdateReportedPropertiesAsync(new JsonObject() { ["a"] = 1 }, TestContext.Current.CancellationToken);
            MqttPublish firstAttempt = connection.PublishedMessages.Last();
            Assert.StartsWith(ClassicReportedPatchTopicPrefix, firstAttempt.Topic);

            connection.SimulateConnectionLost();
            int publishCount = connection.PublishedMessages.Count;
            twinClient.EnqueueReportedPropertiesUpdate(new JsonObject() { ["b"] = 2 });
            Assert.Equal(publishCount, connection.PublishedMessages.Count);

            // IoT hub applied the first attempt before the connection was lost.
            await CompleteConnectSyncAsync(connection, reportedVersion: 3, reportedPayload: "{\"a\":1}");

            MqttPublish secondAttempt = connection.PublishedMessages.Last();
            Assert.StartsWith(ClassicReportedPatchTopicPrefix, secondAttempt.Topic);
            Assert.NotEqual(firstAttempt.Topic, secondAttempt.Topic);
            Assert.Equal("{\"a\":1,\"b\":2}", Encoding.UTF8.GetString(secondAttempt.Payload));

            // A late response to the first attempt is dropped.
            await RespondClassicAsync(connection, firstAttempt, 204, version: 3);
            Assert.False(patchTask.IsCompleted);

            await RespondClassicAsync(connection, secondAttempt, 204, version: 4);
            Assert.Equal(4ul, (await patchTask).Version);
            TwinState state = twinClient.GetTwinState();
            Assert.Equal(4ul, state.ReportedVersion);
            Assert.Equal("{\"a\":1,\"b\":2}", state.Reported.ToJsonString());
        }

        [Theory]
        [InlineData(200, Result.Ok)]
        [InlineData(400, Result.PayloadInvalid)]
        [InlineData(413, Result.PayloadTooLarge)]
        [InlineData(429, Result.InternalError)]
        [InlineData(500, Result.InternalError)]
        public async Task UpdateReportedPropertiesAsync_Classic_MapsTheResponseStatus(int status, Result expectedResult)
        {
            MockFeatureConnectionClient connection = CreateClassicConnection();
            using TwinClient twinClient = new(connection);
            await CompleteConnectSyncAsync(connection);
            List<ReportedPatchCompletedEventArgs> completions = new();
            twinClient.ReportedPatchCompleted += completions.Add;

            Task<ReportedPatchResponse> patchTask = twinClient.UpdateReportedPropertiesAsync(new JsonObject() { ["a"] = 1 }, TestContext.Current.CancellationToken);
            await RespondClassicAsync(connection, connection.PublishedMessages.Last(), status, version: 2);

            Assert.Equal(expectedResult, (await patchTask).Result);
            Assert.Equal(expectedResult, Assert.Single(completions).Response!.Result);
            Assert.Empty(twinClient.GetTwinState().PendingReportedPatches);
        }

        [Fact]
        public async Task ConnectSync_Classic_DoesNotRaiseTwinPushWhenTheRestoredStateIsCurrent()
        {
            MockFeatureConnectionClient connection = CreateClassicConnection();
            TwinState initialState = new()
            {
                DesiredVersion = 5,
                Desired = new JsonObject() { ["fanSpeed"] = 5 },
                ReportedVersion = 7,
                Reported = new JsonObject() { ["x"] = 7 },
                PendingReportedPatches = [new JsonObject() { ["a"] = 1 }],
            };
            using TwinClient twinClient = new(connection, initialState);
            int pushCount = 0;
            twinClient.TwinPushReceived += _ => pushCount++;

            await CompleteConnectSyncAsync(connection, desiredVersion: 5, desiredPayload: "{\"fanSpeed\":5}", reportedVersion: 7, reportedPayload: "{\"x\":7}");

            Assert.Equal(0, pushCount);
            MqttPublish patchPublish = connection.PublishedMessages.Last();
            Assert.Equal("{\"a\":1}", Encoding.UTF8.GetString(patchPublish.Payload));
        }

        [Fact]
        public async Task ConnectSync_Classic_AdoptsIoTHubsTwinWhenTheRestoredStateIsNewer()
        {
            MockFeatureConnectionClient connection = CreateClassicConnection();
            TwinState initialState = new()
            {
                DesiredVersion = 9,
                Desired = new JsonObject() { ["fanSpeed"] = 9 },
                ReportedVersion = 9,
                Reported = new JsonObject() { ["x"] = 9 },
            };
            using TwinClient twinClient = new(connection, initialState);
            List<TwinPushReceivedEventArgs> pushes = new();
            twinClient.TwinPushReceived += pushes.Add;

            await CompleteConnectSyncAsync(connection, desiredVersion: 2, desiredPayload: "{\"fanSpeed\":2}", reportedVersion: 3, reportedPayload: "{\"x\":3}");

            TwinPushReceivedEventArgs push = Assert.Single(pushes);
            Assert.Equal(2ul, push.Desired!.PropertiesVersion);
            Assert.Equal(3ul, push.Reported!.PropertiesVersion);
            TwinState state = twinClient.GetTwinState();
            Assert.Equal(2ul, state.DesiredVersion);
            Assert.Equal("{\"fanSpeed\":2}", state.Desired.ToJsonString());
            Assert.Equal(3ul, state.ReportedVersion);
            Assert.Equal("{\"x\":3}", state.Reported.ToJsonString());
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
        public async Task GetTwinAsync_ThrowsAfterDisconnect()
        {
            using MockConnectionMqttClient mqttClient = new();
            using ConnectionClient connection = new(new() { MqttClient = mqttClient });
            await connection.ConnectToHubAsync(
                MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv3, DeviceId),
                TestContext.Current.CancellationToken);
            using TwinClient twinClient = new(connection);

            await connection.DisconnectAsync(TestContext.Current.CancellationToken);

            await Assert.ThrowsAsync<NotSupportedException>(
                async () => await twinClient.GetTwinAsync(TestContext.Current.CancellationToken));
        }

        [Fact]
        public async Task GetTwinAsync_ThrowsAfterDispose()
        {
            MockFeatureConnectionClient connection = CreateClassicConnection();
            TwinClient twinClient = new(connection);
            twinClient.Dispose();

            await Assert.ThrowsAsync<ObjectDisposedException>(
                async () => await twinClient.GetTwinAsync(TestContext.Current.CancellationToken));
            Assert.True(connection.IsDisposed);
        }

        // Like the unified connection client, the mock exposes a connection nonce even before the first connection to IoT hub,
        // so that reported patches are held until the connect sync completes.
        private static MockFeatureConnectionClient CreateClassicConnection()
        {
            return new MockFeatureConnectionClient()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv3, DeviceId),
                ConnectionNonce = Guid.NewGuid(),
            };
        }

        private static async Task CompleteConnectSyncAsync(
            MockFeatureConnectionClient connection,
            ulong desiredVersion = 1,
            string desiredPayload = "{}",
            ulong reportedVersion = 1,
            string reportedPayload = "{}")
        {
            int publishCount = connection.PublishedMessages.Count;
            connection.SimulateClassicDevicePresenceEstablished();
            MqttPublish getPublish = connection.PublishedMessages[publishCount];
            Assert.StartsWith(ClassicGetTopicPrefix, getPublish.Topic);
            await RespondClassicAsync(connection, getPublish, 200, payload: ClassicTwin(desiredVersion, desiredPayload, reportedVersion, reportedPayload));
        }

        private static string ClassicTwin(ulong desiredVersion, string desiredPayload, ulong reportedVersion, string reportedPayload)
        {
            JsonObject desired = JsonNode.Parse(desiredPayload)!.AsObject();
            desired["$version"] = desiredVersion;
            JsonObject reported = JsonNode.Parse(reportedPayload)!.AsObject();
            reported["$version"] = reportedVersion;
            return new JsonObject() { ["desired"] = desired, ["reported"] = reported }.ToJsonString();
        }

        private static Task RespondClassicAsync(MockFeatureConnectionClient connection, MqttPublish request, int status, string? payload = null, ulong? version = null)
        {
            string topic = $"$iothub/twin/res/{status}/?$rid={ExtractRid(request.Topic)}";
            if (version != null)
            {
                topic += $"&$version={version}";
            }

            return connection.SimulateReceiveAsync(new MqttPublish()
            {
                Topic = topic,
                Payload = payload == null ? Array.Empty<byte>() : Encoding.UTF8.GetBytes(payload),
                QualityOfServiceLevel = MqttQualityOfServiceLevel.AtMostOnce,
            });
        }

        private static Task ReceiveClassicDesiredPatchAsync(MockFeatureConnectionClient connection, ulong version, string payload)
        {
            JsonObject patch = JsonNode.Parse(payload)!.AsObject();
            patch["$version"] = version;
            return connection.SimulateReceiveAsync(new MqttPublish()
            {
                Topic = $"$iothub/twin/PATCH/properties/desired/?$version={version}",
                Payload = Encoding.UTF8.GetBytes(patch.ToJsonString()),
                QualityOfServiceLevel = MqttQualityOfServiceLevel.AtMostOnce,
            });
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
