// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Google.Protobuf;
using Microsoft.Azure.Iot.Device.Exceptions;
using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Models.Twin;
using Microsoft.Azure.Iot.Device.Mqtt;
using Microsoft.Azure.Iot.Device.Provisioning.Models;
using Microsoft.Azure.Iot.Device.Unified.Connection;
using Microsoft.Azure.Iot.Device.Unified.Twin;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using Xunit;

namespace Microsoft.Azure.Iot.Device.UnitTests.Unified
{
    public class TwinClientUnitTests
    {
        private const string DeviceId = "someDeviceId";
        private const string GetTopicPrefix = "$iothub/twin/GET/?$rid=";
        private const string PatchTopicPrefix = "$iothub/twin/PATCH/properties/reported/?$rid=";
        private const string Mqttv5OutgoingTwinTopic = "ih/" + IConnectionClient.DeviceIdPlaceholder + "/srv/twin";
        private const string Mqttv5IncomingTwinTopic = "ih/someDeviceId/dev/twin";

        [Fact]
        public async Task GetTwinAsync_PublishesGetAndReturnsResponseWithoutVersionKeys()
        {
            MockFeatureConnectionClient connection = CreateConnection();
            using TwinClient twinClient = new(connection);

            Task<DeviceTwin> getTask = twinClient.GetTwinAsync(cancellationToken: TestContext.Current.CancellationToken);
            MqttPublish getPublish = Assert.Single(connection.GetPublishedMessages());
            Assert.StartsWith(GetTopicPrefix, getPublish.Topic);
            await RespondToGetAsync(connection, getPublish, 5, "{\"fanSpeed\":10}", 7, "{\"temperature\":21}");

            DeviceTwin twin = await getTask;
            Assert.Equal(5ul, twin.DesiredVersion);
            Assert.Equal(7ul, twin.ReportedVersion);
            Assert.Equal(10, (int)twin.Desired!["fanSpeed"]!);
            Assert.Equal(21, (int)twin.Reported!["temperature"]!);
            Assert.False(twin.Desired.ContainsKey(TwinClient.VersionKey));
            Assert.False(twin.Reported.ContainsKey(TwinClient.VersionKey));

            // The application fetched the twin, so this device now holds it.
            TwinState state = twinClient.GetTwinState();
            Assert.Equal(5ul, state.DesiredVersion);
            Assert.Equal(7ul, state.ReportedVersion);
            Assert.Equal(5ul, twinClient.AuthoritativeDesiredPropertiesVersion);
            Assert.Equal(7ul, twinClient.AuthoritativeReportedPropertiesVersion);
        }

        [Fact]
        public async Task GetTwinAsync_FiltersUnrequestedSectionAndRejectsRequestWithoutSections()
        {
            MockFeatureConnectionClient connection = CreateConnection();
            using TwinClient twinClient = new(connection);

            await Assert.ThrowsAsync<ArgumentException>(() => twinClient.GetTwinAsync(getReported: false, getDesired: false, cancellationToken: TestContext.Current.CancellationToken));

            Task<DeviceTwin> getTask = twinClient.GetTwinAsync(getReported: false, cancellationToken: TestContext.Current.CancellationToken);
            await RespondToGetAsync(connection, Assert.Single(connection.GetPublishedMessages()), 5, "{\"fanSpeed\":10}", 7, "{\"temperature\":21}");

            DeviceTwin twin = await getTask;
            Assert.NotNull(twin.Desired);
            Assert.Null(twin.Reported);
            Assert.Equal(0ul, twinClient.GetTwinState().ReportedVersion);
        }

        [Fact]
        public async Task GetTwinAsync_IdenticalConcurrentCallsShareOneRequest()
        {
            MockFeatureConnectionClient connection = CreateConnection();
            using TwinClient twinClient = new(connection);
            using CancellationTokenSource cancelledCaller = new();

            Task<DeviceTwin> first = twinClient.GetTwinAsync(cancellationToken: TestContext.Current.CancellationToken);
            Task<DeviceTwin> cancelled = twinClient.GetTwinAsync(cancellationToken: cancelledCaller.Token);
            Task<DeviceTwin> second = twinClient.GetTwinAsync(cancellationToken: TestContext.Current.CancellationToken);
            Task<DeviceTwin> different = twinClient.GetTwinAsync(getReported: false, cancellationToken: TestContext.Current.CancellationToken);
            Assert.Equal(2, connection.GetPublishedMessages().Count);

            // One caller giving up doesn't abandon the request for the others.
            cancelledCaller.Cancel();
            await Assert.ThrowsAnyAsync<OperationCanceledException>(() => cancelled);

            await RespondToGetAsync(connection, connection.GetPublishedMessages()[0], 3, "{}", 1, "{}");
            Assert.Same(await first, await second);
            Assert.False(different.IsCompleted);

            await RespondToGetAsync(connection, connection.GetPublishedMessages()[1], 4, "{}", 1, "{}");
            Assert.Equal(4ul, (await different).DesiredVersion);
        }

        [Fact]
        public async Task GetTwinAsync_RetriesWithNewIdAfterTimeoutAndDropsStaleResponses()
        {
            MockFeatureConnectionClient connection = CreateConnection();
            using TwinClient twinClient = new(connection);
            twinClient.GetTwinTimeoutSchedule = attempt => attempt == 1 ? TimeSpan.FromMilliseconds(50) : Timeout.InfiniteTimeSpan;

            Task<DeviceTwin> getTask = twinClient.GetTwinAsync(cancellationToken: TestContext.Current.CancellationToken);
            await WaitUntilAsync(() => connection.GetPublishedMessages().Count == 2);

            MqttPublish firstAttempt = connection.GetPublishedMessages()[0];
            MqttPublish secondAttempt = connection.GetPublishedMessages()[1];
            Assert.NotEqual(GetRid(firstAttempt), GetRid(secondAttempt));

            await RespondToGetAsync(connection, firstAttempt, 1, "{}", 1, "{}");
            Assert.False(getTask.IsCompleted);
            await RespondToGetAsync(connection, secondAttempt, 2, "{}", 1, "{}");
            Assert.Equal(2ul, (await getTask).DesiredVersion);
        }

