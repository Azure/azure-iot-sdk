using MQTTnet;
using MQTTnet.Formatter;
using MQTTnet.Protocol;
using System.Buffers;
using System.Text;
using Xunit;

namespace Microsoft.Azure.Devices.Client.IntegrationTests.StubService
{
    /// <summary>
    /// Tests of the <see cref="InProcessMqttBroker"/>'s fault injection, which is only ever triggered by a PUBLISH to
    /// <see cref="MqttFaultInjection.RequestTopic"/>.
    /// </summary>
    /// <remarks>
    /// The point of these is that nothing here calls a fault injecting method on the broker, because there is no such
    /// method. Everything goes over MQTT, which is what lets a process that is not hosting the broker inject the same
    /// faults, and the fault's structure is carried in the payload so that MQTT 3.1.1 can express it as fully as MQTT 5.
    /// </remarks>
    public class MqttFaultInjectionTests
    {
        private static readonly TimeSpan s_waitTimeout = TimeSpan.FromSeconds(30);

        [Theory]
        [InlineData(MqttProtocolVersion.V311)]
        [InlineData(MqttProtocolVersion.V500)]
        public async Task DisconnectFault_OverEitherProtocolVersion_TerminatesTheTargetsSession(MqttProtocolVersion protocolVersion)
        {
            await using var broker = await InProcessMqttBroker.StartAsync(TestContext.Current.CancellationToken);
            await using MqttFaultInjectionClient faults = await MqttFaultInjectionClient.ForBrokerAsync(
                broker,
                protocolVersion,
                cancellationToken: TestContext.Current.CancellationToken);

            // The victim is on the other protocol version from the client asking for the fault, so neither half of the
            // exchange can be relying on something only one version has.
            MqttProtocolVersion victimProtocolVersion = protocolVersion == MqttProtocolVersion.V311
                ? MqttProtocolVersion.V500
                : MqttProtocolVersion.V311;

            await using var victim = await TestMqttClient.ConnectAsync(broker, "victim-device", victimProtocolVersion);

            bool wasDisconnected = await faults.DisconnectClientAsync(
                "victim-device",
                MqttDisconnectReasonCode.ServerBusy,
                "the test says so",
                cancellationToken: TestContext.Current.CancellationToken);

            Assert.True(wasDisconnected);

            MqttClientDisconnectedEventArgs disconnect = await victim.Disconnected.Task.WaitAsync(
                s_waitTimeout,
                TestContext.Current.CancellationToken);

            // MQTT 3.1.1 has no server-to-client DISCONNECT packet, so a 3.1.1 victim only ever learns that its socket
            // closed. The reason code travels in the request payload either way; only the delivery to the victim differs.
            Assert.Equal(
                victimProtocolVersion == MqttProtocolVersion.V500
                    ? MqttClientDisconnectReason.ServerBusy
                    : MqttClientDisconnectReason.NormalDisconnection,
                disconnect.Reason);

            Assert.DoesNotContain(
                "victim-device",
                await broker.GetConnectedClientIdsAsync(TestContext.Current.CancellationToken));
        }

        [Fact]
        public async Task DisconnectFault_ForAClientThatIsNotConnected_SucceedsWithoutApplyingAnything()
        {
            await using var broker = await InProcessMqttBroker.StartAsync(TestContext.Current.CancellationToken);
            await using MqttFaultInjectionClient faults = await MqttFaultInjectionClient.ForBrokerAsync(
                broker,
                cancellationToken: TestContext.Current.CancellationToken);

            bool wasDisconnected = await faults.DisconnectClientAsync(
                "no-such-device",
                MqttDisconnectReasonCode.ServerBusy,
                cancellationToken: TestContext.Current.CancellationToken);

            Assert.False(wasDisconnected);
        }

        [Fact]
        public async Task DisconnectFault_CanBeDelayed()
        {
            await using var broker = await InProcessMqttBroker.StartAsync(TestContext.Current.CancellationToken);
            await using MqttFaultInjectionClient faults = await MqttFaultInjectionClient.ForBrokerAsync(
                broker,
                cancellationToken: TestContext.Current.CancellationToken);

            await using var victim = await TestMqttClient.ConnectAsync(broker, "victim-device", MqttProtocolVersion.V500);

            Task<bool> disconnecting = faults.DisconnectClientAsync(
                "victim-device",
                MqttDisconnectReasonCode.ServerBusy,
                delay: TimeSpan.FromSeconds(2),
                cancellationToken: TestContext.Current.CancellationToken);

            // The response is only published once the fault has landed, so the request is still in flight while the delay
            // runs and the victim is still connected.
            await Task.Delay(TimeSpan.FromMilliseconds(500), TestContext.Current.CancellationToken);
            Assert.False(disconnecting.IsCompleted);
            Assert.False(victim.Disconnected.Task.IsCompleted);

            Assert.True(await disconnecting);
            await victim.Disconnected.Task.WaitAsync(s_waitTimeout, TestContext.Current.CancellationToken);
        }

