// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Google.Protobuf;
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

            MqttPublish getPublish = await connection.WaitForPublishedMessagesAsync(1);
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
            await RespondClassicAsync(connection, await connection.WaitForPublishedMessagesAsync(1), status);

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
            MqttPublish getPublish = await connection.WaitForPublishedMessagesAsync(1);
            connection.SimulateConnectionLost();
            Assert.False(getTask.IsCompleted);

            connection.ConnectionNonce = Guid.NewGuid();
            sent.SetResult(new MqttPublishAck() { ReasonCode = MqttPublishAckReasonCode.Success });
            await RespondClassicAsync(connection, getPublish, 200, payload: ClassicTwin(5, "{}", 7, "{}"));
            Assert.Equal(5ul, (await getTask).DesiredVersion);
        }

        [Fact]
        public async Task GetTwinAsync_Classic_ConnectionLossWhileTheRequestIsBeingSentFailsItAsRetryable()
        {
            MockFeatureConnectionClient connection = CreateClassicConnection();
            TaskCompletionSource<MqttPublishAck> sent = new(TaskCreationOptions.RunContinuationsAsynchronously);
            connection.OnPublish = _ => sent.Task;
            using TwinClient twinClient = new(connection);

            // The request goes out on a connection that is lost before the publish returns.
            Task<DeviceTwin> getTask = twinClient.GetTwinAsync(TestContext.Current.CancellationToken);
            await connection.WaitForPublishedMessagesAsync(1);
            connection.SimulateConnectionLost();
            sent.SetResult(new MqttPublishAck() { ReasonCode = MqttPublishAckReasonCode.Success });

            DeviceException exception = await Assert.ThrowsAsync<DeviceException>(() => getTask);
            Assert.Equal(ErrorRetryability.Retryable, exception.Retryability);
        }

        [Fact]
        public async Task GetTwinAsync_Classic_ConnectionLossAfterTheRequestIsSentFailsItAsRetryable()
        {
            MockFeatureConnectionClient connection = CreateClassicConnection();
            using TwinClient twinClient = new(connection);

            Task<DeviceTwin> getTask = twinClient.GetTwinAsync(TestContext.Current.CancellationToken);
            await connection.WaitForPublishedMessagesAsync(1);
            connection.SimulateConnectionLost();

            DeviceException exception = await Assert.ThrowsAsync<DeviceException>(() => getTask);
            Assert.Equal(ErrorRetryability.Retryable, exception.Retryability);
        }

        [Fact]
        public async Task GetTwinAsync_Classic_CancellingStopsWaitingWhileTheRequestIsHeldForReconnection()
        {
            MockFeatureConnectionClient connection = CreateClassicConnection();
            using ManualResetEventSlim reconnected = new();

            // Like the connection client while reconnecting, the publish blocks the thread that sends it.
            connection.OnPublish = _ =>
            {
                reconnected.Wait(TimeSpan.FromSeconds(10));
                return Task.FromResult(new MqttPublishAck() { ReasonCode = MqttPublishAckReasonCode.Success });
            };
            using TwinClient twinClient = new(connection);
            using CancellationTokenSource cancellation = new();

            Task<DeviceTwin> getTask = Task.Run(() => twinClient.GetTwinAsync(cancellation.Token), TestContext.Current.CancellationToken);
            await connection.WaitForPublishedMessagesAsync(1);
            cancellation.Cancel();

            try
            {
                await Assert.ThrowsAnyAsync<OperationCanceledException>(() => getTask.WaitAsync(TimeSpan.FromSeconds(5), TestContext.Current.CancellationToken));
            }
            finally
            {
                reconnected.Set();
            }
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
            MqttPublish getPublish = await connection.WaitForPublishedMessagesAsync(1);
            Assert.Single(connection.PublishedMessages);
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
        public async Task ConnectSync_Classic_StartsWhenTheTwinClientIsCreatedAfterPresenceIsEstablished()
        {
            MockFeatureConnectionClient connection = CreateClassicConnection();
            connection.IsClassicDevicePresenceEstablished = true;

            using TwinClient twinClient = new(connection);

            Assert.StartsWith(ClassicGetTopicPrefix, (await connection.WaitForPublishedMessagesAsync(1)).Topic);
        }

        [Fact]
        public async Task ConnectSync_Classic_SendsTheGetOffThePresenceThreadAndCancelsItWhenTheConnectionIsLost()
        {
            MockFeatureConnectionClient connection = CreateClassicConnection();
            using ManualResetEventSlim presenceHandlersReturned = new();
            TaskCompletionSource<bool> sentAfterPresenceHandlersReturned = new(TaskCreationOptions.RunContinuationsAsynchronously);
            TaskCompletionSource<MqttPublishAck> sent = new(TaskCreationOptions.RunContinuationsAsynchronously);
            connection.OnPublish = _ =>
            {
                // The connection client raises the presence event while holding the lock that reconnecting needs, so a GET that
                // waits for a reconnection on that thread would never be released.
                sentAfterPresenceHandlersReturned.TrySetResult(presenceHandlersReturned.Wait(TimeSpan.FromSeconds(5)));
                return sent.Task;
            };
            using TwinClient twinClient = new(connection);

            connection.SimulateClassicDevicePresenceEstablished();
            presenceHandlersReturned.Set();

            Assert.True(await sentAfterPresenceHandlersReturned.Task);
            await connection.WaitForPublishedMessagesAsync(1);
            CancellationToken getCancellation = connection.PublishCancellationTokens[0];
            Assert.False(getCancellation.IsCancellationRequested);

            connection.SimulateConnectionLost();

            Assert.True(getCancellation.IsCancellationRequested);
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
            MqttPublish getPublish = await connection.WaitForPublishedMessagesAsync(1);

            await ReceiveClassicDesiredPatchAsync(connection, 4, "{\"fanSpeed\":4}");
            Assert.Empty(events);

            await RespondClassicAsync(connection, getPublish, 200, payload: ClassicTwin(3, "{\"fanSpeed\":3}", 1, "{}"));

            Assert.Equal(new[] { "push:3", "patch:4" }, events);
            Assert.Equal("{\"fanSpeed\":4}", twinClient.GetTwinState().Desired.ToJsonString());
        }

        [Fact]
        public async Task ConnectSync_Classic_ThrowingHandlerDoesNotStopLaterDeliveriesOrHeldPatches()
        {
            MockFeatureConnectionClient connection = CreateClassicConnection();
            using TwinClient twinClient = new(connection);
            twinClient.TwinPushReceived += _ => throw new InvalidOperationException("Application bug");
            List<ulong> patchVersions = new();
            twinClient.DesiredPatchReceived += args => patchVersions.Add(args.DesiredPropertiesVersion);
            twinClient.EnqueueReportedPropertiesUpdate(new JsonObject() { ["a"] = 1 });

            connection.SimulateClassicDevicePresenceEstablished();
            MqttPublish getPublish = await connection.WaitForPublishedMessagesAsync(1);
            await ReceiveClassicDesiredPatchAsync(connection, 4, "{\"fanSpeed\":4}");
            await RespondClassicAsync(connection, getPublish, 200, payload: ClassicTwin(3, "{\"fanSpeed\":3}", 1, "{}"));

            Assert.Equal(new[] { 4ul }, patchVersions);
            Assert.StartsWith(ClassicReportedPatchTopicPrefix, connection.PublishedMessages.Last().Topic);
        }

        [Fact]
        public async Task ConnectSync_Classic_CoalescesPatchesMadeByItsHandlersWithHeldPatches()
        {
            MockFeatureConnectionClient connection = CreateClassicConnection();
            using TwinClient twinClient = new(connection);
            List<string> events = new();
            twinClient.TwinPushReceived += args =>
            {
                events.Add("push");
                twinClient.EnqueueReportedPropertiesUpdate(new JsonObject() { ["ack"] = args.Desired!.PropertiesVersion });
            };
            twinClient.ReportedPatchCompleted += _ => events.Add("completed");
            twinClient.EnqueueReportedPropertiesUpdate(new JsonObject() { ["a"] = 1 });

            // IoT hub responds to each reported patch before its publish returns.
            connection.OnPublish = async publish =>
            {
                if (publish.Topic.StartsWith(ClassicReportedPatchTopicPrefix, StringComparison.Ordinal))
                {
                    await RespondClassicAsync(connection, publish, 204, version: 6);
                }

                return new MqttPublishAck() { ReasonCode = MqttPublishAckReasonCode.Success };
            };

            await CompleteConnectSyncAsync(connection, desiredVersion: 3, reportedVersion: 5);

            MqttPublish patchPublish = Assert.Single(connection.PublishedMessages, publish => publish.Topic.StartsWith(ClassicReportedPatchTopicPrefix, StringComparison.Ordinal));
            Assert.Equal("{\"a\":1,\"ack\":3}", Encoding.UTF8.GetString(patchPublish.Payload));
            Assert.Equal(new[] { "push", "completed" }, events);
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
        public async Task UpdateReportedPropertiesAsync_Classic_CancellingAfterTheConnectionIsLostKeepsASentPatch()
        {
            MockFeatureConnectionClient connection = CreateClassicConnection();
            using TwinClient twinClient = new(connection);
            await CompleteConnectSyncAsync(connection);
            using CancellationTokenSource cancellation = new();

            Task<ReportedPatchResponse> patchTask = twinClient.UpdateReportedPropertiesAsync(new JsonObject() { ["a"] = 1 }, cancellation.Token);
            Assert.StartsWith(ClassicReportedPatchTopicPrefix, (await connection.WaitForPublishedMessagesAsync(2)).Topic);

            // IoT hub may have applied the patch before the connection was lost, so it must still be sent again.
            connection.SimulateConnectionLost();
            cancellation.Cancel();
            await Assert.ThrowsAnyAsync<OperationCanceledException>(() => patchTask);
            Assert.Single(twinClient.GetTwinState().PendingReportedPatches);

            await CompleteConnectSyncAsync(connection);
            MqttPublish resent = await connection.WaitForPublishedMessagesAsync(4);
            Assert.StartsWith(ClassicReportedPatchTopicPrefix, resent.Topic);
            Assert.Equal("{\"a\":1}", Encoding.UTF8.GetString(resent.Payload));
        }

        [Fact]
        public async Task UpdateReportedPropertiesAsync_Classic_DoesNotCombinePatchesBeyondTheSizeLimit()
        {
            MockFeatureConnectionClient connection = CreateClassicConnection();
            using TwinClient twinClient = new(connection);
            string large = new('x', 20 * 1024);
            twinClient.EnqueueReportedPropertiesUpdate(new JsonObject() { ["a"] = large });
            twinClient.EnqueueReportedPropertiesUpdate(new JsonObject() { ["b"] = large });

            await CompleteConnectSyncAsync(connection);

            MqttPublish first = await connection.WaitForPublishedMessagesAsync(2);
            Assert.Equal(new JsonObject() { ["a"] = large }.ToJsonString(), Encoding.UTF8.GetString(first.Payload));
            await RespondClassicAsync(connection, first, 204, version: 2);

            MqttPublish second = await connection.WaitForPublishedMessagesAsync(3);
            Assert.Equal(new JsonObject() { ["b"] = large }.ToJsonString(), Encoding.UTF8.GetString(second.Payload));
        }

        [Fact]
        public async Task UpdateReportedPropertiesAsync_Classic_VersionGapLeavesTheReportedPropertiesIncomplete()
        {
            MockFeatureConnectionClient connection = CreateClassicConnection();
            using TwinClient twinClient = new(connection);
            await CompleteConnectSyncAsync(connection, reportedVersion: 2);

            Task<ReportedPatchResponse> patchTask = twinClient.UpdateReportedPropertiesAsync(new JsonObject() { ["a"] = 1 }, TestContext.Current.CancellationToken);
            await RespondClassicAsync(connection, connection.PublishedMessages.Last(), 204, version: 4);

            Assert.Equal(Result.Ok, (await patchTask).Result);
            Assert.Equal(0ul, twinClient.GetTwinState().ReportedVersion);
        }

        [Fact]
        public async Task UpdateReportedPropertiesAsync_Classic_TransientPublishFailureKeepsThePatchPending()
        {
            MockFeatureConnectionClient connection = CreateClassicConnection();
            using TwinClient twinClient = new(connection);
            await CompleteConnectSyncAsync(connection);
            List<ReportedPatchCompletedEventArgs> completions = new();
            twinClient.ReportedPatchCompleted += completions.Add;

            // For example, the connection dropped while the QoS 0 request was being written.
            connection.OnPublish = _ => Task.FromException<MqttPublishAck>(new DeviceException("Transport failure") { Retryability = ErrorRetryability.Retryable, IsContained = true });
            Task<ReportedPatchResponse> patchTask = twinClient.UpdateReportedPropertiesAsync(new JsonObject() { ["a"] = 1 }, TestContext.Current.CancellationToken);

            Assert.False(patchTask.IsCompleted);
            Assert.Empty(completions);
            Assert.Single(twinClient.GetTwinState().PendingReportedPatches);

            connection.OnPublish = null;
            connection.SimulateConnectionLost();
            await CompleteConnectSyncAsync(connection);

            MqttPublish resent = connection.PublishedMessages.Last();
            Assert.StartsWith(ClassicReportedPatchTopicPrefix, resent.Topic);
            Assert.Equal("{\"a\":1}", Encoding.UTF8.GetString(resent.Payload));
            await RespondClassicAsync(connection, resent, 204, version: 2);
            Assert.Equal(Result.Ok, (await patchTask).Result);
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
        public async Task ConnectSync_Classic_AdoptsIoTHubsTwinWhenTheRestoredStateHasTheSameVersionsButDifferentProperties()
        {
            // For example, the device was re-provisioned to another hub, where its new twin happens to have the same versions.
            MockFeatureConnectionClient connection = CreateClassicConnection();
            TwinState initialState = new()
            {
                DesiredVersion = 2,
                Desired = new JsonObject() { ["fanSpeed"] = 9 },
                ReportedVersion = 2,
                Reported = new JsonObject() { ["x"] = 9 },
            };
            using TwinClient twinClient = new(connection, initialState);
            List<TwinPushReceivedEventArgs> pushes = new();
            twinClient.TwinPushReceived += pushes.Add;

            await CompleteConnectSyncAsync(connection, desiredVersion: 2, desiredPayload: "{\"fanSpeed\":2}", reportedVersion: 2, reportedPayload: "{\"x\":2}");

            TwinPushReceivedEventArgs push = Assert.Single(pushes);
            Assert.Equal("{\"fanSpeed\":2}", push.Desired!.Properties.ToJsonString());
            Assert.Equal("{\"x\":2}", push.Reported!.Properties.ToJsonString());
            TwinState state = twinClient.GetTwinState();
            Assert.Equal(2ul, state.DesiredVersion);
            Assert.Equal("{\"fanSpeed\":2}", state.Desired.ToJsonString());
            Assert.Equal(2ul, state.ReportedVersion);
            Assert.Equal("{\"x\":2}", state.Reported.ToJsonString());
        }

        [Fact]
        public async Task ConnectSync_Classic_AppliesPatchesReceivedBeforeTheResponseWhenTheRestoredVersionIsNewer()
        {
            MockFeatureConnectionClient connection = CreateClassicConnection();
            TwinState initialState = new()
            {
                DesiredVersion = 9,
                Desired = new JsonObject() { ["fanSpeed"] = 9 },
            };
            using TwinClient twinClient = new(connection, initialState);
            List<string> events = new();
            twinClient.TwinPushReceived += args => events.Add($"push:{args.Desired?.PropertiesVersion}");
            twinClient.DesiredPatchReceived += args => events.Add($"patch:{args.DesiredPropertiesVersion}");

            connection.SimulateClassicDevicePresenceEstablished();
            MqttPublish getPublish = await connection.WaitForPublishedMessagesAsync(1);

            // Patch 3 looks obsolete next to the restored version 9, until IoT hub's twin shows that version 9 isn't valid.
            await ReceiveClassicDesiredPatchAsync(connection, 3, "{\"mode\":\"eco\"}");
            await RespondClassicAsync(connection, getPublish, 200, payload: ClassicTwin(2, "{\"fanSpeed\":2}", 1, "{}"));

            Assert.Equal(new[] { "push:2", "patch:3" }, events);
            TwinState state = twinClient.GetTwinState();
            Assert.Equal(3ul, state.DesiredVersion);
            Assert.Equal("{\"fanSpeed\":2,\"mode\":\"eco\"}", state.Desired.ToJsonString());
        }

        [Fact]
        public async Task GetTwinAsync_MqttV5_DelegatesToMqttv5Path()
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv5, DeviceId),
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

                await connection.SimulateReceiveAsync(CreateInboundMqttv5TwinPublish("get-response:1", publish.CorrelationData!, response.ToByteArray()));
                return new MqttPublishAck() { ReasonCode = MqttPublishAckReasonCode.Success };
            };

            DeviceTwin twin = await twinClient.GetTwinAsync(TestContext.Current.CancellationToken);

            MqttPublish getPublish = Assert.Single(connection.PublishedMessages);
            // The device ID is left for the connection client to fill in when it actually sends the publish.
            Assert.Equal($"ih/{IConnectionClient.DeviceIdPlaceholder}/srv/twin", getPublish.Topic);
            Assert.Equal(1ul, twin.DesiredVersion);
            Assert.Equal(2ul, twin.ReportedVersion);
        }

        [Fact]
        public async Task UpdateReportedPropertiesAsync_MqttV5_SendsConditionalPatchOnceIoTHubAcknowledgesPresence()
        {
            Guid nonce = Guid.NewGuid();
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv5, DeviceId),
                ConnectionNonce = nonce,
            };
            using TwinClient twinClient = new(connection);

            Task<ReportedPatchResponse> patchTask = twinClient.UpdateReportedPropertiesAsync(new JsonObject() { ["temperature"] = 25 }, TestContext.Current.CancellationToken);
            Assert.Empty(connection.PublishedMessages);

            MqttPublish birthAck = new()
            {
                Topic = $"ih/{DeviceId}/dev/presence",
                Payload = new global::Microsoft.Azure.Iot.Device.MQTTv5.Connection.BirthAck() { DesiredVersion = 1, ReportedVersion = 2 }.ToByteArray(),
                CorrelationData = nonce.ToByteArray(bigEndian: true),
                QualityOfServiceLevel = MqttQualityOfServiceLevel.AtMostOnce,
            };
            birthAck.UserProperties.Add(new("type", Encoding.UTF8.GetBytes("birth-ack:1")));
            await connection.SimulateReceiveAsync(birthAck);

            MqttPublish patchPublish = Assert.Single(connection.PublishedMessages);
            ReportedPatch patch = ReportedPatch.Parser.ParseFrom(patchPublish.Payload);
            Assert.Equal(2ul, patch.IfMatch);
            Assert.Equal("{\"temperature\":25}", patch.Payload.ToStringUtf8());

            var response = new ReportedPatchResponse() { Result = Result.Ok, Version = 3 };
            await connection.SimulateReceiveAsync(CreateInboundMqttv5TwinPublish("reported-patch-response:1", patchPublish.CorrelationData!, response.ToByteArray()));
            Assert.Equal(3ul, (await patchTask).Version);
        }

        [Fact]
        public async Task TwinPushReceived_MqttV5_IsForwarded()
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv5, DeviceId),
            };
            using TwinClient twinClient = new(connection);
            TwinPushReceivedEventArgs? received = null;
            twinClient.TwinPushReceived += args => received = args;

            var push = new TwinPush()
            {
                Desired = new Section { Version = 4, Payload = ByteString.CopyFromUtf8("{\"fanSpeed\":40}") },
            };
            await connection.SimulateReceiveAsync(CreateInboundMqttv5TwinPublish("twin-push:1", Guid.NewGuid().ToByteArray(bigEndian: true), push.ToByteArray()));

            Assert.Equal(4ul, received!.Desired!.PropertiesVersion);
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

            await connection.DisconnectAsync(null, TestContext.Current.CancellationToken);

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
            MqttPublish getPublish = await connection.WaitForPublishedMessagesAsync(publishCount + 1);
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

        private static MqttPublish CreateInboundMqttv5TwinPublish(string type, byte[] correlationData, byte[] payload)
        {
            var publish = new MqttPublish()
            {
                Topic = $"ih/{DeviceId}/dev/twin",
                Payload = payload,
                CorrelationData = correlationData,
                QualityOfServiceLevel = MqttQualityOfServiceLevel.AtMostOnce,
            };
            publish.UserProperties.Add(new("type", Encoding.UTF8.GetBytes(type)));
            return publish;
        }
    }
}