        [Theory]
        [InlineData(429)]
        [InlineData(503)]
        public async Task GetTwinAsync_TransientStatus_IsSentAgain(int status)
        {
            MockFeatureConnectionClient connection = CreateConnection();
            using TwinClient twinClient = new(connection);
            int scheduleCalls = 0;

            // Only the timer restarted by the transient response elapses.
            twinClient.GetTwinTimeoutSchedule = _ => Interlocked.Increment(ref scheduleCalls) == 2 ? TimeSpan.FromMilliseconds(50) : Timeout.InfiniteTimeSpan;

            Task<DeviceTwin> getTask = twinClient.GetTwinAsync(cancellationToken: TestContext.Current.CancellationToken);
            await RespondAsync(connection, connection.GetPublishedMessages()[0], status);
            Assert.False(getTask.IsCompleted);

            await WaitUntilAsync(() => connection.GetPublishedMessages().Count == 2);
            await RespondToGetAsync(connection, connection.GetPublishedMessages()[1], 2, "{}", 1, "{}");
            Assert.Equal(2ul, (await getTask).DesiredVersion);
        }

        [Fact]
        public async Task GetTwinAsync_RejectedStatus_FailsWithTerminalError()
        {
            MockFeatureConnectionClient connection = CreateConnection();
            using TwinClient twinClient = new(connection);

            Task<DeviceTwin> getTask = twinClient.GetTwinAsync(cancellationToken: TestContext.Current.CancellationToken);
            await RespondAsync(connection, connection.GetPublishedMessages()[0], 404);

            DeviceException exception = await Assert.ThrowsAsync<DeviceException>(() => getTask);
            Assert.Equal(ErrorRetryability.Terminal, exception.Retryability);
        }

        [Fact]
        public async Task GetTwinAsync_ConnectionLostFailsWithRetryableErrorAndStopsRetrying()
        {
            MockFeatureConnectionClient connection = CreateConnection();
            using TwinClient twinClient = new(connection);
            twinClient.GetTwinTimeoutSchedule = _ => TimeSpan.FromMilliseconds(100);

            Task<DeviceTwin> getTask = twinClient.GetTwinAsync(cancellationToken: TestContext.Current.CancellationToken);
            connection.SimulateConnectionLost();

            DeviceException exception = await Assert.ThrowsAsync<DeviceException>(() => getTask);
            Assert.Equal(ErrorRetryability.Retryable, exception.Retryability);

            // A late response is dropped, and the request is not sent again.
            await RespondToGetAsync(connection, connection.GetPublishedMessages()[0], 1, "{}", 1, "{}");
            await Task.Delay(300, TestContext.Current.CancellationToken);
            Assert.Single(connection.GetPublishedMessages());
        }

        [Fact]
        public async Task GetTwinAsync_ThrowsWhenNotConnected()
        {
            MockFeatureConnectionClient disconnected = new();
            using TwinClient disconnectedClient = new(disconnected);
            await Assert.ThrowsAsync<NotSupportedException>(() => disconnectedClient.GetTwinAsync(cancellationToken: TestContext.Current.CancellationToken));
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

            await Assert.ThrowsAsync<NotSupportedException>(() => twinClient.GetTwinAsync(cancellationToken: TestContext.Current.CancellationToken));
        }

        [Fact]
        public async Task Dispose_FailsPendingOperationsAndDisposesConnection()
        {
            MockFeatureConnectionClient connection = CreateConnection(isReady: false);
            TwinClient twinClient = new(connection);
            Task<DeviceTwin> getTask = twinClient.GetTwinAsync(cancellationToken: TestContext.Current.CancellationToken);
            Task<ReportedPatchResponse> patchTask = UpdateReportedAsync(twinClient, "{\"temperature\":1}");

            twinClient.Dispose();

            await Assert.ThrowsAsync<ObjectDisposedException>(() => getTask);
            await Assert.ThrowsAsync<ObjectDisposedException>(() => patchTask);
            Assert.True(connection.IsDisposed);
            await Assert.ThrowsAsync<ObjectDisposedException>(() => twinClient.GetTwinAsync(cancellationToken: TestContext.Current.CancellationToken));
            Assert.Throws<ObjectDisposedException>(() => twinClient.EnqueueReportedPropertiesUpdate(new JsonObject()));
        }

        [Fact]
        public async Task UpdateReportedPropertiesAsync_PublishesUnconditionalPatchAndReturnsResponse()
        {
            MockFeatureConnectionClient connection = CreateConnection();
            using TwinClient twinClient = new(connection);
            List<ReportedPatchCompletedEventArgs> completions = new();
            twinClient.ReportedPatchCompleted += completions.Add;

            Task<ReportedPatchResponse> patchTask = UpdateReportedAsync(twinClient, "{\"temperature\":25}");
            MqttPublish patchPublish = Assert.Single(connection.GetPublishedMessages());
            Assert.StartsWith(PatchTopicPrefix, patchPublish.Topic);
            Assert.Equal(25, (int)ParsePayload(patchPublish)["temperature"]!);

            await RespondAsync(connection, patchPublish, 204, 12);

            ReportedPatchResponse response = await patchTask;
            Assert.Equal(Result.Ok, response.Result);
            Assert.Equal(12ul, response.Version);
            Assert.Equal(12ul, twinClient.AuthoritativeReportedPropertiesVersion);
            Assert.Same(response, Assert.Single(completions).Response);
            Assert.Empty(twinClient.GetTwinState().PendingReportedPatches);
        }