        [Fact]
        public async Task ListClientsFault_ReportsTheConnectedClients()
        {
            await using var broker = await InProcessMqttBroker.StartAsync(TestContext.Current.CancellationToken);
            await using MqttFaultInjectionClient faults = await MqttFaultInjectionClient.ForBrokerAsync(
                broker,
                clientId: "the-fault-injector",
                cancellationToken: TestContext.Current.CancellationToken);

            await using var other = await TestMqttClient.ConnectAsync(broker, "some-device", MqttProtocolVersion.V311);

            IReadOnlyList<string> clientIds = await faults.GetConnectedClientIdsAsync(TestContext.Current.CancellationToken);

            Assert.Contains("some-device", clientIds);
            Assert.Contains("the-fault-injector", clientIds);
        }

        [Fact]
        public async Task UnknownFault_IsReportedAsAnError()
        {
            await using var broker = await InProcessMqttBroker.StartAsync(TestContext.Current.CancellationToken);
            await using MqttFaultInjectionClient faults = await MqttFaultInjectionClient.ForBrokerAsync(
                broker,
                cancellationToken: TestContext.Current.CancellationToken);

            InvalidOperationException exception = await Assert.ThrowsAsync<InvalidOperationException>(
                () => faults.SendAsync(
                    new MqttFaultInjectionRequest { Fault = "meltDown" },
                    TestContext.Current.CancellationToken));

            Assert.Contains("meltDown", exception.Message);
        }

        [Fact]
        public async Task DisconnectFault_WithAReasonCodeMqttDoesNotDefine_IsReportedAsAnError()
        {
            await using var broker = await InProcessMqttBroker.StartAsync(TestContext.Current.CancellationToken);
            await using MqttFaultInjectionClient faults = await MqttFaultInjectionClient.ForBrokerAsync(
                broker,
                cancellationToken: TestContext.Current.CancellationToken);

            InvalidOperationException exception = await Assert.ThrowsAsync<InvalidOperationException>(
                () => faults.SendAsync(
                    new MqttFaultInjectionRequest
                    {
                        Fault = MqttFaultInjection.Faults.Disconnect,
                        ClientId = "victim-device",
                        ReasonCode = 250,
                    },
                    TestContext.Current.CancellationToken));

            Assert.Contains("250", exception.Message);
        }

        [Fact]
        public async Task MalformedRequest_IsIgnoredAndLeavesTheBrokerWorking()
        {
            await using var broker = await InProcessMqttBroker.StartAsync(TestContext.Current.CancellationToken);
            await using MqttFaultInjectionClient faults = await MqttFaultInjectionClient.ForBrokerAsync(
                broker,
                cancellationToken: TestContext.Current.CancellationToken);

            await using var victim = await TestMqttClient.ConnectAsync(broker, "victim-device", MqttProtocolVersion.V500);

            await victim.PublishAsync(MqttFaultInjection.RequestTopic, "this is not a fault injection request");

            // A request that cannot be parsed cannot be answered either, so the proof that it did no harm is that the next
            // well formed one still works.
            Assert.True(await faults.DisconnectClientAsync(
                "victim-device",
                MqttDisconnectReasonCode.ServerBusy,
                cancellationToken: TestContext.Current.CancellationToken));
        }

        /// <summary>
        /// The out-of-process case, as closely as a single test process can stage it: a bare MQTT client that knows only
        /// the broker's endpoint, the topic, and the JSON, with no reference to any of the helper types.
        /// </summary>
        [Fact]
        public async Task AnyClientThatKnowsTheTopicAndThePayload_CanInjectAFault()
        {
            await using var broker = await InProcessMqttBroker.StartAsync(TestContext.Current.CancellationToken);

            await using var victim = await TestMqttClient.ConnectAsync(broker, "victim-device", MqttProtocolVersion.V500);
            await using var stranger = await TestMqttClient.ConnectAsync(broker, "a-stranger", MqttProtocolVersion.V311);

            await stranger.SubscribeAsync("$fault/res/request-1");

            await stranger.PublishAsync(
                "$fault/req",
                """
                {"requestId":"request-1","fault":"disconnect","clientId":"victim-device","reasonCode":137,"reasonString":"a stranger says so"}
                """);

            MqttApplicationMessage response = await stranger.NextMessage.Task.WaitAsync(s_waitTimeout, TestContext.Current.CancellationToken);

            Assert.Equal("$fault/res/request-1", response.Topic);

            MqttFaultInjectionResponse? parsed = MqttFaultInjection.DeserializeResponse(response.Payload.ToArray());

            Assert.NotNull(parsed);
            Assert.Equal("request-1", parsed.RequestId);
            Assert.True(parsed.Succeeded);
            Assert.True(parsed.FaultApplied);

            MqttClientDisconnectedEventArgs disconnect = await victim.Disconnected.Task.WaitAsync(
                s_waitTimeout,
                TestContext.Current.CancellationToken);

            Assert.Equal(MqttClientDisconnectReason.ServerBusy, disconnect.Reason);
        }

