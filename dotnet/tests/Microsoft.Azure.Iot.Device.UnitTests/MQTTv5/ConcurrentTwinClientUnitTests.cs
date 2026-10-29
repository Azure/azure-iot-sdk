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
using System.Text.Json;
using System.Text.Json.Nodes;
using Xunit;

namespace Microsoft.Azure.Iot.Device.UnitTests.MQTTv5
{
    public class ConcurrentTwinClientUnitTests
    {
        private const string DeviceId = "someDeviceId";
        // The device ID is left for the connection client to fill in when it actually sends the publish.
        private const string OutgoingTwinTopic = "ih/" + IConnectionClient.DeviceIdPlaceholder + "/srv/twin";
        private const string IncomingTwinTopic = "ih/someDeviceId/dev/twin";

        [Fact]
        public async Task GetTwinAsync_PublishesGetRequestAndReturnsResponse()
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv5, DeviceId),
            };
            using ConcurrentTwinClient twinClient = new(connection);

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

        [Theory]
        [InlineData(true, false, Sections.Reported)]
        [InlineData(false, true, Sections.Desired)]
        [InlineData(true, true, Sections.Both)]
        public async Task GetTwinAsync_RequestsSelectedSections(
            bool getReported,
            bool getDesired,
            Sections expectedSections)
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv5, DeviceId),
            };
            using ConcurrentTwinClient twinClient = new(connection);

            connection.OnPublish += async (publish) =>
            {
                var response = new TwinGetResponse()
                {
                    DesiredVersion = 5,
                    ReportedVersion = 7,
                };
                if (getDesired)
                {
                    response.DesiredPayload = ByteString.CopyFromUtf8("{\"fanSpeed\":10}");
                }
                if (getReported)
                {
                    response.ReportedPayload = ByteString.CopyFromUtf8("{\"temperature\":21}");
                }

                await connection.SimulateReceiveAsync(CreateInboundTwinPublish("get-response:1", publish.CorrelationData!, response.ToByteArray()));
                return new MqttPublishAck() { ReasonCode = MqttPublishAckReasonCode.Success };
            };

            DeviceTwin twin = await twinClient.GetTwinAsync(
                getReported,
                getDesired,
                cancellationToken: TestContext.Current.CancellationToken);

            TwinGet request = TwinGet.Parser.ParseFrom(Assert.Single(connection.PublishedMessages).Payload);
            Assert.Equal(expectedSections, request.Sections);
            Assert.Equal(getDesired, twin.Desired is not null);
            Assert.Equal(getReported, twin.Reported is not null);
        }

        [Theory]
        [InlineData(true, false)]
        [InlineData(false, true)]
        public async Task GetTwinAsync_FiltersUnrequestedSectionReturnedByService(
            bool getReported,
            bool getDesired)
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv5, DeviceId),
            };
            using ConcurrentTwinClient twinClient = new(connection);

            connection.OnPublish += async publish =>
            {
                var response = new TwinGetResponse
                {
                    DesiredVersion = 5,
                    ReportedVersion = 7,
                    DesiredPayload = ByteString.CopyFromUtf8("{\"fanSpeed\":10}"),
                    ReportedPayload = ByteString.CopyFromUtf8("{\"temperature\":21}"),
                };

                await connection.SimulateReceiveAsync(
                    CreateInboundTwinPublish("get-response:1", publish.CorrelationData!, response.ToByteArray()));
                return new MqttPublishAck { ReasonCode = MqttPublishAckReasonCode.Success };
            };

            DeviceTwin twin = await twinClient.GetTwinAsync(
                getReported,
                getDesired,
                cancellationToken: TestContext.Current.CancellationToken);

            Assert.Equal(getDesired, twin.Desired is not null);
            Assert.Equal(getReported, twin.Reported is not null);
        }

        [Fact]
        public async Task GetTwinAsync_AllowsConditionalResponseWithoutPayloads()
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv5, DeviceId),
            };
            using ConcurrentTwinClient twinClient = new(connection);

            connection.OnPublish += async (publish) =>
            {
                var response = new TwinGetResponse()
                {
                    DesiredVersion = 5,
                    ReportedVersion = 7,
                };

                await connection.SimulateReceiveAsync(CreateInboundTwinPublish("get-response:1", publish.CorrelationData!, response.ToByteArray()));
                return new MqttPublishAck() { ReasonCode = MqttPublishAckReasonCode.Success };
            };

            DeviceTwin twin = await twinClient.GetTwinAsync(
                ifNotMatchReported: 7,
                ifNotMatchDesired: 5,
                cancellationToken: TestContext.Current.CancellationToken);

            Assert.Null(twin.Desired);
            Assert.Null(twin.Reported);
            Assert.Equal(5ul, twin.DesiredVersion);
            Assert.Equal(7ul, twin.ReportedVersion);
        }

        [Fact]
        public async Task GetTwinAsync_RejectsRequestWithoutSections()
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv5, DeviceId),
            };
            using ConcurrentTwinClient twinClient = new(connection);

            await Assert.ThrowsAsync<ArgumentException>(
                () => twinClient.GetTwinAsync(
                    getReported: false,
                    getDesired: false,
                    cancellationToken: TestContext.Current.CancellationToken));
        }

        [Fact]
        public async Task UpdateReportedPropertiesAsync_PublishesPatchAndReturnsResponse()
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv5, DeviceId),
            };
            using ConcurrentTwinClient twinClient = new(connection);

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
            };

            ReportedPatchResponse response = await twinClient.UpdateReportedPropertiesAsync(patch, TestContext.Current.CancellationToken);

            MqttPublish patchPublish = Assert.Single(connection.PublishedMessages);
            Assert.Equal(OutgoingTwinTopic, patchPublish.Topic);
            Assert.Equal("reported-patch:1", GetType(patchPublish));
            (ulong ifMatch, JsonObject sentPatch) = ParseReportedPatch(patchPublish);
            Assert.Equal(0ul, ifMatch);
            Assert.Equal(25, (int)sentPatch["temperature"]!);

            Assert.Equal(Result.Ok, response.Result);
            Assert.Equal(12ul, response.Version);
        }

        [Fact]
        public async Task DesiredPatchReceived_RaisedOnInboundDesiredPatch()
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv5, DeviceId),
            };
            using ConcurrentTwinClient twinClient = new(connection);

            DesiredPatchReceivedEventArgs? received = null;
            twinClient.DesiredPatchReceived += (args) => received = args;

            var desiredPatch = new DesiredPatch()
            {
                Version = 1,
                Payload = ByteString.CopyFromUtf8("{\"fanSpeed\":40}"),
            };

            await connection.SimulateReceiveAsync(CreateInboundTwinPublish("desired-patch:1", Guid.NewGuid().ToByteArray(), desiredPatch.ToByteArray()));

            Assert.NotNull(received);
            Assert.Equal(1ul, received!.DesiredPropertiesVersion);
            Assert.Equal(40, (int)received.DesiredProperties["fanSpeed"]!);
        }

        [Fact]
        public async Task TwinPushReceived_MapsDesiredAndReportedSections()
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv5, DeviceId),
            };
            using ConcurrentTwinClient twinClient = new(connection);

            TwinPushReceivedEventArgs? received = null;
            twinClient.TwinPushReceived += args => received = args;

            var push = new TwinPush()
            {
                Desired = new Section
                {
                    Version = 3,
                    Payload = ByteString.CopyFromUtf8("{\"fanSpeed\":40}"),
                },
                Reported = new Section
                {
                    Version = 4,
                    Payload = ByteString.CopyFromUtf8("{\"temperature\":21}"),
                },
            };

            await connection.SimulateReceiveAsync(
                CreateInboundTwinPublish("twin-push:1", Guid.NewGuid().ToByteArray(), push.ToByteArray()));

            Assert.NotNull(received);
            Assert.Equal(3ul, received!.Desired!.PropertiesVersion);
            Assert.Equal(40, (int)received.Desired.Properties["fanSpeed"]!);
            Assert.Equal(4ul, received.Reported!.PropertiesVersion);
            Assert.Equal(21, (int)received.Reported.Properties["temperature"]!);
        }

        [Fact]
        public async Task HandleReceivedMqttPublish_IgnoresUnrelatedTopics()
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv5, DeviceId),
            };
            using ConcurrentTwinClient twinClient = new(connection);

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
        public async Task GetTwinAsync_RetriesWithNewIdAfterTimeoutAndDropsStaleResponses()
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv5, DeviceId),
            };
            using ConcurrentTwinClient twinClient = new(connection);
            List<int> attempts = new();
            twinClient.GetTwinTimeoutSchedule = attempt =>
            {
                lock (attempts)
                {
                    attempts.Add(attempt);
                }

                return attempt == 1 ? TimeSpan.FromMilliseconds(50) : Timeout.InfiniteTimeSpan;
            };

            Task<DeviceTwin> getTask = twinClient.GetTwinAsync(getReported: false, cancellationToken: TestContext.Current.CancellationToken);
            await WaitUntilAsync(() => connection.PublishedMessages.Count == 2);

            MqttPublish firstAttempt = connection.PublishedMessages[0];
            MqttPublish secondAttempt = connection.PublishedMessages[1];
            Assert.NotEqual(firstAttempt.CorrelationData, secondAttempt.CorrelationData);
            Assert.Equal(TwinGet.Parser.ParseFrom(firstAttempt.Payload), TwinGet.Parser.ParseFrom(secondAttempt.Payload));

            // The response to the first attempt is stale, so only the response to the latest attempt completes the request.
            await ReceiveGetResponseAsync(connection, firstAttempt, 1, "{\"fanSpeed\":1}");
            Assert.False(getTask.IsCompleted);
            await ReceiveGetResponseAsync(connection, secondAttempt, 2, "{\"fanSpeed\":2}");

            DeviceTwin twin = await getTask;
            Assert.Equal(2ul, twin.DesiredVersion);
            Assert.Equal(new[] { 1, 2 }, attempts);
        }

        [Fact]
        public async Task GetTwinAsync_IdenticalConcurrentCallsShareOneRequest()
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv5, DeviceId),
            };
            using ConcurrentTwinClient twinClient = new(connection);
            using CancellationTokenSource cancelledCaller = new();

            Task<DeviceTwin> first = twinClient.GetTwinAsync(getReported: false, cancellationToken: TestContext.Current.CancellationToken);
            Task<DeviceTwin> cancelled = twinClient.GetTwinAsync(getReported: false, cancellationToken: cancelledCaller.Token);
            Task<DeviceTwin> second = twinClient.GetTwinAsync(getReported: false, cancellationToken: TestContext.Current.CancellationToken);
            Task<DeviceTwin> different = twinClient.GetTwinAsync(getReported: false, ifNotMatchDesired: 1, cancellationToken: TestContext.Current.CancellationToken);

            Assert.Equal(2, connection.PublishedMessages.Count);

            // One caller giving up doesn't abandon the request for the others.
            cancelledCaller.Cancel();
            await Assert.ThrowsAnyAsync<OperationCanceledException>(() => cancelled);

            await ReceiveGetResponseAsync(connection, connection.PublishedMessages[0], 3, "{\"fanSpeed\":3}");
            Assert.Equal(3ul, (await first).DesiredVersion);
            Assert.Same(await first, await second);
            Assert.False(different.IsCompleted);

            await ReceiveGetResponseAsync(connection, connection.PublishedMessages[1], 4, "{\"fanSpeed\":4}");
            Assert.Equal(4ul, (await different).DesiredVersion);

            // Once the shared request completes, a new call sends a new request.
            Task<DeviceTwin> later = twinClient.GetTwinAsync(getReported: false, cancellationToken: TestContext.Current.CancellationToken);
            Assert.Equal(3, connection.PublishedMessages.Count);
            await ReceiveGetResponseAsync(connection, connection.PublishedMessages[2], 5, "{\"fanSpeed\":5}");
            Assert.Equal(5ul, (await later).DesiredVersion);
        }

        [Fact]
        public async Task GetTwinAsync_StopsRetryingOnceNoCallerIsWaiting()
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv5, DeviceId),
            };
            using ConcurrentTwinClient twinClient = new(connection);
            twinClient.GetTwinTimeoutSchedule = _ => TimeSpan.FromMilliseconds(50);
            using CancellationTokenSource caller = new();

            Task<DeviceTwin> getTask = twinClient.GetTwinAsync(cancellationToken: caller.Token);
            caller.Cancel();
            await Assert.ThrowsAnyAsync<OperationCanceledException>(() => getTask);

            await Task.Delay(300, TestContext.Current.CancellationToken);
            Assert.Single(connection.PublishedMessages);
        }

        [Fact]
        public async Task GetTwinAsync_ConnectionLostFailsWithRetryableErrorAndStopsRetrying()
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv5, DeviceId),
            };
            using ConcurrentTwinClient twinClient = new(connection);
            twinClient.GetTwinTimeoutSchedule = _ => TimeSpan.FromMilliseconds(100);

            Task<DeviceTwin> getTask = twinClient.GetTwinAsync(cancellationToken: TestContext.Current.CancellationToken);
            connection.SimulateConnectionLost();

            DeviceException exception = await Assert.ThrowsAsync<DeviceException>(() => getTask);
            Assert.Equal(ErrorRetryability.Retryable, exception.Retryability);

            // A late response is dropped, and the request is not sent again.
            await ReceiveGetResponseAsync(connection, connection.PublishedMessages[0], 1, "{}");
            await Task.Delay(300, TestContext.Current.CancellationToken);
            Assert.Single(connection.PublishedMessages);

            // A new request after reconnecting is unaffected.
            Task<DeviceTwin> retry = twinClient.GetTwinAsync(cancellationToken: TestContext.Current.CancellationToken);
            await ReceiveGetResponseAsync(connection, connection.PublishedMessages[1], 2, "{}");
            Assert.Equal(2ul, (await retry).DesiredVersion);
        }

        [Fact]
        public async Task UpdateReportedPropertiesAsync_WaitsForBirthAckThenTracksReportedVersion()
        {
            (MockFeatureConnectionClient connection, Guid nonce) = CreateConnectionWithNonce();
            using ConcurrentTwinClient twinClient = new(connection);

            Task<ReportedPatchResponse> first = UpdateReportedAsync(twinClient, "{\"temperature\":1}");
            Assert.Empty(connection.PublishedMessages);

            await ReceiveBirthAckAsync(connection, nonce, desiredVersion: 1, reportedVersion: 7);
            Assert.Equal(7ul, ParseReportedPatch(Assert.Single(connection.PublishedMessages)).IfMatch);

            await RespondToReportedPatchAsync(connection, connection.PublishedMessages[0], Result.Ok, 8);
            Assert.Equal(8ul, (await first).Version);

            _ = UpdateReportedAsync(twinClient, "{\"temperature\":2}");
            Assert.Equal(8ul, ParseReportedPatch(connection.PublishedMessages[1]).IfMatch);
        }

        [Fact]
        public async Task UpdateReportedPropertiesAsync_IgnoresBirthAckThatConnectionClientTimedOut()
        {
            (MockFeatureConnectionClient connection, Guid nonce) = CreateConnectionWithNonce();
            connection.TimedOutBirthAckNonce = nonce;
            using ConcurrentTwinClient twinClient = new(connection);

            _ = UpdateReportedAsync(twinClient, "{\"temperature\":1}");
            await ReceiveBirthAckAsync(connection, nonce, desiredVersion: 1, reportedVersion: 7);

            Assert.Empty(connection.PublishedMessages);
            Assert.Null(twinClient.AuthoritativeReportedPropertiesVersion);
        }

        [Fact]
        public async Task UpdateReportedPropertiesAsync_CreatedAfterBirthAck_UsesConnectionsBirthAckVersion()
        {
            (MockFeatureConnectionClient connection, Guid nonce) = CreateConnectionWithNonce();
            connection.LatestBirthAck = new BirthAckVersions(nonce, 3, 7);
            using ConcurrentTwinClient twinClient = new(connection);

            _ = UpdateReportedAsync(twinClient, "{\"temperature\":1}");

            Assert.Equal(7ul, ParseReportedPatch(Assert.Single(connection.PublishedMessages)).IfMatch);
            Assert.Equal(7ul, twinClient.AuthoritativeReportedPropertiesVersion);
        }

        [Fact]
        public async Task UpdateReportedPropertiesAsync_CombinesPatchesMadeWhileOneIsInFlight()
        {
            (MockFeatureConnectionClient connection, Guid nonce) = CreateConnectionWithNonce();
            using ConcurrentTwinClient twinClient = new(connection);
            await ReceiveBirthAckAsync(connection, nonce, desiredVersion: 1, reportedVersion: 1);

            Task<ReportedPatchResponse> first = UpdateReportedAsync(twinClient, "{\"temperature\":1}");
            Task<ReportedPatchResponse> second = UpdateReportedAsync(twinClient, "{\"temperature\":2,\"fan\":{\"speed\":1}}");
            Task<ReportedPatchResponse> third = UpdateReportedAsync(twinClient, "{\"temperature\":null,\"fan\":{\"mode\":\"auto\"}}");
            Task<ReportedPatchResponse> fourth = UpdateReportedAsync(twinClient, "{\"fan\":null}");

            // Merging into a property that an earlier patch removed can't be expressed in the same merge patch.
            Task<ReportedPatchResponse> fifth = UpdateReportedAsync(twinClient, "{\"fan\":{\"speed\":3}}");
            Assert.Single(connection.PublishedMessages);

            await RespondToReportedPatchAsync(connection, connection.PublishedMessages[0], Result.Ok, 2);
            Assert.Equal(2ul, (await first).Version);

            (ulong ifMatch, JsonObject combined) = ParseReportedPatch(connection.PublishedMessages[1]);
            Assert.Equal(2ul, ifMatch);
            Assert.Equal("{\"temperature\":null,\"fan\":null}", combined.ToJsonString());

            await RespondToReportedPatchAsync(connection, connection.PublishedMessages[1], Result.Ok, 3);
            Assert.Equal(3ul, (await second).Version);
            Assert.Equal(3ul, (await third).Version);
            Assert.Equal(3ul, (await fourth).Version);
            Assert.False(fifth.IsCompleted);

            (ifMatch, JsonObject last) = ParseReportedPatch(connection.PublishedMessages[2]);
            Assert.Equal(3ul, ifMatch);
            Assert.Equal("{\"fan\":{\"speed\":3}}", last.ToJsonString());
        }

        [Fact]
        public async Task UpdateReportedPropertiesAsync_VersionMismatch_IsSentAgainAgainstHubVersion()
        {
            (MockFeatureConnectionClient connection, Guid nonce) = CreateConnectionWithNonce();
            using ConcurrentTwinClient twinClient = new(connection);
            await ReceiveBirthAckAsync(connection, nonce, desiredVersion: 1, reportedVersion: 2);

            Task<ReportedPatchResponse> patch = UpdateReportedAsync(twinClient, "{\"temperature\":1}");
            await RespondToReportedPatchAsync(connection, connection.PublishedMessages[0], Result.VersionMismatch, 4);

            MqttPublish retry = connection.PublishedMessages[1];
            Assert.NotEqual(connection.PublishedMessages[0].CorrelationData, retry.CorrelationData);
            Assert.Equal(4ul, ParseReportedPatch(retry).IfMatch);
            Assert.False(patch.IsCompleted);

            await RespondToReportedPatchAsync(connection, retry, Result.Ok, 5);
            Assert.Equal(Result.Ok, (await patch).Result);

            // A mismatch against IoT hub's own version would be rejected again, so it is returned.
            Task<ReportedPatchResponse> rejected = UpdateReportedAsync(twinClient, "{\"temperature\":2}");
            await RespondToReportedPatchAsync(connection, connection.PublishedMessages[2], Result.VersionMismatch, 5);
            Assert.Equal(Result.VersionMismatch, (await rejected).Result);
            Assert.Equal(3, connection.PublishedMessages.Count);
        }

        [Fact]
        public async Task UpdateReportedPropertiesAsync_RejectedPatch_IsReturnedAndNextPatchIsSent()
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv5, DeviceId),
            };
            using ConcurrentTwinClient twinClient = new(connection);

            Task<ReportedPatchResponse> tooLarge = UpdateReportedAsync(twinClient, "{\"temperature\":1}");
            Task<ReportedPatchResponse> next = UpdateReportedAsync(twinClient, "{\"temperature\":2}");

            await RespondToReportedPatchAsync(connection, connection.PublishedMessages[0], Result.PayloadTooLarge, 1);
            Assert.Equal(Result.PayloadTooLarge, (await tooLarge).Result);

            Assert.Equal(2, (int)ParseReportedPatch(connection.PublishedMessages[1]).Patch["temperature"]!);
            await RespondToReportedPatchAsync(connection, connection.PublishedMessages[1], Result.Ok, 2);
            Assert.Equal(Result.Ok, (await next).Result);
        }

        [Fact]
        public async Task UpdateReportedPropertiesAsync_TimesOut_IsSentAgainWithNewIdAndDropsStaleResponse()
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv5, DeviceId),
            };
            using ConcurrentTwinClient twinClient = new(connection);
            List<int> attempts = new();
            twinClient.ReportedPatchTimeoutSchedule = attempt =>
            {
                lock (attempts)
                {
                    attempts.Add(attempt);
                }

                return attempt == 1 ? TimeSpan.FromMilliseconds(50) : Timeout.InfiniteTimeSpan;
            };

            Task<ReportedPatchResponse> patch = UpdateReportedAsync(twinClient, "{\"temperature\":1}");
            await WaitUntilAsync(() => connection.PublishedMessages.Count == 2);

            MqttPublish firstAttempt = connection.PublishedMessages[0];
            MqttPublish secondAttempt = connection.PublishedMessages[1];
            Assert.NotEqual(firstAttempt.CorrelationData, secondAttempt.CorrelationData);
            Assert.Equal(ReportedPatch.Parser.ParseFrom(firstAttempt.Payload), ReportedPatch.Parser.ParseFrom(secondAttempt.Payload));

            await RespondToReportedPatchAsync(connection, firstAttempt, Result.Ok, 1);
            Assert.False(patch.IsCompleted);
            await RespondToReportedPatchAsync(connection, secondAttempt, Result.Ok, 2);

            Assert.Equal(2ul, (await patch).Version);
            Assert.Equal(new[] { 1, 2 }, attempts);
        }

        [Fact]
        public async Task UpdateReportedPropertiesAsync_ConnectionLost_IsSentAgainAfterNextBirthAck()
        {
            (MockFeatureConnectionClient connection, Guid nonce) = CreateConnectionWithNonce();
            using ConcurrentTwinClient twinClient = new(connection);
            await ReceiveBirthAckAsync(connection, nonce, desiredVersion: 1, reportedVersion: 2);

            Task<ReportedPatchResponse> patch = UpdateReportedAsync(twinClient, "{\"temperature\":1}");
            MqttPublish firstAttempt = Assert.Single(connection.PublishedMessages);

            connection.SimulateConnectionLost();
            Guid newNonce = Guid.NewGuid();
            connection.ConnectionNonce = newNonce;
            Assert.Single(connection.PublishedMessages);

            // IoT hub applied the first attempt before the connection was lost, so its version moved on.
            await ReceiveBirthAckAsync(connection, newNonce, desiredVersion: 1, reportedVersion: 3);
            MqttPublish secondAttempt = connection.PublishedMessages[1];
            Assert.Equal(3ul, ParseReportedPatch(secondAttempt).IfMatch);

            await RespondToReportedPatchAsync(connection, firstAttempt, Result.Ok, 3);
            Assert.False(patch.IsCompleted);

            await RespondToReportedPatchAsync(connection, secondAttempt, Result.Ok, 4);
            Assert.Equal(4ul, (await patch).Version);
        }

        [Fact]
        public async Task UpdateReportedPropertiesAsync_CancelledBeforeBeingSent_IsDiscarded()
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv5, DeviceId),
            };
            using ConcurrentTwinClient twinClient = new(connection);
            using CancellationTokenSource caller = new();

            Task<ReportedPatchResponse> first = UpdateReportedAsync(twinClient, "{\"temperature\":1}");
            Task<ReportedPatchResponse> cancelled = twinClient.UpdateReportedPropertiesAsync(
                new() { ReportedProperties = new JsonObject() { ["temperature"] = 2 } },
                caller.Token);
            caller.Cancel();
            await Assert.ThrowsAnyAsync<OperationCanceledException>(() => cancelled);

            await RespondToReportedPatchAsync(connection, connection.PublishedMessages[0], Result.Ok, 2);
            await first;
            Assert.Single(connection.PublishedMessages);
        }

        [Fact]
        public async Task UpdateReportedPropertiesAsync_Dispose_FailsPendingPatches()
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv5, DeviceId),
            };
            ConcurrentTwinClient twinClient = new(connection);

            Task<ReportedPatchResponse> inFlight = UpdateReportedAsync(twinClient, "{\"temperature\":1}");
            Task<ReportedPatchResponse> queued = UpdateReportedAsync(twinClient, "{\"temperature\":2}");
            twinClient.Dispose();

            await Assert.ThrowsAsync<ObjectDisposedException>(() => inFlight);
            await Assert.ThrowsAsync<ObjectDisposedException>(() => queued);
        }

        [Fact]
        public async Task GetTwinAsync_ConnectionClientConnectionLossFailsOutstandingRequest()
        {
            using MockConnectionMqttClient mqttClient = new() { AutoRespondToBirth = true };
            using ConnectionClient connection = new(new() { MqttClient = mqttClient });
            await connection.ConnectToHubAsync(
                MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv5, DeviceId),
                TestContext.Current.CancellationToken);
            using ConcurrentTwinClient twinClient = new(connection);

            Task<DeviceTwin> getTask = twinClient.GetTwinAsync(cancellationToken: TestContext.Current.CancellationToken);
            await mqttClient.SimulateServerDisconnectAsync(MqttDisconnectReason.UnspecifiedError);

            DeviceException exception = await Assert.ThrowsAsync<DeviceException>(() => getTask);
            Assert.Equal(ErrorRetryability.Retryable, exception.Retryability);
        }

        [Fact]
        public async Task GetTwinAsync_ThrowsWhenDisconnected()
        {
            MockFeatureConnectionClient connection = new();
            using ConcurrentTwinClient twinClient = new(connection);

            await Assert.ThrowsAsync<NotSupportedException>(
                async () => await twinClient.GetTwinAsync(cancellationToken: TestContext.Current.CancellationToken));
        }

        [Fact]
        public async Task GetTwinAsync_ThrowsAfterDisconnect()
        {
            using MockConnectionMqttClient mqttClient = new() { AutoRespondToBirth = true };
            using ConnectionClient connection = new(new() { MqttClient = mqttClient });
            await connection.ConnectToHubAsync(
                MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv5, DeviceId),
                TestContext.Current.CancellationToken);
            using ConcurrentTwinClient twinClient = new(connection);

            await connection.DisconnectAsync(TestContext.Current.CancellationToken);

            await Assert.ThrowsAsync<NotSupportedException>(
                async () => await twinClient.GetTwinAsync(cancellationToken: TestContext.Current.CancellationToken));
        }

        [Fact]
        public async Task GetTwinAsync_ThrowsAfterDispose()
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv5, DeviceId),
            };
            ConcurrentTwinClient twinClient = new(connection);
            twinClient.Dispose();

            await Assert.ThrowsAsync<ObjectDisposedException>(
                async () => await twinClient.GetTwinAsync(cancellationToken: TestContext.Current.CancellationToken));
        }

        [Theory]
        [InlineData(5ul)] // Matches this device's view
        [InlineData(4ul)] // Stale
        public async Task DesiredProbe_NotAheadOfLocalVersion_IsDropped(ulong probeVersion)
        {
            (MockFeatureConnectionClient connection, Guid nonce) = CreateConnectionWithNonce();
            using ConcurrentTwinClient twinClient = new(connection);
            await ReceiveTwinPushAsync(connection, nonce, 5, "{\"fanSpeed\":40}");

            int eventCount = 0;
            twinClient.DesiredPatchReceived += _ => eventCount++;
            twinClient.TwinPushReceived += _ => eventCount++;

            await ReceiveDesiredPatchAsync(connection, nonce, probeVersion, payload: null);

            Assert.Equal(0, eventCount);
            Assert.Empty(connection.PublishedMessages);
        }

        [Fact]
        public async Task DesiredProbe_AheadOfLocalVersion_ResyncsAndDeliversSnapshotThenBufferedPatches()
        {
            (MockFeatureConnectionClient connection, Guid nonce) = CreateConnectionWithNonce();
            using ConcurrentTwinClient twinClient = new(connection);
            await ReceiveTwinPushAsync(connection, nonce, 5, "{\"fanSpeed\":40}");

            List<object> received = new();
            twinClient.DesiredPatchReceived += args => received.Add(args);
            twinClient.TwinPushReceived += args => received.Add(args);

            await ReceiveDesiredPatchAsync(connection, nonce, 7, payload: null);

            MqttPublish getPublish = Assert.Single(connection.PublishedMessages);
            Assert.Equal(OutgoingTwinTopic, getPublish.Topic);
            Assert.Equal("get:1", GetType(getPublish));
            Assert.Equal(Sections.Desired, TwinGet.Parser.ParseFrom(getPublish.Payload).Sections);

            // While resyncing, patches and probes are buffered. This probe is covered by the patch, so it doesn't resync again.
            await ReceiveDesiredPatchAsync(connection, nonce, 8, "{\"fanSpeed\":80}");
            await ReceiveDesiredPatchAsync(connection, nonce, 8, payload: null);
            Assert.Empty(received);
            Assert.Single(connection.PublishedMessages);

            await ReceiveGetResponseAsync(connection, getPublish, 7, "{\"fanSpeed\":70}");

            Assert.Collection(
                received,
                snapshot =>
                {
                    var push = Assert.IsType<TwinPushReceivedEventArgs>(snapshot);
                    Assert.Equal(7ul, push.Desired!.PropertiesVersion);
                    Assert.Equal(70, (int)push.Desired.Properties["fanSpeed"]!);
                    Assert.Null(push.Reported);
                },
                patch =>
                {
                    var desiredPatch = Assert.IsType<DesiredPatchReceivedEventArgs>(patch);
                    Assert.Equal(8ul, desiredPatch.DesiredPropertiesVersion);
                    Assert.Equal(80, (int)desiredPatch.DesiredProperties["fanSpeed"]!);
                });

            // Back to idle, so the next patch is delivered right away.
            await ReceiveDesiredPatchAsync(connection, nonce, 9, "{\"fanSpeed\":90}");
            Assert.Equal(9ul, Assert.IsType<DesiredPatchReceivedEventArgs>(received.Last()).DesiredPropertiesVersion);
        }

        [Fact]
        public async Task DesiredPatchGap_ResyncsAndDrainsOnlyPatchesNewerThanSnapshot()
        {
            (MockFeatureConnectionClient connection, Guid nonce) = CreateConnectionWithNonce();
            using ConcurrentTwinClient twinClient = new(connection);
            await ReceiveTwinPushAsync(connection, nonce, 1, "{}");

            List<ulong> patchVersions = new();
            List<ulong> snapshotVersions = new();
            twinClient.DesiredPatchReceived += args => patchVersions.Add(args.DesiredPropertiesVersion);
            twinClient.TwinPushReceived += args => snapshotVersions.Add(args.Desired!.PropertiesVersion);

            await ReceiveDesiredPatchAsync(connection, nonce, 3, "{\"a\":3}");
            MqttPublish getPublish = Assert.Single(connection.PublishedMessages);
            await ReceiveDesiredPatchAsync(connection, nonce, 4, "{\"a\":4}");

            await ReceiveGetResponseAsync(connection, getPublish, 3, "{\"a\":3}");

            Assert.Equal(new[] { 3ul }, snapshotVersions);
            Assert.Equal(new[] { 4ul }, patchVersions);
        }

        [Fact]
        public async Task TwinPush_DuringResync_ResolvesResyncAndLaterGetResponseIsDropped()
        {
            (MockFeatureConnectionClient connection, Guid nonce) = CreateConnectionWithNonce();
            using ConcurrentTwinClient twinClient = new(connection);

            await ReceiveDesiredPatchAsync(connection, nonce, 4, payload: null);
            MqttPublish getPublish = Assert.Single(connection.PublishedMessages);
            await ReceiveDesiredPatchAsync(connection, nonce, 5, "{\"a\":5}");
            await ReceiveDesiredPatchAsync(connection, nonce, 6, "{\"a\":6}");

            List<object> received = new();
            twinClient.DesiredPatchReceived += args => received.Add(args);
            twinClient.TwinPushReceived += args => received.Add(args);

            await ReceiveTwinPushAsync(connection, nonce, 5, "{\"a\":5}");

            Assert.Collection(
                received,
                push => Assert.Equal(5ul, Assert.IsType<TwinPushReceivedEventArgs>(push).Desired!.PropertiesVersion),
                patch => Assert.Equal(6ul, Assert.IsType<DesiredPatchReceivedEventArgs>(patch).DesiredPropertiesVersion));

            await ReceiveGetResponseAsync(connection, getPublish, 4, "{\"a\":4}");
            Assert.Equal(2, received.Count);
        }

        [Fact]
        public async Task DesiredResyncBufferOverflow_RequestsDesiredPropertiesAgain()
        {
            (MockFeatureConnectionClient connection, Guid nonce) = CreateConnectionWithNonce();
            using ConcurrentTwinClient twinClient = new(connection);

            List<ulong> snapshotVersions = new();
            twinClient.TwinPushReceived += args => snapshotVersions.Add(args.Desired!.PropertiesVersion);

            await ReceiveDesiredPatchAsync(connection, nonce, 2, payload: null);
            MqttPublish firstGet = Assert.Single(connection.PublishedMessages);

            for (ulong version = 3; version < 3 + ConcurrentTwinClient.DesiredResyncBufferCapacity + 1; version++)
            {
                await ReceiveDesiredPatchAsync(connection, nonce, version, "{}");
            }

            Assert.Equal(2, connection.PublishedMessages.Count);
            MqttPublish secondGet = connection.PublishedMessages[1];
            Assert.Equal("get:1", GetType(secondGet));
            Assert.NotEqual(firstGet.CorrelationData, secondGet.CorrelationData);

            await ReceiveGetResponseAsync(connection, firstGet, 2, "{}");
            Assert.Empty(snapshotVersions);

            await ReceiveGetResponseAsync(connection, secondGet, 11, "{}");
            Assert.Equal(new[] { 11ul }, snapshotVersions);
        }

        [Fact]
        public async Task DesiredResyncGet_TimesOut_IsSentAgainWithNewCorrelationData()
        {
            (MockFeatureConnectionClient connection, Guid nonce) = CreateConnectionWithNonce();
            using ConcurrentTwinClient twinClient = new(connection);
            using SemaphoreSlim getPublished = new(0);
            connection.OnPublish = publish =>
            {
                getPublished.Release();
                return Task.FromResult(new MqttPublishAck() { ReasonCode = MqttPublishAckReasonCode.Success });
            };

            List<int> timedOutAttempts = new();
            twinClient.DesiredResyncTimeoutSchedule = attempt =>
            {
                timedOutAttempts.Add(attempt);
                return attempt == 1 ? TimeSpan.FromMilliseconds(10) : TimeSpan.FromHours(1);
            };

            List<ulong> snapshotVersions = new();
            twinClient.TwinPushReceived += args => snapshotVersions.Add(args.Desired!.PropertiesVersion);

            await ReceiveDesiredPatchAsync(connection, nonce, 3, payload: null);
            Assert.True(await getPublished.WaitAsync(TimeSpan.FromSeconds(10), TestContext.Current.CancellationToken));
            Assert.True(await getPublished.WaitAsync(TimeSpan.FromSeconds(10), TestContext.Current.CancellationToken));

            Assert.Equal(new[] { 1, 2 }, timedOutAttempts);
            Assert.NotEqual(connection.PublishedMessages[0].CorrelationData, connection.PublishedMessages[1].CorrelationData);

            // Only the response to the latest attempt is applied.
            await ReceiveGetResponseAsync(connection, connection.PublishedMessages[0], 3, "{}");
            await ReceiveGetResponseAsync(connection, connection.PublishedMessages[1], 3, "{}");
            Assert.Equal(new[] { 3ul }, snapshotVersions);
        }

        [Fact]
        public async Task DesiredResync_IsDiscardedOnNewConnection()
        {
            (MockFeatureConnectionClient connection, Guid nonce) = CreateConnectionWithNonce();
            using ConcurrentTwinClient twinClient = new(connection);

            bool eventRaised = false;
            twinClient.TwinPushReceived += _ => eventRaised = true;

            await ReceiveDesiredPatchAsync(connection, nonce, 3, payload: null);
            MqttPublish getPublish = Assert.Single(connection.PublishedMessages);

            connection.ConnectionNonce = Guid.NewGuid();
            await ReceiveGetResponseAsync(connection, getPublish, 3, "{}");

            Assert.False(eventRaised);
        }

        [Fact]
        public async Task DesiredResync_IsDiscardedWhenConnectionIsLost()
        {
            (MockFeatureConnectionClient connection, Guid nonce) = CreateConnectionWithNonce();
            using ConcurrentTwinClient twinClient = new(connection);

            bool eventRaised = false;
            twinClient.TwinPushReceived += _ => eventRaised = true;

            await ReceiveDesiredPatchAsync(connection, nonce, 3, payload: null);
            MqttPublish getPublish = Assert.Single(connection.PublishedMessages);

            // Even before the device reconnects (with the same nonce, as seen by this client), the resync is abandoned.
            connection.SimulateConnectionLost();
            await ReceiveGetResponseAsync(connection, getPublish, 3, "{}");

            Assert.False(eventRaised);
        }

        [Fact]
        public async Task DesiredPatchAndTwinPush_ForAnotherConnection_AreDropped()
        {
            (MockFeatureConnectionClient connection, _) = CreateConnectionWithNonce();
            using ConcurrentTwinClient twinClient = new(connection);

            bool eventRaised = false;
            twinClient.DesiredPatchReceived += _ => eventRaised = true;
            twinClient.TwinPushReceived += _ => eventRaised = true;

            Guid otherNonce = Guid.NewGuid();
            await ReceiveTwinPushAsync(connection, otherNonce, 1, "{}");
            await ReceiveDesiredPatchAsync(connection, otherNonce, 1, "{}");
            await ReceiveDesiredPatchAsync(connection, otherNonce, 5, payload: null);

            Assert.False(eventRaised);
            Assert.Empty(connection.PublishedMessages);
        }

        [Fact]
        public async Task TwinPush_WithStaleDesiredSection_DeliversOnlyReportedSection()
        {
            (MockFeatureConnectionClient connection, Guid nonce) = CreateConnectionWithNonce();
            using ConcurrentTwinClient twinClient = new(connection);
            await ReceiveTwinPushAsync(connection, nonce, 5, "{}");

            TwinPushReceivedEventArgs? received = null;
            twinClient.TwinPushReceived += args => received = args;

            var push = new TwinPush()
            {
                Desired = new Section { Version = 5, Payload = ByteString.CopyFromUtf8("{}") },
                Reported = new Section { Version = 2, Payload = ByteString.CopyFromUtf8("{\"temperature\":21}") },
            };
            await connection.SimulateReceiveAsync(CreateInboundTwinPublish("twin-push:1", nonce.ToByteArray(bigEndian: true), push.ToByteArray()));

            Assert.NotNull(received);
            Assert.Null(received!.Desired);
            Assert.Equal(2ul, received.Reported!.PropertiesVersion);
        }

        [Fact]
        public async Task GetTwinAsync_WithDesiredPayload_AdvancesLocalDesiredVersion()
        {
            (MockFeatureConnectionClient connection, Guid nonce) = CreateConnectionWithNonce();
            using ConcurrentTwinClient twinClient = new(connection);
            connection.OnPublish = async publish =>
            {
                var response = new TwinGetResponse()
                {
                    DesiredVersion = 5,
                    DesiredPayload = ByteString.CopyFromUtf8("{}"),
                };
                await connection.SimulateReceiveAsync(CreateInboundTwinPublish("get-response:1", publish.CorrelationData!, response.ToByteArray()));
                return new MqttPublishAck() { ReasonCode = MqttPublishAckReasonCode.Success };
            };

            await twinClient.GetTwinAsync(getReported: false, cancellationToken: TestContext.Current.CancellationToken);
            await ReceiveDesiredPatchAsync(connection, nonce, 5, payload: null);

            // The probe matched the version the application fetched, so no resync GET was sent.
            Assert.Single(connection.PublishedMessages);
        }

        [Fact]
        public async Task InitialDesiredVersion_NextPatchIsDeliveredWithoutResync()
        {
            (MockFeatureConnectionClient connection, Guid nonce) = CreateConnectionWithNonce();
            connection.InitialDesiredPropertiesVersion = 5;
            using ConcurrentTwinClient twinClient = new(connection);

            DesiredPatchReceivedEventArgs? received = null;
            twinClient.DesiredPatchReceived += args => received = args;

            await ReceiveDesiredPatchAsync(connection, nonce, 5, payload: null);
            await ReceiveDesiredPatchAsync(connection, nonce, 6, "{\"fanSpeed\":60}");

            Assert.Empty(connection.PublishedMessages);
            Assert.Equal(6ul, received!.DesiredPropertiesVersion);
            Assert.Equal(new[] { 6ul }, connection.ObservedDesiredPropertiesVersions);
        }

        [Fact]
        public async Task ReportedPatchOk_RecordsReportedVersionOnlyWhenDeviceHeldPreviousVersion()
        {
            (MockFeatureConnectionClient connection, _) = CreateConnectionWithNonce();
            connection.InitialReportedPropertiesVersion = 3;
            using ConcurrentTwinClient twinClient = new(connection);
            await ReceiveBirthAckAsync(connection, connection.ConnectionNonce!.Value, desiredVersion: 1, reportedVersion: 3);

            ulong nextResponseVersion = 0;
            connection.OnPublish = async publish =>
            {
                var response = new ReportedPatchResponse() { Result = Result.Ok, Version = nextResponseVersion };
                await connection.SimulateReceiveAsync(CreateInboundTwinPublish("reported-patch-response:1", publish.CorrelationData!, response.ToByteArray()));
                return new MqttPublishAck() { ReasonCode = MqttPublishAckReasonCode.Success };
            };

            nextResponseVersion = 4;
            await twinClient.UpdateReportedPropertiesAsync(new() { ReportedProperties = new JsonObject() }, TestContext.Current.CancellationToken);

            // Version 5 was written by someone else, so this device doesn't hold the reported properties at version 6 in full.
            nextResponseVersion = 6;
            await twinClient.UpdateReportedPropertiesAsync(new() { ReportedProperties = new JsonObject() }, TestContext.Current.CancellationToken);

            Assert.Equal(new[] { 4ul }, connection.ObservedReportedPropertiesVersions);
        }

        [Fact]
        public async Task BirthAck_RevealsAuthoritativeVersionsForCurrentConnectionOnly()
        {
            (MockFeatureConnectionClient connection, Guid nonce) = CreateConnectionWithNonce();
            using ConcurrentTwinClient twinClient = new(connection);

            Assert.Null(twinClient.AuthoritativeDesiredPropertiesVersion);
            Assert.Null(twinClient.AuthoritativeReportedPropertiesVersion);

            await ReceiveBirthAckAsync(connection, Guid.NewGuid(), desiredVersion: 9, reportedVersion: 9);
            Assert.Null(twinClient.AuthoritativeDesiredPropertiesVersion);

            await ReceiveBirthAckAsync(connection, nonce, desiredVersion: 4, reportedVersion: 2);
            Assert.Equal(4ul, twinClient.AuthoritativeDesiredPropertiesVersion);
            Assert.Equal(2ul, twinClient.AuthoritativeReportedPropertiesVersion);

            connection.OnPublish = async publish =>
            {
                var response = new ReportedPatchResponse() { Result = Result.Ok, Version = 5 };
                await connection.SimulateReceiveAsync(CreateInboundTwinPublish("reported-patch-response:1", publish.CorrelationData!, response.ToByteArray()));
                return new MqttPublishAck() { ReasonCode = MqttPublishAckReasonCode.Success };
            };
            await twinClient.UpdateReportedPropertiesAsync(new() { ReportedProperties = new JsonObject() }, TestContext.Current.CancellationToken);
            await ReceiveDesiredPatchAsync(connection, nonce, 5, payload: null);

            Assert.Equal(5ul, twinClient.AuthoritativeDesiredPropertiesVersion);
            Assert.Equal(5ul, twinClient.AuthoritativeReportedPropertiesVersion);

            connection.ConnectionNonce = Guid.NewGuid();
            Assert.Null(twinClient.AuthoritativeDesiredPropertiesVersion);
            Assert.Null(twinClient.AuthoritativeReportedPropertiesVersion);
        }

        [Fact]
        public async Task BirthAck_OlderDesiredVersionThanHeld_DiscardsHeldVersionAndResyncs()
        {
            (MockFeatureConnectionClient connection, Guid nonce) = CreateConnectionWithNonce();
            connection.InitialDesiredPropertiesVersion = 10;
            using ConcurrentTwinClient twinClient = new(connection);

            TwinPushReceivedEventArgs? snapshot = null;
            twinClient.TwinPushReceived += args => snapshot = args;
            List<ulong> patches = new();
            twinClient.DesiredPatchReceived += args => patches.Add(args.DesiredPropertiesVersion);

            // IoT hub pushes its desired properties before the birth ack, but they look stale until the birth ack arrives.
            await ReceiveTwinPushAsync(connection, nonce, 5, "{\"fanSpeed\":50}");
            Assert.Null(snapshot);

            await ReceiveBirthAckAsync(connection, nonce, desiredVersion: 5, reportedVersion: 1);

            Assert.Equal(new[] { 0ul }, connection.ReplacedDesiredPropertiesVersions);
            MqttPublish get = Assert.Single(connection.PublishedMessages);
            Assert.Equal("get:1", GetType(get));

            await ReceiveGetResponseAsync(connection, get, 5, "{\"fanSpeed\":50}");
            await ReceiveDesiredPatchAsync(connection, nonce, 6, "{\"fanSpeed\":60}");

            Assert.Equal(5ul, snapshot!.Desired!.PropertiesVersion);
            Assert.Equal(new[] { 6ul }, patches);
        }

        [Fact]
        public async Task BirthAck_OlderReportedVersionThanHeld_DiscardsHeldVersion()
        {
            (MockFeatureConnectionClient connection, Guid nonce) = CreateConnectionWithNonce();
            connection.InitialReportedPropertiesVersion = 10;
            using ConcurrentTwinClient twinClient = new(connection);

            await ReceiveBirthAckAsync(connection, nonce, desiredVersion: 1, reportedVersion: 5);

            Assert.Equal(new[] { 0ul }, connection.ReplacedReportedPropertiesVersions);
            Assert.Empty(connection.PublishedMessages);
        }

        [Fact]
        public async Task TwinPush_OlderReportedVersionThanHeld_AdoptsPushedVersion()
        {
            (MockFeatureConnectionClient connection, Guid nonce) = CreateConnectionWithNonce();
            connection.InitialReportedPropertiesVersion = 10;
            using ConcurrentTwinClient twinClient = new(connection);

            var push = new TwinPush() { Reported = new Section { Version = 5, Payload = ByteString.CopyFromUtf8("{}") } };
            await connection.SimulateReceiveAsync(CreateInboundTwinPublish("twin-push:1", nonce.ToByteArray(bigEndian: true), push.ToByteArray()));
            await ReceiveBirthAckAsync(connection, nonce, desiredVersion: 1, reportedVersion: 5);

            Assert.Equal(new[] { 5ul }, connection.ReplacedReportedPropertiesVersions);
        }

        [Fact]
        public async Task UpdateReportedPropertiesAsync_BeforeConnecting_IsSentAfterBirthAck()
        {
            MockFeatureConnectionClient connection = new() { IsConnectionTargetSet = false };
            using ConcurrentTwinClient twinClient = new(connection);

            Task<ReportedPatchResponse> first = UpdateReportedAsync(twinClient, "{\"a\":1}");
            Task<ReportedPatchResponse> second = UpdateReportedAsync(twinClient, "{\"b\":2}");
            Assert.False(first.IsCompleted);
            Assert.Empty(connection.PublishedMessages);

            Guid nonce = Guid.NewGuid();
            connection.CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv5, DeviceId);
            connection.IsConnectionTargetSet = true;
            connection.ConnectionNonce = nonce;
            await ReceiveBirthAckAsync(connection, nonce, desiredVersion: 1, reportedVersion: 4);

            await WaitUntilAsync(() => connection.PublishedMessages.Count == 1);
            MqttPublish patchPublish = connection.PublishedMessages[0];
            (ulong ifMatch, JsonObject patch) = ParseReportedPatch(patchPublish);
            Assert.Equal(4ul, ifMatch);
            Assert.Equal("{\"a\":1,\"b\":2}", patch.ToJsonString());

            await RespondToReportedPatchAsync(connection, patchPublish, Result.Ok, 5);
            Assert.Equal(5ul, (await first).Version);
            Assert.Equal(5ul, (await second).Version);
        }

        [Fact]
        public async Task UpdateReportedPropertiesAsync_AfterDisconnect_IsSentOnceReconnected()
        {
            List<MqttPublish> patchPublishes = new();
            using MockConnectionMqttClient mqttClient = new()
            {
                AutoRespondToBirth = true,
                AutoBirthAck = new BirthAck() { DesiredVersion = 1, ReportedVersion = 4 },
            };
            mqttClient.OnPublish = publish =>
            {
                if (publish.UserProperties.Any(p => p.Name == "type" && p.ReadValueAsString() == "reported-patch:1"))
                {
                    lock (patchPublishes)
                    {
                        patchPublishes.Add(publish);
                    }
                }

                return Task.FromResult(new MqttPublishAck() { ReasonCode = MqttPublishAckReasonCode.Success });
            };
            using ConnectionClient connection = new(new() { MqttClient = mqttClient });
            using ConcurrentTwinClient twinClient = new(connection);
            ConnectionContext context = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv5, DeviceId);
            await connection.ConnectToHubAsync(context, TestContext.Current.CancellationToken);
            await connection.DisconnectAsync(TestContext.Current.CancellationToken);

            Task<ReportedPatchResponse> update = UpdateReportedAsync(twinClient, "{\"a\":1}");
            Assert.False(update.IsCompleted);
            lock (patchPublishes)
            {
                Assert.Empty(patchPublishes);
            }

            await connection.ConnectToHubAsync(context, TestContext.Current.CancellationToken);

            await WaitUntilAsync(() => { lock (patchPublishes) { return patchPublishes.Count == 1; } });
            MqttPublish patchPublish = patchPublishes[0];
            Assert.Equal(4ul, ParseReportedPatch(patchPublish).IfMatch);

            var response = new ReportedPatchResponse() { Result = Result.Ok, Version = 5 };
            await mqttClient.SimulatePublishReceivedAsync(CreateInboundTwinPublish("reported-patch-response:1", patchPublish.CorrelationData!, response.ToByteArray()));
            Assert.Equal(5ul, (await update).Version);
        }

        [Fact]
        public async Task Constructor_WithTwinState_RestoresVersionsAndSendsPendingPatchesAfterBirthAck()
        {
            (MockFeatureConnectionClient connection, Guid nonce) = CreateConnectionWithNonce();
            TwinState initialState = JsonSerializer.Deserialize<TwinState>(
                "{\"DesiredVersion\":3,\"Desired\":{\"d\":1},\"ReportedVersion\":4,\"Reported\":{\"a\":0,\"c\":3},\"PendingReportedPatches\":[{\"a\":1},{\"b\":2}]}")!;
            using ConcurrentTwinClient twinClient = new(connection, initialState);
            List<string> events = new();
            TwinState? latestState = null;
            twinClient.ReportedPatchCompleted += args => events.Add($"completed {args.Patch.ToJsonString()} {args.Response?.Result}");
            twinClient.TwinStateChanged += state =>
            {
                events.Add("state");
                latestState = state;
            };

            Assert.Equal([3ul], connection.ReplacedDesiredPropertiesVersions);
            Assert.Equal([4ul], connection.ReplacedReportedPropertiesVersions);
            Assert.Equal(2, twinClient.GetTwinState().PendingReportedPatches.Count);
            Assert.Empty(connection.PublishedMessages);

            await ReceiveBirthAckAsync(connection, nonce, desiredVersion: 3, reportedVersion: 4);
            await WaitUntilAsync(() => connection.PublishedMessages.Count == 1);
            MqttPublish patchPublish = connection.PublishedMessages[0];
            (ulong ifMatch, JsonObject patch) = ParseReportedPatch(patchPublish);
            Assert.Equal(4ul, ifMatch);
            Assert.Equal("{\"a\":1,\"b\":2}", patch.ToJsonString());

            await RespondToReportedPatchAsync(connection, patchPublish, Result.Ok, 5);

            Assert.Equal(["completed {\"a\":1,\"b\":2} Ok", "state"], events);
            Assert.NotNull(latestState);
            Assert.Equal(3ul, latestState.DesiredVersion);
            Assert.Equal("{\"d\":1}", latestState.Desired.ToJsonString());
            Assert.Equal(5ul, latestState.ReportedVersion);
            Assert.Equal("{\"a\":1,\"c\":3,\"b\":2}", latestState.Reported.ToJsonString());
            Assert.Empty(latestState.PendingReportedPatches);
        }

        [Fact]
        public async Task TwinState_TracksDesiredChangesAndOnlyAppliedReportedPatches()
        {
            (MockFeatureConnectionClient connection, Guid nonce) = CreateConnectionWithNonce();
            using ConcurrentTwinClient twinClient = new(connection);
            await ReceiveBirthAckAsync(connection, nonce, desiredVersion: 1, reportedVersion: 2);
            var push = new TwinPush()
            {
                Desired = new Section { Version = 1, Payload = ByteString.CopyFromUtf8("{\"x\":{\"y\":1,\"z\":2}}") },
                Reported = new Section { Version = 2, Payload = ByteString.CopyFromUtf8("{\"r\":1}") },
            };
            await connection.SimulateReceiveAsync(CreateInboundTwinPublish("twin-push:1", nonce.ToByteArray(bigEndian: true), push.ToByteArray()));
            await ReceiveDesiredPatchAsync(connection, nonce, 2, "{\"x\":{\"y\":null,\"w\":3}}");

            Task<ReportedPatchResponse> rejected = UpdateReportedAsync(twinClient, "{\"bad\":1}");
            await WaitUntilAsync(() => connection.PublishedMessages.Count(p => GetType(p) == "reported-patch:1") == 1);
            await RespondToReportedPatchAsync(connection, connection.PublishedMessages.Last(p => GetType(p) == "reported-patch:1"), Result.PayloadTooLarge, 2);
            await rejected;

            Task<ReportedPatchResponse> applied = UpdateReportedAsync(twinClient, "{\"r\":null,\"s\":2}");
            await WaitUntilAsync(() => connection.PublishedMessages.Count(p => GetType(p) == "reported-patch:1") == 2);
            await RespondToReportedPatchAsync(connection, connection.PublishedMessages.Last(p => GetType(p) == "reported-patch:1"), Result.Ok, 3);
            await applied;

            TwinState state = twinClient.GetTwinState();
            Assert.Equal(2ul, state.DesiredVersion);
            Assert.Equal("{\"x\":{\"z\":2,\"w\":3}}", state.Desired.ToJsonString());
            Assert.Equal(3ul, state.ReportedVersion);
            Assert.Equal("{\"s\":2}", state.Reported.ToJsonString());
            Assert.Empty(state.PendingReportedPatches);
        }

        [Fact]
        public async Task EnqueueReportedPropertiesUpdate_PersistsPatchBeforeReturningAndRaisesSendFailure()
        {
            (MockFeatureConnectionClient connection, Guid nonce) = CreateConnectionWithNonce();
            connection.OnPublish = _ => throw new InvalidOperationException("send failed");
            using ConcurrentTwinClient twinClient = new(connection);
            List<TwinState> states = new();
            ReportedPatchCompletedEventArgs? completion = null;
            twinClient.TwinStateChanged += states.Add;
            twinClient.ReportedPatchCompleted += args => completion = args;

            twinClient.EnqueueReportedPropertiesUpdate(JsonNode.Parse("{\"a\":1}")!.AsObject());

            Assert.Equal("{\"a\":1}", Assert.Single(Assert.Single(states).PendingReportedPatches).ToJsonString());

            await ReceiveBirthAckAsync(connection, nonce, desiredVersion: 1, reportedVersion: 4);
            await WaitUntilAsync(() => completion != null);
            Assert.Equal("{\"a\":1}", completion!.Patch.ToJsonString());
            Assert.Null(completion.Response);
            Assert.IsType<InvalidOperationException>(completion.Error);
            await WaitUntilAsync(() => states.Count == 2);
            Assert.Empty(states[1].PendingReportedPatches);
        }

        [Fact]
        public async Task TwinState_InitialVersionsWithoutPayloads_AreOnlyClaimedOnceFullPayloadsAreHeld()
        {
            (MockFeatureConnectionClient connection, Guid nonce) = CreateConnectionWithNonce();
            connection.InitialDesiredPropertiesVersion = 5;
            connection.InitialReportedPropertiesVersion = 3;
            using ConcurrentTwinClient twinClient = new(connection);

            TwinState state = twinClient.GetTwinState();
            Assert.Equal(0ul, state.DesiredVersion);
            Assert.Equal(0ul, state.ReportedVersion);

            // The patch is still delivered in order, but the device doesn't hold the rest of the desired properties.
            await ReceiveDesiredPatchAsync(connection, nonce, 6, "{\"b\":2}");
            Assert.Equal(0ul, twinClient.GetTwinState().DesiredVersion);

            connection.OnPublish = async publish =>
            {
                var response = new TwinGetResponse()
                {
                    DesiredVersion = 6,
                    DesiredPayload = ByteString.CopyFromUtf8("{\"a\":1,\"b\":2}"),
                    ReportedVersion = 3,
                    ReportedPayload = ByteString.CopyFromUtf8("{\"r\":1}"),
                };
                await connection.SimulateReceiveAsync(CreateInboundTwinPublish("get-response:1", publish.CorrelationData!, response.ToByteArray()));
                return new MqttPublishAck() { ReasonCode = MqttPublishAckReasonCode.Success };
            };
            await twinClient.GetTwinAsync(cancellationToken: TestContext.Current.CancellationToken);

            state = twinClient.GetTwinState();
            Assert.Equal(6ul, state.DesiredVersion);
            Assert.Equal("{\"a\":1,\"b\":2}", state.Desired.ToJsonString());
            Assert.Equal(3ul, state.ReportedVersion);
            Assert.Equal("{\"r\":1}", state.Reported.ToJsonString());
        }

        [Fact]
        public async Task UpdateReportedPropertiesAsync_CancelledAfterBeingSentAndConnectionLost_StaysPending()
        {
            (MockFeatureConnectionClient connection, Guid nonce) = CreateConnectionWithNonce();
            using ConcurrentTwinClient twinClient = new(connection);
            await ReceiveBirthAckAsync(connection, nonce, desiredVersion: 1, reportedVersion: 2);
            using CancellationTokenSource caller = new();

            Task<ReportedPatchResponse> cancelled = twinClient.UpdateReportedPropertiesAsync(
                new() { ReportedProperties = new JsonObject() { ["temperature"] = 1 } },
                caller.Token);
            Assert.Single(connection.PublishedMessages);

            connection.SimulateConnectionLost();
            caller.Cancel();
            await Assert.ThrowsAnyAsync<OperationCanceledException>(() => cancelled);

            // IoT hub may have applied the patch, so it is kept for persistence and sent again once reconnected.
            Assert.Equal("{\"temperature\":1}", Assert.Single(twinClient.GetTwinState().PendingReportedPatches).ToJsonString());

            Guid newNonce = Guid.NewGuid();
            connection.ConnectionNonce = newNonce;
            await ReceiveBirthAckAsync(connection, newNonce, desiredVersion: 1, reportedVersion: 2);
            Assert.Equal(2, connection.PublishedMessages.Count);
            Assert.Equal("{\"temperature\":1}", ParseReportedPatch(connection.PublishedMessages[1]).Patch.ToJsonString());
        }

        [Fact]
        public async Task EnqueueReportedPropertiesUpdate_FromTwinStateChangedHandler_IsSentAfterSnapshotContainingIt()
        {
            (MockFeatureConnectionClient connection, Guid nonce) = CreateConnectionWithNonce();
            using ConcurrentTwinClient twinClient = new(connection);
            await ReceiveBirthAckAsync(connection, nonce, desiredVersion: 1, reportedVersion: 2);
            List<string> events = new();
            connection.OnPublish = publish =>
            {
                events.Add("publish");
                return Task.FromResult(new MqttPublishAck() { ReasonCode = MqttPublishAckReasonCode.Success });
            };
            twinClient.TwinStateChanged += state =>
            {
                events.Add($"state {state.PendingReportedPatches.Count}");
                if (events.Count == 1)
                {
                    twinClient.EnqueueReportedPropertiesUpdate(JsonNode.Parse("{\"a\":1}")!.AsObject());
                }
            };

            await ReceiveTwinPushAsync(connection, nonce, 1, "{\"d\":1}");

            Assert.Equal(["state 0", "state 1", "publish"], events);
        }

        [Fact]
        public async Task ReportedPatchOk_SkippedVersion_MarksReportedPropertiesIncomplete()
        {
            (MockFeatureConnectionClient connection, Guid nonce) = CreateConnectionWithNonce();
            TwinState initialState = new() { ReportedVersion = 4, Reported = new JsonObject() { ["a"] = 0 } };
            using ConcurrentTwinClient twinClient = new(connection, initialState);
            await ReceiveBirthAckAsync(connection, nonce, desiredVersion: 1, reportedVersion: 4);

            Task<ReportedPatchResponse> update = UpdateReportedAsync(twinClient, "{\"b\":1}");
            await RespondToReportedPatchAsync(connection, Assert.Single(connection.PublishedMessages), Result.Ok, 6);
            await update;

            // Version 5 was written by someone else, so this device's copy lacks its changes.
            Assert.Equal(0ul, twinClient.GetTwinState().ReportedVersion);
            Assert.Equal(0ul, connection.ReplacedReportedPropertiesVersions.Last());
        }

        [Fact]
        public async Task DesiredProbe_DuringResync_NewerThanGetResponse_ResyncsAgain()
        {
            (MockFeatureConnectionClient connection, Guid nonce) = CreateConnectionWithNonce();
            using ConcurrentTwinClient twinClient = new(connection);
            await ReceiveTwinPushAsync(connection, nonce, 5, "{\"a\":5}");

            await ReceiveDesiredPatchAsync(connection, nonce, 7, payload: null);
            MqttPublish firstGet = Assert.Single(connection.PublishedMessages);

            // IoT hub may have answered the outstanding GET before it reached this version.
            await ReceiveDesiredPatchAsync(connection, nonce, 8, payload: null);
            await ReceiveGetResponseAsync(connection, firstGet, 7, "{\"a\":7}");

            Assert.Equal(2, connection.PublishedMessages.Count);
            Assert.Equal("get:1", GetType(connection.PublishedMessages[1]));
            Assert.NotEqual(firstGet.CorrelationData, connection.PublishedMessages[1].CorrelationData);
        }

        [Fact]
        public async Task ReportedPatchOk_AfterNewerGetResponse_DoesNotOverwriteNewerReportedProperties()
        {
            (MockFeatureConnectionClient connection, Guid nonce) = CreateConnectionWithNonce();
            using ConcurrentTwinClient twinClient = new(connection);
            await ReceiveBirthAckAsync(connection, nonce, desiredVersion: 1, reportedVersion: 4);

            Task<ReportedPatchResponse> update = UpdateReportedAsync(twinClient, "{\"a\":1}");
            MqttPublish patchPublish = Assert.Single(connection.PublishedMessages);

            // The response to a get twin request, sent after IoT hub applied the patch at version 5 and someone else changed
            // the reported properties at version 6, arrives before the patch's response.
            RespondToGetWithReportedProperties(connection, 6, "{\"a\":2}");
            await twinClient.GetTwinAsync(getDesired: false, cancellationToken: TestContext.Current.CancellationToken);
            await RespondToReportedPatchAsync(connection, patchPublish, Result.Ok, 5);

            Assert.Equal(5ul, (await update).Version);
            TwinState state = twinClient.GetTwinState();
            Assert.Equal(6ul, state.ReportedVersion);
            Assert.Equal("{\"a\":2}", state.Reported.ToJsonString());
        }

        [Fact]
        public async Task TwinPush_OlderReportedVersionThanRevealedOnThisConnection_IsIgnored()
        {
            (MockFeatureConnectionClient connection, Guid nonce) = CreateConnectionWithNonce();
            using ConcurrentTwinClient twinClient = new(connection);
            await ReceiveBirthAckAsync(connection, nonce, desiredVersion: 1, reportedVersion: 6);
            RespondToGetWithReportedProperties(connection, 6, "{\"a\":6}");
            await twinClient.GetTwinAsync(getDesired: false, cancellationToken: TestContext.Current.CancellationToken);
            TwinPushReceivedEventArgs? pushed = null;
            twinClient.TwinPushReceived += args => pushed = args;

            // A push that IoT hub sent before the reported properties reached version 6.
            var push = new TwinPush() { Reported = new Section { Version = 5, Payload = ByteString.CopyFromUtf8("{\"a\":5}") } };
            await connection.SimulateReceiveAsync(CreateInboundTwinPublish("twin-push:1", nonce.ToByteArray(bigEndian: true), push.ToByteArray()));

            Assert.Null(pushed);
            TwinState state = twinClient.GetTwinState();
            Assert.Equal(6ul, state.ReportedVersion);
            Assert.Equal("{\"a\":6}", state.Reported.ToJsonString());
            Assert.Empty(connection.ReplacedReportedPropertiesVersions);
        }

        [Fact]
        public async Task Constructor_AfterBirthAckWithOlderDesiredVersion_DiscardsInitialVersionAndResyncs()
        {
            (MockFeatureConnectionClient connection, Guid nonce) = CreateConnectionWithNonce();
            connection.InitialDesiredPropertiesVersion = 10;
            connection.LatestBirthAck = new BirthAckVersions(nonce, DesiredVersion: 5, ReportedVersion: 1);
            using ConcurrentTwinClient twinClient = new(connection);
            List<ulong> patches = new();
            twinClient.DesiredPatchReceived += args => patches.Add(args.DesiredPropertiesVersion);

            Assert.Equal(new[] { 0ul }, connection.ReplacedDesiredPropertiesVersions);
            MqttPublish get = Assert.Single(connection.PublishedMessages);
            Assert.Equal("get:1", GetType(get));

            await ReceiveGetResponseAsync(connection, get, 5, "{}");
            await ReceiveDesiredPatchAsync(connection, nonce, 6, "{\"a\":6}");

            Assert.Equal(new[] { 6ul }, patches);
        }

        [Fact]
        public async Task TwinIdentityChanged_DiscardsHeldTwinAndKeepsPendingPatches()
        {
            (MockFeatureConnectionClient connection, Guid nonce) = CreateConnectionWithNonce();
            TwinState initialState = new()
            {
                DesiredVersion = 3,
                Desired = new JsonObject() { ["d"] = 1 },
                ReportedVersion = 4,
                Reported = new JsonObject() { ["r"] = 1 },
                PendingReportedPatches = [new JsonObject() { ["a"] = 1 }],
            };
            using ConcurrentTwinClient twinClient = new(connection, initialState);
            List<ulong> patches = new();
            twinClient.DesiredPatchReceived += args => patches.Add(args.DesiredPropertiesVersion);

            connection.SimulateTwinIdentityChanged();

            TwinState state = twinClient.GetTwinState();
            Assert.Equal(0ul, state.DesiredVersion);
            Assert.Empty(state.Desired);
            Assert.Equal(0ul, state.ReportedVersion);
            Assert.Empty(state.Reported);
            Assert.Equal("{\"a\":1}", Assert.Single(state.PendingReportedPatches).ToJsonString());

            // The new twin's first patch is delivered without resyncing.
            await ReceiveDesiredPatchAsync(connection, nonce, 1, "{\"x\":1}");
            Assert.Equal(new[] { 1ul }, patches);
            Assert.Empty(connection.PublishedMessages);
        }

        [Fact]
        public async Task EnqueueReportedPropertiesUpdate_BirthAckWhileSnapshotIsRaised_IsSentOnlyAfterSnapshotHandlerReturns()
        {
            (MockFeatureConnectionClient connection, Guid nonce) = CreateConnectionWithNonce();
            using ConcurrentTwinClient twinClient = new(connection);
            int isHandlingSnapshot = 0;
            bool wasSentWhileHandlingSnapshot = false;
            connection.OnPublish = publish =>
            {
                if (Volatile.Read(ref isHandlingSnapshot) == 1)
                {
                    wasSentWhileHandlingSnapshot = true;
                }

                return Task.FromResult(new MqttPublishAck() { ReasonCode = MqttPublishAckReasonCode.Success });
            };
            Task? birthAck = null;
            twinClient.TwinStateChanged += state =>
            {
                if (birthAck != null)
                {
                    return;
                }

                // The application is still persisting the snapshot when the birth ack arrives.
                Volatile.Write(ref isHandlingSnapshot, 1);
                birthAck = Task.Run(() => ReceiveBirthAckAsync(connection, nonce, desiredVersion: 1, reportedVersion: 4));
                Thread.Sleep(200);
                Volatile.Write(ref isHandlingSnapshot, 0);
            };

            twinClient.EnqueueReportedPropertiesUpdate(JsonNode.Parse("{\"a\":1}")!.AsObject());
            await birthAck!;

            await WaitUntilAsync(() => connection.PublishedMessages.Count == 1);
            Assert.False(wasSentWhileHandlingSnapshot);
        }

        private static void RespondToGetWithReportedProperties(MockFeatureConnectionClient connection, ulong reportedVersion, string reportedPayload)
        {
            connection.OnPublish = async publish =>
            {
                if (GetType(publish) == "get:1")
                {
                    var response = new TwinGetResponse()
                    {
                        ReportedVersion = reportedVersion,
                        ReportedPayload = ByteString.CopyFromUtf8(reportedPayload),
                    };
                    await connection.SimulateReceiveAsync(CreateInboundTwinPublish("get-response:1", publish.CorrelationData!, response.ToByteArray()));
                }

                return new MqttPublishAck() { ReasonCode = MqttPublishAckReasonCode.Success };
            };
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

        private static async Task WaitUntilAsync(Func<bool> condition)
        {
            using CancellationTokenSource timeout = CancellationTokenSource.CreateLinkedTokenSource(TestContext.Current.CancellationToken);
            timeout.CancelAfter(TimeSpan.FromSeconds(10));
            while (!condition())
            {
                await Task.Delay(10, timeout.Token);
            }
        }

        private static Task ReceiveGetResponseAsync(MockFeatureConnectionClient connection, MqttPublish getPublish, ulong desiredVersion, string desiredPayload)
        {
            var response = new TwinGetResponse()
            {
                DesiredVersion = desiredVersion,
                DesiredPayload = ByteString.CopyFromUtf8(desiredPayload),
            };

            return connection.SimulateReceiveAsync(CreateInboundTwinPublish("get-response:1", getPublish.CorrelationData!, response.ToByteArray()));
        }

        private static Task ReceiveBirthAckAsync(MockFeatureConnectionClient connection, Guid nonce, ulong desiredVersion, ulong reportedVersion)
        {
            var publish = new MqttPublish()
            {
                Topic = "ih/someDeviceId/dev/presence",
                Payload = new BirthAck() { DesiredVersion = desiredVersion, ReportedVersion = reportedVersion }.ToByteArray(),
                CorrelationData = nonce.ToByteArray(bigEndian: true),
                QualityOfServiceLevel = MqttQualityOfServiceLevel.AtMostOnce,
            };
            publish.UserProperties.Add(new("type", Encoding.UTF8.GetBytes("birth-ack:1")));
            return connection.SimulateReceiveAsync(publish);
        }

        private static Task<ReportedPatchResponse> UpdateReportedAsync(ConcurrentTwinClient twinClient, string patch)
        {
            return twinClient.UpdateReportedPropertiesAsync(
                new() { ReportedProperties = JsonNode.Parse(patch)!.AsObject() },
                TestContext.Current.CancellationToken);
        }

        private static (ulong IfMatch, JsonObject Patch) ParseReportedPatch(MqttPublish publish)
        {
            ReportedPatch patch = ReportedPatch.Parser.ParseFrom(publish.Payload);
            return (patch.IfMatch, JsonNode.Parse(patch.Payload.ToStringUtf8())!.AsObject());
        }

        private static Task RespondToReportedPatchAsync(MockFeatureConnectionClient connection, MqttPublish patchPublish, Result result, ulong version)
        {
            var response = new ReportedPatchResponse() { Result = result, Version = version };
            return connection.SimulateReceiveAsync(CreateInboundTwinPublish("reported-patch-response:1", patchPublish.CorrelationData!, response.ToByteArray()));
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