        [Fact]
        public async Task UpdateReportedPropertiesAsync_CombinesPatchesMadeWhileOneIsInFlight()
        {
            MockFeatureConnectionClient connection = CreateConnection();
            using TwinClient twinClient = new(connection);

            Task<ReportedPatchResponse> first = UpdateReportedAsync(twinClient, "{\"a\":1}");
            Task<ReportedPatchResponse> second = UpdateReportedAsync(twinClient, "{\"b\":{\"c\":1}}");
            Task<ReportedPatchResponse> third = UpdateReportedAsync(twinClient, "{\"b\":{\"d\":2}}");
            Assert.Single(connection.GetPublishedMessages());

            await RespondAsync(connection, connection.GetPublishedMessages()[0], 204, 2);
            Assert.Equal(2ul, (await first).Version);

            MqttPublish combined = connection.GetPublishedMessages()[1];
            Assert.True(JsonNode.DeepEquals(JsonNode.Parse("{\"b\":{\"c\":1,\"d\":2}}"), ParsePayload(combined)));
            await RespondAsync(connection, combined, 204, 3);
            Assert.Same(await second, await third);
            Assert.Equal(2, connection.GetPublishedMessages().Count);
        }

        [Theory]
        [InlineData(400, Result.PayloadInvalid)]
        [InlineData(413, Result.PayloadTooLarge)]
        [InlineData(404, Result.InternalError)]
        public async Task UpdateReportedPropertiesAsync_RejectedStatus_IsMappedAndNextPatchIsSent(int status, Result expectedResult)
        {
            MockFeatureConnectionClient connection = CreateConnection();
            using TwinClient twinClient = new(connection);

            Task<ReportedPatchResponse> rejected = UpdateReportedAsync(twinClient, "{\"a\":1}");
            Task<ReportedPatchResponse> next = UpdateReportedAsync(twinClient, "{\"b\":2}");
            await RespondAsync(connection, connection.GetPublishedMessages()[0], status);

            Assert.Equal(expectedResult, (await rejected).Result);
            Assert.False(twinClient.GetTwinState().Reported.ContainsKey("a"));

            MqttPublish nextPublish = connection.GetPublishedMessages()[1];
            Assert.True(JsonNode.DeepEquals(JsonNode.Parse("{\"b\":2}"), ParsePayload(nextPublish)));
            await RespondAsync(connection, nextPublish, 204, 2);
            Assert.Equal(Result.Ok, (await next).Result);
        }

        [Theory]
        [InlineData(429)]
        [InlineData(500)]
        public async Task UpdateReportedPropertiesAsync_TransientStatus_IsSentAgainWithNewIdAndDropsStaleResponse(int status)
        {
            MockFeatureConnectionClient connection = CreateConnection();
            using TwinClient twinClient = new(connection);
            List<int> attempts = new();
            twinClient.ReportedPatchTimeoutSchedule = attempt =>
            {
                lock (attempts)
                {
                    attempts.Add(attempt);
                }

                // Only the timer restarted by the transient response elapses.
                return attempts.Count == 2 ? TimeSpan.FromMilliseconds(50) : Timeout.InfiniteTimeSpan;
            };

            Task<ReportedPatchResponse> patchTask = UpdateReportedAsync(twinClient, "{\"a\":1}");
            MqttPublish firstAttempt = connection.GetPublishedMessages()[0];
            await RespondAsync(connection, firstAttempt, status);
            await WaitUntilAsync(() => connection.GetPublishedMessages().Count == 2);

            MqttPublish secondAttempt = connection.GetPublishedMessages()[1];
            Assert.NotEqual(GetRid(firstAttempt), GetRid(secondAttempt));
            Assert.True(JsonNode.DeepEquals(ParsePayload(firstAttempt), ParsePayload(secondAttempt)));

            await RespondAsync(connection, firstAttempt, 204, 5);
            Assert.False(patchTask.IsCompleted);
            await RespondAsync(connection, secondAttempt, 204, 6);
            Assert.Equal(6ul, (await patchTask).Version);
            Assert.Equal(new[] { 1, 1, 2 }, attempts);
        }

        [Fact]
        public async Task UpdateReportedPropertiesAsync_IsHeldUntilConnectionReadyAndSentAgainAfterConnectionLost()
        {
            MockFeatureConnectionClient connection = CreateConnection(isReady: false);
            using TwinClient twinClient = new(connection);

            Task<ReportedPatchResponse> patchTask = UpdateReportedAsync(twinClient, "{\"a\":1}");
            Assert.Empty(connection.GetPublishedMessages());

            connection.SimulateConnectionReady();
            MqttPublish firstAttempt = Assert.Single(connection.GetPublishedMessages(), IsPatch);

            connection.SimulateConnectionLost();
            connection.SimulateConnectionReady();
            List<MqttPublish> patches = connection.GetPublishedMessages().Where(IsPatch).ToList();
            Assert.Equal(2, patches.Count);
            Assert.NotEqual(GetRid(firstAttempt), GetRid(patches[1]));

            await RespondAsync(connection, firstAttempt, 204, 2);
            Assert.False(patchTask.IsCompleted);
            await RespondAsync(connection, patches[1], 204, 2);
            Assert.Equal(Result.Ok, (await patchTask).Result);
        }

        [Fact]
        public async Task UpdateReportedPropertiesAsync_CancelledBeforeBeingSent_IsDiscarded()
        {
            MockFeatureConnectionClient connection = CreateConnection(isReady: false);
            using TwinClient twinClient = new(connection);
            using CancellationTokenSource cancellation = new();

            Task<ReportedPatchResponse> patchTask = twinClient.UpdateReportedPropertiesAsync(
                new ReportedPatchRequest() { ReportedProperties = new JsonObject() { ["a"] = 1 } },
                cancellation.Token);
            Assert.Single(twinClient.GetTwinState().PendingReportedPatches);

            cancellation.Cancel();
            await Assert.ThrowsAnyAsync<OperationCanceledException>(() => patchTask);
            Assert.Empty(twinClient.GetTwinState().PendingReportedPatches);

            connection.SimulateConnectionReady();
            Assert.DoesNotContain(connection.GetPublishedMessages(), IsPatch);
        }