        [Fact]
        public async Task AFaultRequestWithoutARequestId_IsInjectedWithoutAResponse()
        {
            await using var broker = await InProcessMqttBroker.StartAsync(TestContext.Current.CancellationToken);

            await using var victim = await TestMqttClient.ConnectAsync(broker, "victim-device", MqttProtocolVersion.V500);
            await using var stranger = await TestMqttClient.ConnectAsync(broker, "a-stranger", MqttProtocolVersion.V311);

            await stranger.SubscribeAsync(MqttFaultInjection.ResponseTopicFilter);

            // Fire and forget is the shape a shell one liner would take.
            await stranger.PublishAsync(
                MqttFaultInjection.RequestTopic,
                """
                {"fault":"disconnect","clientId":"victim-device","reasonCode":137}
                """);

            await victim.Disconnected.Task.WaitAsync(s_waitTimeout, TestContext.Current.CancellationToken);

            Assert.False(stranger.NextMessage.Task.IsCompleted);
        }

        [Fact]
        public async Task FaultInjectionRequests_AreNotForwardedToSubscribers()
        {
            await using var broker = await InProcessMqttBroker.StartAsync(TestContext.Current.CancellationToken);
            await using MqttFaultInjectionClient faults = await MqttFaultInjectionClient.ForBrokerAsync(
                broker,
                cancellationToken: TestContext.Current.CancellationToken);

            await using var eavesdropper = await TestMqttClient.ConnectAsync(broker, "eavesdropper", MqttProtocolVersion.V500);
            await eavesdropper.SubscribeAsync(MqttFaultInjection.RequestTopic);

            await faults.GetConnectedClientIdsAsync(TestContext.Current.CancellationToken);

            // The control channel is between the caller and the broker, so it never reaches the topic space the devices
            // and the stubs share.
            Assert.False(eavesdropper.NextMessage.Task.IsCompleted);
        }

        /// <summary>
        /// A plain MQTT client, standing in for whatever is connected to the broker: a device, or something injecting a
        /// fault with nothing but the topic and the payload.
        /// </summary>
        private sealed class TestMqttClient : IAsyncDisposable
        {
            private readonly IMqttClient _mqttClient;

            private TestMqttClient(IMqttClient mqttClient)
            {
                _mqttClient = mqttClient;

                _mqttClient.DisconnectedAsync += args =>
                {
                    Disconnected.TrySetResult(args);
                    return Task.CompletedTask;
                };

                _mqttClient.ApplicationMessageReceivedAsync += args =>
                {
                    NextMessage.TrySetResult(args.ApplicationMessage);
                    return Task.CompletedTask;
                };
            }

            public TaskCompletionSource<MqttClientDisconnectedEventArgs> Disconnected { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);

            public TaskCompletionSource<MqttApplicationMessage> NextMessage { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);

            public static async Task<TestMqttClient> ConnectAsync(
                InProcessMqttBroker broker,
                string clientId,
                MqttProtocolVersion protocolVersion)
            {
                var client = new TestMqttClient(new MqttClientFactory().CreateMqttClient());

                await client._mqttClient.ConnectAsync(
                    new MqttClientOptionsBuilder()
                        .WithProtocolVersion(protocolVersion)
                        .WithTcpServer(broker.HostName, broker.Port)
                        .WithClientId(clientId)
                        .WithCleanSession(true)
                        .Build(),
                    TestContext.Current.CancellationToken);

                return client;
            }

            public Task SubscribeAsync(string topic)
            {
                return _mqttClient.SubscribeAsync(
                    new MqttClientSubscribeOptionsBuilder()
                        .WithTopicFilter(filter => filter
                            .WithTopic(topic)
                            .WithQualityOfServiceLevel(MqttQualityOfServiceLevel.AtLeastOnce))
                        .Build(),
                    TestContext.Current.CancellationToken);
            }

            public Task PublishAsync(string topic, string payload)
            {
                return _mqttClient.PublishAsync(
                    new MqttApplicationMessageBuilder()
                        .WithTopic(topic)
                        .WithPayload(Encoding.UTF8.GetBytes(payload))
                        .WithQualityOfServiceLevel(MqttQualityOfServiceLevel.AtLeastOnce)
                        .Build(),
                    TestContext.Current.CancellationToken);
            }

            public async ValueTask DisposeAsync()
            {
                try
                {
                    if (_mqttClient.IsConnected)
                    {
                        await _mqttClient.DisconnectAsync(new MqttClientDisconnectOptions(), CancellationToken.None);
                    }
                }
                catch (Exception)
                {
                    // Teardown of a test helper should never mask the test's own failure.
                }

                _mqttClient.Dispose();
            }
        }
    }
}
