// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Google.Protobuf;
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
            using TwinClient twinClient = new(connection);

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
            using TwinClient twinClient = new(connection);

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
            using TwinClient twinClient = new(connection);

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
            using TwinClient twinClient = new(connection);

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
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv5, DeviceId),
            };
            using TwinClient twinClient = new(connection);

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
            using TwinClient twinClient = new(connection);

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
        public async Task GetTwinAsync_ThrowsAfterDisconnect()
        {
            using MockConnectionMqttClient mqttClient = new() { AutoRespondToBirth = true };
            using ConnectionClient connection = new(new() { MqttClient = mqttClient });
            await connection.ConnectToHubAsync(
                MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv5, DeviceId),
                TestContext.Current.CancellationToken);
            using TwinClient twinClient = new(connection);

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
            TwinClient twinClient = new(connection);
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
            using TwinClient twinClient = new(connection);
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
            using TwinClient twinClient = new(connection);
            await ReceiveTwinPushAsync(connection, nonce, 5, "{\"fanSpeed\":40}");

            List<object> received = new();
            twinClient.DesiredPatchReceived += args => received.Add(args);
            twinClient.TwinPushReceived += args => received.Add(args);

            await ReceiveDesiredPatchAsync(connection, nonce, 7, payload: null);

            MqttPublish getPublish = Assert.Single(connection.PublishedMessages);
            Assert.Equal(OutgoingTwinTopic, getPublish.Topic);
            Assert.Equal("get:1", GetType(getPublish));
            Assert.Equal(Sections.Desired, TwinGet.Parser.ParseFrom(getPublish.Payload).Sections);

            // While resyncing, patches are buffered and probes are dropped.
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
            using TwinClient twinClient = new(connection);
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
            using TwinClient twinClient = new(connection);

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
            using TwinClient twinClient = new(connection);

            List<ulong> snapshotVersions = new();
            twinClient.TwinPushReceived += args => snapshotVersions.Add(args.Desired!.PropertiesVersion);

            await ReceiveDesiredPatchAsync(connection, nonce, 2, payload: null);
            MqttPublish firstGet = Assert.Single(connection.PublishedMessages);

            for (ulong version = 3; version < 3 + TwinClient.DesiredResyncBufferCapacity + 1; version++)
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
            using TwinClient twinClient = new(connection);
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
            using TwinClient twinClient = new(connection);

            bool eventRaised = false;
            twinClient.TwinPushReceived += _ => eventRaised = true;

            await ReceiveDesiredPatchAsync(connection, nonce, 3, payload: null);
            MqttPublish getPublish = Assert.Single(connection.PublishedMessages);

            connection.ConnectionNonce = Guid.NewGuid();
            await ReceiveGetResponseAsync(connection, getPublish, 3, "{}");

            Assert.False(eventRaised);
        }

        [Fact]
        public async Task DesiredPatchAndTwinPush_ForAnotherConnection_AreDropped()
        {
            (MockFeatureConnectionClient connection, _) = CreateConnectionWithNonce();
            using TwinClient twinClient = new(connection);

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
            using TwinClient twinClient = new(connection);
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
            using TwinClient twinClient = new(connection);
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
            using TwinClient twinClient = new(connection);

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
            using TwinClient twinClient = new(connection);

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
            using TwinClient twinClient = new(connection);

            Assert.Null(twinClient.AuthoritativeDesiredPropertiesVersion);
            Assert.Null(twinClient.AuthoritativeReportedPropertiesVersion);

            await ReceiveBirthAckAsync(connection, Guid.NewGuid(), desiredVersion: 9, reportedVersion: 9);
            Assert.Null(twinClient.AuthoritativeDesiredPropertiesVersion);

            await ReceiveBirthAckAsync(connection, nonce, desiredVersion: 4, reportedVersion: 2);
            Assert.Equal(4ul, twinClient.AuthoritativeDesiredPropertiesVersion);
            Assert.Equal(2ul, twinClient.AuthoritativeReportedPropertiesVersion);

            connection.OnPublish = async publish =>
            {
                var response = new ReportedPatchResponse() { Result = Result.VersionMismatch, Version = 5 };
                await connection.SimulateReceiveAsync(CreateInboundTwinPublish("reported-patch-response:1", publish.CorrelationData!, response.ToByteArray()));
                return new MqttPublishAck() { ReasonCode = MqttPublishAckReasonCode.Success };
            };
            await twinClient.UpdateReportedPropertiesAsync(new() { IfMatch = 2, ReportedProperties = new JsonObject() }, TestContext.Current.CancellationToken);
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
            using TwinClient twinClient = new(connection);

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
            using TwinClient twinClient = new(connection);

            await ReceiveBirthAckAsync(connection, nonce, desiredVersion: 1, reportedVersion: 5);

            Assert.Equal(new[] { 0ul }, connection.ReplacedReportedPropertiesVersions);
            Assert.Empty(connection.PublishedMessages);
        }

        [Fact]
        public async Task TwinPush_OlderReportedVersionThanHeld_AdoptsPushedVersion()
        {
            (MockFeatureConnectionClient connection, Guid nonce) = CreateConnectionWithNonce();
            connection.InitialReportedPropertiesVersion = 10;
            using TwinClient twinClient = new(connection);

            var push = new TwinPush() { Reported = new Section { Version = 5, Payload = ByteString.CopyFromUtf8("{}") } };
            await connection.SimulateReceiveAsync(CreateInboundTwinPublish("twin-push:1", nonce.ToByteArray(bigEndian: true), push.ToByteArray()));
            await ReceiveBirthAckAsync(connection, nonce, desiredVersion: 1, reportedVersion: 5);

            Assert.Equal(new[] { 5ul }, connection.ReplacedReportedPropertiesVersions);
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