        [Fact]
        public async Task ReportedPatchOk_SkippedVersion_MarksReportedPropertiesIncompleteAndGetsTwin()
        {
            MockFeatureConnectionClient connection = CreateConnection();
            using TwinClient twinClient = new(connection, new TwinState() { ReportedVersion = 3, Reported = new JsonObject() { ["a"] = 1 } });
            List<TwinPushReceivedEventArgs> pushes = new();
            twinClient.TwinPushReceived += pushes.Add;

            Task<ReportedPatchResponse> patchTask = UpdateReportedAsync(twinClient, "{\"b\":2}");
            await RespondAsync(connection, connection.GetPublishedMessages()[0], 204, 5);
            Assert.Equal(5ul, (await patchTask).Version);

            // Someone else changed the reported properties at version 4, so this device no longer holds them in full.
            Assert.Equal(0ul, twinClient.GetTwinState().ReportedVersion);
            MqttPublish getPublish = Assert.Single(connection.GetPublishedMessages(), IsGet);

            await RespondToGetAsync(connection, getPublish, 1, "{}", 5, "{\"a\":1,\"b\":2,\"c\":3}");
            TwinPushReceivedEventArgs push = Assert.Single(pushes);
            Assert.Equal(5ul, push.Reported!.PropertiesVersion);
            TwinState state = twinClient.GetTwinState();
            Assert.Equal(5ul, state.ReportedVersion);
            Assert.Equal(3, (int)state.Reported["c"]!);
        }

        [Fact]
        public async Task DesiredPatchReceived_RaisedInVersionOrderWithoutVersionKey()
        {
            MockFeatureConnectionClient connection = CreateConnection();
            using TwinClient twinClient = new(connection);
            List<DesiredPatchReceivedEventArgs> received = new();
            twinClient.DesiredPatchReceived += received.Add;

            await ReceiveDesiredPatchAsync(connection, 1, "{\"fanSpeed\":40}");
            await ReceiveDesiredPatchAsync(connection, 1, "{\"fanSpeed\":41}");

            DesiredPatchReceivedEventArgs patch = Assert.Single(received);
            Assert.Equal(1ul, patch.DesiredPropertiesVersion);
            Assert.Equal(40, (int)patch.DesiredProperties["fanSpeed"]!);
            Assert.False(patch.DesiredProperties.ContainsKey(TwinClient.VersionKey));
            Assert.Equal(1ul, twinClient.AuthoritativeDesiredPropertiesVersion);
            Assert.Equal(40, (int)twinClient.GetTwinState().Desired["fanSpeed"]!);
            Assert.Empty(connection.GetPublishedMessages());
        }

        [Fact]
        public async Task DesiredPatchGap_GetsTwinAndDeliversSnapshotThenBufferedPatches()
        {
            MockFeatureConnectionClient connection = CreateConnection();
            using TwinClient twinClient = new(connection);
            List<string> deliveries = new();
            twinClient.DesiredPatchReceived += args => deliveries.Add($"patch {args.DesiredPropertiesVersion}");
            twinClient.TwinPushReceived += args => deliveries.Add($"push {args.Desired?.PropertiesVersion}");

            await ReceiveDesiredPatchAsync(connection, 1, "{\"a\":1}");
            await ReceiveDesiredPatchAsync(connection, 3, "{\"a\":3}");
            MqttPublish getPublish = Assert.Single(connection.GetPublishedMessages(), IsGet);
            await ReceiveDesiredPatchAsync(connection, 4, "{\"a\":4}");
            Assert.Equal(new[] { "patch 1" }, deliveries);

            await RespondToGetAsync(connection, getPublish, 3, "{\"a\":3}", 1, "{}");

            Assert.Equal(new[] { "patch 1", "push 3", "patch 4" }, deliveries);
            Assert.Equal(4ul, twinClient.GetTwinState().DesiredVersion);
            Assert.Equal(4, (int)twinClient.GetTwinState().Desired["a"]!);
        }

        [Fact]
        public async Task DesiredResyncBufferOverflow_GetsTwinAgain()
        {
            MockFeatureConnectionClient connection = CreateConnection();
            using TwinClient twinClient = new(connection);
            List<string> deliveries = new();
            twinClient.DesiredPatchReceived += args => deliveries.Add($"patch {args.DesiredPropertiesVersion}");
            twinClient.TwinPushReceived += args => deliveries.Add($"push {args.Desired?.PropertiesVersion}");

            ulong version = 2;
            for (; version <= TwinClient.DesiredResyncBufferCapacity + 1; version++)
            {
                await ReceiveDesiredPatchAsync(connection, version, "{}");
            }

            MqttPublish firstGet = Assert.Single(connection.GetPublishedMessages(), IsGet);
            await ReceiveDesiredPatchAsync(connection, version, "{}");
            List<MqttPublish> gets = connection.GetPublishedMessages().Where(IsGet).ToList();
            Assert.Equal(2, gets.Count);

            // The response to the first GET is stale.
            await RespondToGetAsync(connection, firstGet, 3, "{}", 1, "{}");
            Assert.Empty(deliveries);

            await RespondToGetAsync(connection, gets[1], version, "{}", 1, "{}");
            Assert.Equal(new[] { $"push {version}" }, deliveries);
        }

        [Fact]
        public async Task ConnectionReady_GetsTwinAndRaisesTwinPushWithChangedSections()
        {
            MockFeatureConnectionClient connection = CreateConnection(isReady: false);
            using TwinClient twinClient = new(connection);
            List<TwinPushReceivedEventArgs> pushes = new();
            twinClient.TwinPushReceived += pushes.Add;

            connection.SimulateConnectionReady();
            await RespondToGetAsync(connection, Assert.Single(connection.GetPublishedMessages(), IsGet), 4, "{\"fanSpeed\":4}", 7, "{\"temperature\":21}");

            TwinPushReceivedEventArgs push = Assert.Single(pushes);
            Assert.Equal(4ul, push.Desired!.PropertiesVersion);
            Assert.Equal(4, (int)push.Desired.Properties["fanSpeed"]!);
            Assert.Equal(7ul, push.Reported!.PropertiesVersion);
            Assert.Equal(21, (int)push.Reported.Properties["temperature"]!);
            Assert.Equal(4ul, twinClient.AuthoritativeDesiredPropertiesVersion);
            Assert.Equal(7ul, twinClient.AuthoritativeReportedPropertiesVersion);

            // The authoritative versions are per connection, and an unchanged twin isn't raised again.
            connection.SimulateConnectionLost();
            Assert.Null(twinClient.AuthoritativeDesiredPropertiesVersion);
            connection.SimulateConnectionReady();
            await RespondToGetAsync(connection, connection.GetPublishedMessages().Where(IsGet).Last(), 4, "{\"fanSpeed\":4}", 7, "{\"temperature\":21}");
            Assert.Single(pushes);
            Assert.Equal(4ul, twinClient.AuthoritativeDesiredPropertiesVersion);

            // Only the section that changed while disconnected is raised.
            connection.SimulateConnectionLost();
            connection.SimulateConnectionReady();
            await RespondToGetAsync(connection, connection.GetPublishedMessages().Where(IsGet).Last(), 5, "{\"fanSpeed\":5}", 7, "{\"temperature\":21}");
            Assert.Equal(2, pushes.Count);
            Assert.Equal(5ul, pushes[1].Desired!.PropertiesVersion);
            Assert.Null(pushes[1].Reported);
        }

        [Fact]
        public async Task ConnectionReady_WithConnectTwinSnapshot_AppliesItWithoutGettingTwinThenSendsPendingPatches()
        {
            MockFeatureConnectionClient connection = CreateConnection(isReady: false);
            using TwinClient twinClient = new(connection);
            List<TwinPushReceivedEventArgs> pushes = new();
            twinClient.TwinPushReceived += pushes.Add;
            int pushesWhenPatchSent = -1;
            connection.OnPublish = publish =>
            {
                if (IsPatch(publish))
                {
                    pushesWhenPatchSent = pushes.Count;
                }

                return Task.FromResult(new MqttPublishAck() { ReasonCode = MqttPublishAckReasonCode.Success });
            };
            twinClient.EnqueueReportedPropertiesUpdate(new JsonObject() { ["temperature"] = 30 });

            connection.SimulateConnectionReady(connection.CreateConnectTwinSnapshot(4, "{\"fanSpeed\":4}", 7, "{\"temperature\":21}"));
            await WaitUntilAsync(() => connection.GetPublishedMessages().Any(IsPatch));

            Assert.DoesNotContain(connection.GetPublishedMessages(), IsGet);
            TwinPushReceivedEventArgs push = Assert.Single(pushes);
            Assert.Equal(4ul, push.Desired!.PropertiesVersion);
            Assert.Equal(4, (int)push.Desired.Properties["fanSpeed"]!);
            Assert.Equal(7ul, push.Reported!.PropertiesVersion);
            Assert.Equal(1, pushesWhenPatchSent);
            Assert.Equal(4ul, twinClient.AuthoritativeDesiredPropertiesVersion);
            Assert.Equal(7ul, twinClient.AuthoritativeReportedPropertiesVersion);
        }

        [Fact]
        public async Task ConnectionReady_WithConnectTwinSnapshotFromPreviousConnection_GetsTwin()
        {
            MockFeatureConnectionClient connection = CreateConnection(isReady: false);
            using TwinClient twinClient = new(connection);
            List<TwinPushReceivedEventArgs> pushes = new();
            twinClient.TwinPushReceived += pushes.Add;

            ConnectTwinSnapshot staleSnapshot = new(connection.HubConnectionId, new JsonObject(), 4, new JsonObject(), 7);
            connection.SimulateConnectionReady(staleSnapshot);

            Assert.Empty(pushes);
            await RespondToGetAsync(connection, Assert.Single(connection.GetPublishedMessages(), IsGet), 5, "{}", 7, "{}");
            Assert.Equal(5ul, Assert.Single(pushes).Desired!.PropertiesVersion);
        }

        [Fact]
        public void Constructor_WhenConnectionIsReadyWithConnectTwinSnapshot_AppliesIt()
        {
            MockFeatureConnectionClient connection = CreateConnection(isReady: false);
            connection.SimulateConnectionReady(connection.CreateConnectTwinSnapshot(4, "{\"fanSpeed\":4}", 7, "{}"));

            using TwinClient twinClient = new(connection);

            Assert.Equal(4ul, twinClient.GetTwinState().DesiredVersion);
            Assert.Equal(4ul, twinClient.AuthoritativeDesiredPropertiesVersion);
            Assert.Empty(connection.GetPublishedMessages());
        }

        /// <summary>
        /// IoT hub may send a desired properties patch after responding to the connection client's get twin request, but
        /// before this client handles the connection becoming ready. That patch is newer than the twin, so the twin must not
        /// undo it, while later patches still apply on top of it.
        /// </summary>
        [Fact]
        public async Task ConnectTwinSnapshot_OlderThanDesiredPatchReceivedBeforeReady_DoesNotUndoIt()
        {
            MockFeatureConnectionClient connection = CreateConnection(isReady: false);
            using TwinClient twinClient = new(connection, new TwinState()
            {
                DesiredVersion = 4,
                Desired = new JsonObject() { ["a"] = 4 },
                ReportedVersion = 2,
                Reported = new JsonObject() { ["r"] = 2 },
            });
            List<string> deliveries = new();
            twinClient.DesiredPatchReceived += args => deliveries.Add($"patch {args.DesiredPropertiesVersion}");
            twinClient.TwinPushReceived += args => deliveries.Add($"push {args.Desired?.PropertiesVersion}");

            ConnectTwinSnapshot snapshot = connection.CreateConnectTwinSnapshot(4, "{\"a\":4}", 2, "{\"r\":2}");
            await ReceiveDesiredPatchAsync(connection, 5, "{\"a\":5}");
            connection.SimulateConnectionReady(snapshot);
            await ReceiveDesiredPatchAsync(connection, 6, "{\"a\":6}");

            Assert.Equal(new[] { "patch 5", "patch 6" }, deliveries);
            TwinState state = twinClient.GetTwinState();
            Assert.Equal(6ul, state.DesiredVersion);
            Assert.Equal(6, (int)state.Desired["a"]!);
            Assert.DoesNotContain(connection.GetPublishedMessages(), IsGet);
        }

        [Fact]
        public async Task TwinState_PersistedAndRestored_SendsPendingPatchesAndOnlyRaisesChanges()
        {
            MockFeatureConnectionClient firstConnection = CreateConnection(isReady: false);
            TwinState? persisted = null;
            using (TwinClient firstClient = new(firstConnection))
            {
                firstClient.TwinStateChanged += state => persisted = JsonSerializer.Deserialize<TwinState>(JsonSerializer.Serialize(state));
                firstConnection.SimulateConnectionReady();
                await RespondToGetAsync(firstConnection, Assert.Single(firstConnection.GetPublishedMessages(), IsGet), 2, "{\"fanSpeed\":2}", 5, "{\"temperature\":20}");

                firstConnection.SimulateConnectionLost();
                firstClient.EnqueueReportedPropertiesUpdate(new JsonObject() { ["temperature"] = 30 });
            }

            Assert.NotNull(persisted);
            Assert.Equal(2ul, persisted!.DesiredVersion);
            Assert.Equal(5ul, persisted.ReportedVersion);
            Assert.Equal(30, (int)Assert.Single(persisted.PendingReportedPatches)["temperature"]!);

            MockFeatureConnectionClient secondConnection = CreateConnection(isReady: false);
            using TwinClient secondClient = new(secondConnection, persisted);
            List<TwinPushReceivedEventArgs> pushes = new();
            List<ReportedPatchCompletedEventArgs> completions = new();
            secondClient.TwinPushReceived += pushes.Add;
            secondClient.ReportedPatchCompleted += completions.Add;

            secondConnection.SimulateConnectionReady();
            MqttPublish patchPublish = Assert.Single(secondConnection.GetPublishedMessages(), IsPatch);
            Assert.Equal(30, (int)ParsePayload(patchPublish)["temperature"]!);
            await RespondAsync(secondConnection, patchPublish, 204, 6);
            await RespondToGetAsync(secondConnection, Assert.Single(secondConnection.GetPublishedMessages(), IsGet), 2, "{\"fanSpeed\":2}", 6, "{\"temperature\":30}");

            Assert.Empty(pushes);
            Assert.Equal(Result.Ok, Assert.Single(completions).Response!.Result);
            TwinState restored = secondClient.GetTwinState();
            Assert.Equal(6ul, restored.ReportedVersion);
            Assert.Equal(30, (int)restored.Reported["temperature"]!);
            Assert.Empty(restored.PendingReportedPatches);
        }

        [Fact]
        public async Task EnqueueReportedPropertiesUpdate_FromTwinStateChangedHandler_IsSentAfterSnapshotContainingIt()
        {
            MockFeatureConnectionClient connection = CreateConnection();
            using TwinClient twinClient = new(connection);
            List<TwinState> snapshots = new();
            int publishedDuringHandler = -1;
            twinClient.TwinStateChanged += state =>
            {
                snapshots.Add(state);
                if (snapshots.Count == 1)
                {
                    twinClient.EnqueueReportedPropertiesUpdate(new JsonObject() { ["applied"] = 1 });
                    publishedDuringHandler = connection.GetPublishedMessages().Count;
                }
            };

            await ReceiveDesiredPatchAsync(connection, 1, "{\"a\":1}");

            Assert.Equal(0, publishedDuringHandler);
            Assert.Equal(2, snapshots.Count);
            Assert.Single(snapshots[1].PendingReportedPatches);
            Assert.Single(connection.GetPublishedMessages(), IsPatch);
        }

        [Fact]
        public async Task Mqttv5Hub_ReconcilesWithBirthAckAndTwinPushAndSendsUnconditionalPatches()
        {
            Guid nonce = Guid.NewGuid();
            MockFeatureConnectionClient connection = CreateConnection(IotHubConnectionType.Mqttv5, isReady: false);
            connection.ConnectionNonce = nonce;
            using TwinClient twinClient = new(connection);
            List<TwinPushReceivedEventArgs> pushes = new();
            twinClient.TwinPushReceived += pushes.Add;
            Task<ReportedPatchResponse> first = UpdateReportedAsync(twinClient, "{\"temperature\":1}");

            // The patch waits for IoT hub to acknowledge this device's presence, and the twin isn't fetched.
            connection.SimulateConnectionReady();
            Assert.Empty(connection.GetPublishedMessages());
            await ReceiveBirthAckAsync(connection, nonce, desiredVersion: 4, reportedVersion: 7);
            MqttPublish firstPatch = Assert.Single(connection.GetPublishedMessages());
            Assert.Equal(Mqttv5OutgoingTwinTopic, firstPatch.Topic);
            Assert.Equal("reported-patch:1", GetMqttv5Type(firstPatch));
            Assert.Equal(0ul, ReportedPatch.Parser.ParseFrom(firstPatch.Payload).IfMatch);
            Assert.Equal(4ul, twinClient.AuthoritativeDesiredPropertiesVersion);
            Assert.Equal(7ul, twinClient.AuthoritativeReportedPropertiesVersion);

            // IoT hub pushes the twin sections that changed instead.
            await ReceiveTwinPushAsync(connection, nonce, 4, "{\"fanSpeed\":4}");
            Assert.Equal(4ul, Assert.Single(pushes).Desired!.PropertiesVersion);
            Assert.Equal(4ul, twinClient.GetTwinState().DesiredVersion);

            await RespondToMqttv5PatchAsync(connection, firstPatch, Result.Ok, 8);
            Assert.Equal(8ul, (await first).Version);

            // Patches stay unconditional once IoT hub's version of the reported properties is known.
            _ = UpdateReportedAsync(twinClient, "{\"temperature\":2}");
            Assert.Equal(0ul, ReportedPatch.Parser.ParseFrom(connection.GetPublishedMessages()[1].Payload).IfMatch);
            Assert.DoesNotContain(connection.GetPublishedMessages(), publish => GetMqttv5Type(publish) == "get:1");
        }

        [Fact]
        public async Task Mqttv5Hub_DesiredProbeAheadOfLocalVersion_ResyncsDesiredProperties()
        {
            Guid nonce = Guid.NewGuid();
            MockFeatureConnectionClient connection = CreateConnection(IotHubConnectionType.Mqttv5);
            connection.ConnectionNonce = nonce;
            using TwinClient twinClient = new(connection);
            await ReceiveBirthAckAsync(connection, nonce, desiredVersion: 5, reportedVersion: 1);
            await ReceiveTwinPushAsync(connection, nonce, 5, "{\"fanSpeed\":50}");
            List<TwinPushReceivedEventArgs> pushes = new();
            twinClient.TwinPushReceived += pushes.Add;

            // A probe for the version this device already holds is dropped.
            await ReceiveMqttv5DesiredProbeAsync(connection, nonce, 5);
            Assert.Empty(connection.GetPublishedMessages());

            await ReceiveMqttv5DesiredProbeAsync(connection, nonce, 7);
            MqttPublish getPublish = Assert.Single(connection.GetPublishedMessages());
            Assert.Equal("get:1", GetMqttv5Type(getPublish));
            Assert.Equal(Sections.Desired, TwinGet.Parser.ParseFrom(getPublish.Payload).Sections);

            var response = new TwinGetResponse() { DesiredVersion = 7, DesiredPayload = ByteString.CopyFromUtf8("{\"fanSpeed\":70}") };
            await connection.SimulateReceiveAsync(CreateMqttv5TwinPublish("get-response:1", getPublish.CorrelationData!, response.ToByteArray()));

            TwinPushReceivedEventArgs push = Assert.Single(pushes);
            Assert.Equal(7ul, push.Desired!.PropertiesVersion);
            Assert.Equal(70, (int)push.Desired.Properties["fanSpeed"]!);
            Assert.Equal(7ul, twinClient.GetTwinState().DesiredVersion);
        }

        [Fact]
        public async Task ReconnectingToMqttv5Hub_KeepsTwinStateAndSendsPendingPatchAgain()
        {
            MockFeatureConnectionClient connection = CreateConnection(isReady: false);
            using TwinClient twinClient = new(connection);
            List<TwinPushReceivedEventArgs> pushes = new();
            twinClient.TwinPushReceived += pushes.Add;
            connection.SimulateConnectionReady();
            await RespondToGetAsync(connection, Assert.Single(connection.GetPublishedMessages(), IsGet), 2, "{\"fanSpeed\":2}", 5, "{\"temperature\":20}");
            Assert.Single(pushes);

            Task<ReportedPatchResponse> patchTask = UpdateReportedAsync(twinClient, "{\"temperature\":30}");
            MqttPublish classicPatch = Assert.Single(connection.GetPublishedMessages(), IsPatch);

            // Re-provisioning moves the device (with the same identity) to an IoT hub that uses MQTTv5.
            connection.SimulateConnectionLost();
            Guid nonce = Guid.NewGuid();
            connection.CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv5, DeviceId);
            connection.ConnectionNonce = nonce;
            int publishedBeforeReconnecting = connection.GetPublishedMessages().Count;

            // The held versions are announced in the birth message, so IoT hub acknowledges them and pushes nothing.
            Assert.Equal(2ul, connection.ObservedDesiredPropertiesVersions.Last());
            Assert.Equal(5ul, connection.ObservedReportedPropertiesVersions.Last());
            await ReceiveBirthAckAsync(connection, nonce, desiredVersion: 2, reportedVersion: 5);
            connection.SimulateConnectionReady();

            MqttPublish mqttv5Patch = Assert.Single(connection.GetPublishedMessages().Skip(publishedBeforeReconnecting));
            Assert.Equal("reported-patch:1", GetMqttv5Type(mqttv5Patch));
            ReportedPatch patch = ReportedPatch.Parser.ParseFrom(mqttv5Patch.Payload);
            Assert.Equal(0ul, patch.IfMatch);
            Assert.Equal(30, (int)JsonNode.Parse(patch.Payload.ToStringUtf8())!["temperature"]!);

            // The response to the attempt sent over the lost connection is dropped.
            await RespondAsync(connection, classicPatch, 204, 6);
            Assert.False(patchTask.IsCompleted);
            await RespondToMqttv5PatchAsync(connection, mqttv5Patch, Result.Ok, 6);
            Assert.Equal(6ul, (await patchTask).Version);

            Assert.Single(pushes);
            TwinState state = twinClient.GetTwinState();
            Assert.Equal(2ul, state.DesiredVersion);
            Assert.Equal(2, (int)state.Desired["fanSpeed"]!);
            Assert.Equal(6ul, state.ReportedVersion);
            Assert.Equal(30, (int)state.Reported["temperature"]!);
            Assert.Empty(state.PendingReportedPatches);
        }

        private static MockFeatureConnectionClient CreateConnection(IotHubConnectionType connectionProfile = IotHubConnectionType.Mqttv3, bool isReady = true)
        {
            return new MockFeatureConnectionClient()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(connectionProfile, DeviceId),
                IsConnectionReady = isReady,
            };
        }

        private static async Task WaitUntilAsync(Func<bool> condition)
        {
            using CancellationTokenSource timeout = CancellationTokenSource.CreateLinkedTokenSource(TestContext.Current.CancellationToken);
            timeout.CancelAfter(TimeSpan.FromSeconds(10));
            while (!condition())
            {
                await Task.Delay(10, timeout.Token);
            }
        }

        private static Task<ReportedPatchResponse> UpdateReportedAsync(TwinClient twinClient, string patch)
        {
            return twinClient.UpdateReportedPropertiesAsync(
                new ReportedPatchRequest() { ReportedProperties = JsonNode.Parse(patch)!.AsObject() },
                TestContext.Current.CancellationToken);
        }

        private static bool IsGet(MqttPublish publish) => publish.Topic.StartsWith(GetTopicPrefix, StringComparison.Ordinal);

        private static bool IsPatch(MqttPublish publish) => publish.Topic.StartsWith(PatchTopicPrefix, StringComparison.Ordinal);

        private static JsonObject ParsePayload(MqttPublish publish) => JsonNode.Parse(publish.Payload)!.AsObject();

        private static string GetRid(MqttPublish request)
        {
            string rest = request.Topic.Substring(request.Topic.IndexOf("$rid=", StringComparison.Ordinal) + "$rid=".Length);
            int ampersand = rest.IndexOf('&');
            return ampersand >= 0 ? rest.Substring(0, ampersand) : rest;
        }

        private static Task RespondAsync(MockFeatureConnectionClient connection, MqttPublish request, int status, ulong? version = null, string? payload = null)
        {
            string topic = $"$iothub/twin/res/{status}/?$rid={GetRid(request)}" + (version == null ? "" : $"&$version={version}");
            return connection.SimulateReceiveAsync(new MqttPublish()
            {
                Topic = topic,
                Payload = payload == null ? Array.Empty<byte>() : Encoding.UTF8.GetBytes(payload),
                QualityOfServiceLevel = MqttQualityOfServiceLevel.AtMostOnce,
            });
        }

        private static Task RespondToGetAsync(MockFeatureConnectionClient connection, MqttPublish getPublish, ulong desiredVersion, string desired, ulong reportedVersion, string reported)
        {
            JsonObject desiredSection = JsonNode.Parse(desired)!.AsObject();
            desiredSection[TwinClient.VersionKey] = desiredVersion;
            JsonObject reportedSection = JsonNode.Parse(reported)!.AsObject();
            reportedSection[TwinClient.VersionKey] = reportedVersion;
            JsonObject twin = new() { ["desired"] = desiredSection, ["reported"] = reportedSection };
            return RespondAsync(connection, getPublish, 200, payload: twin.ToJsonString());
        }

        private static Task ReceiveDesiredPatchAsync(MockFeatureConnectionClient connection, ulong version, string patch)
        {
            JsonObject payload = JsonNode.Parse(patch)!.AsObject();
            payload[TwinClient.VersionKey] = version;
            return connection.SimulateReceiveAsync(new MqttPublish()
            {
                Topic = $"$iothub/twin/PATCH/properties/desired/?$version={version}",
                Payload = Encoding.UTF8.GetBytes(payload.ToJsonString()),
                QualityOfServiceLevel = MqttQualityOfServiceLevel.AtMostOnce,
            });
        }

        private static Task ReceiveBirthAckAsync(MockFeatureConnectionClient connection, Guid nonce, ulong desiredVersion, ulong reportedVersion)
        {
            var publish = new MqttPublish()
            {
                Topic = "ih/someDeviceId/dev/presence",
                Payload = new Microsoft.Azure.Iot.Device.MQTTv5.Connection.BirthAck() { DesiredVersion = desiredVersion, ReportedVersion = reportedVersion }.ToByteArray(),
                CorrelationData = nonce.ToByteArray(bigEndian: true),
                QualityOfServiceLevel = MqttQualityOfServiceLevel.AtMostOnce,
            };
            publish.UserProperties.Add(new("type", Encoding.UTF8.GetBytes("birth-ack:1")));
            return connection.SimulateReceiveAsync(publish);
        }

        private static Task ReceiveTwinPushAsync(MockFeatureConnectionClient connection, Guid nonce, ulong desiredVersion, string desiredPayload)
        {
            var push = new TwinPush()
            {
                Desired = new Section { Version = desiredVersion, Payload = ByteString.CopyFromUtf8(desiredPayload) },
            };

            return connection.SimulateReceiveAsync(CreateMqttv5TwinPublish("twin-push:1", nonce.ToByteArray(bigEndian: true), push.ToByteArray()));
        }

        private static Task ReceiveMqttv5DesiredProbeAsync(MockFeatureConnectionClient connection, Guid nonce, ulong version)
        {
            var probe = new DesiredPatch() { Version = version };
            return connection.SimulateReceiveAsync(CreateMqttv5TwinPublish("desired-patch:1", nonce.ToByteArray(bigEndian: true), probe.ToByteArray()));
        }

        private static Task RespondToMqttv5PatchAsync(MockFeatureConnectionClient connection, MqttPublish patchPublish, Result result, ulong version)
        {
            var response = new ReportedPatchResponse() { Result = result, Version = version };
            return connection.SimulateReceiveAsync(CreateMqttv5TwinPublish("reported-patch-response:1", patchPublish.CorrelationData!, response.ToByteArray()));
        }

        private static MqttPublish CreateMqttv5TwinPublish(string type, byte[] correlationData, byte[] payload)
        {
            var publish = new MqttPublish()
            {
                Topic = Mqttv5IncomingTwinTopic,
                Payload = payload,
                CorrelationData = correlationData,
                QualityOfServiceLevel = MqttQualityOfServiceLevel.AtMostOnce,
            };
            publish.UserProperties.Add(new("type", Encoding.UTF8.GetBytes(type)));
            return publish;
        }

        private static string? GetMqttv5Type(MqttPublish publish)
        {
            return publish.UserProperties.SingleOrDefault(p => p.Name == "type")?.ReadValueAsString();
        }
    }
}
