// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Unified.Connection;
using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Mqtt;
using Xunit;

namespace Microsoft.Azure.Iot.Device.UnitTests.Unified
{
    /// <summary>
    /// Tests for the failure path of the classic (MQTTv3) device presence flow that the unified
    /// <see cref="ConnectionClient"/> runs in <c>HandleConnectedToHubAsync</c> during the initial connect.
    /// </summary>
    /// <remarks>
    /// On an MQTTv3 hub the device presence flow re-subscribes to the classic twin, desired-property and direct-method
    /// topics. If the broker grants any of them a QoS other than the requested QoS 0, the flow treats the subscribe as
    /// failed and tears the connection down with <c>DisconnectAsync(desireReconnection: true)</c>. The
    /// <c>MqttConnectionManager</c> then reconnects on its own and runs the flow again, so a single failed grant is
    /// expected to eventually recover into a successful subscribe.
    ///
    /// These tests exercise that recovery on the very first connect attempt. The presence flow runs fire-and-forget as
    /// soon as the broker accepts the CONNECT, and the connection layer now marks the connection as desired before that
    /// first attempt, so a failure there disconnects and reconnects just like it would on an already-established
    /// connection. Because the connection client's <c>ConnectAsync</c> does not complete until the presence flow has
    /// finished successfully, awaiting it is enough to prove that the client recovered from the injected failure.
    /// </remarks>
    public class DevicePresenceFailureRecoveryTests
    {
        private const string ClassicTwinResponseTopicFilter = "$iothub/twin/res/#";

        private static ConnectionContext GetMockConnectionContext()
        {
#pragma warning disable SYSLIB0026 // Type or member is obsolete (Mock certificate, don't need to load a real one using typical X509 certificate loader
            return new ConnectionContext()
            {
                AuthenticationProvider = new X509AuthenticationProvider(new System.Security.Cryptography.X509Certificates.X509Certificate2()),
                DeviceId = "someDeviceId",
                IotHubHostName = "someHostName",
                ConnectionProfile = Provisioning.Models.ConnectionProfile.Classic,
            };
#pragma warning restore SYSLIB0026 // Type or member is obsolete
        }

        // The presence flow subscribes to the twin response (0), desired-property patch (1) and direct-method request (2)
        // topics in a single SUBSCRIBE. A failed grant on any one of them, or on all of them, must trigger recovery.
        [Theory]
        [InlineData(0)]
        [InlineData(1)]
        [InlineData(2)]
        [InlineData(-1)] // -1 fails every topic in the presence subscribe
        public async Task AnnounceDevicePresenceRecoversWhenClassicPresenceSubscribeGrantIsUnsuccessful(int failingTopicIndex)
        {
            MockMqttClient mockMqttClient = new(true);
            ConnectionClient connectionClient = new(new()
            {
                MqttClient = mockMqttClient
            });

            int connectCount = 0;
            int disconnectCount = 0;
            int presenceSubscribeCount = 0;

            // Sessions never resume, so every connect's presence flow re-sends the classic topic SUBSCRIBE that the
            // failure case targets.
            mockMqttClient.OnConnectAttempt += async (connect) =>
            {
                Interlocked.Increment(ref connectCount);
                return new MqttConnectAck() { IsSessionPresent = false, ResultCode = MqttConnectReasonCode.Success };
            };

            mockMqttClient.OnDisconnectAttempt += async (disconnect) =>
            {
                Interlocked.Increment(ref disconnectCount);
            };

            mockMqttClient.OnSubscribeAttempt += async (subscribe) =>
            {
                // Only the presence flow subscribes in this test, so every subscribe here is the classic topic SUBSCRIBE.
                if (Interlocked.Increment(ref presenceSubscribeCount) == 1)
                {
                    // Grant a QoS other than the requested QoS 0 on the targeted topic(s). GrantedQoS1 is not one of the
                    // reason codes the connection manager rejects on its own, so the presence flow's own
                    // "any grant that is not QoS 0" check is what detects the failure and disconnects.
                    List<MqttSubscribeAckItem> items = new();
                    int topicIndex = 0;
                    foreach (var topicFilter in subscribe.TopicFilters)
                    {
                        bool failThisTopic = failingTopicIndex < 0 || topicIndex == failingTopicIndex;
                        items.Add(new MqttSubscribeAckItem()
                        {
                            ReasonCode = failThisTopic ? MqttClientSubscribeReasonCode.GrantedQoS1 : MqttClientSubscribeReasonCode.GrantedQoS0,
                            TopicFilter = new(topicFilter.Topic, topicFilter.QualityOfServiceLevel),
                        });
                        topicIndex++;
                    }

                    return new MqttSubscribeAck() { Items = items };
                }

                return MqttObjectHelpers.CreateSuccessfulSuback(subscribe);
            };

            // TryConnectAsync only completes once the presence flow has successfully subscribed to the classic topics, which
            // requires recovering from the injected failure on the first attempt. A timeout guards against a regression
            // where the failure is not retried (which would hang forever).
            bool connected = await connectionClient.TryConnectAsync(GetMockConnectionContext(), cancellationToken: TestContext.Current.CancellationToken)
                .WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);
            Assert.True(connected);

            // The presence flow's failure handling must have disconnected at least once...
            Assert.True(disconnectCount >= 1, $"Expected at least one disconnect from the failed presence subscribe, but saw {disconnectCount}.");

            // ...and the manager must have reconnected on its own for the initial connect to eventually succeed.
            Assert.True(connectCount >= 2, $"Expected at least two connects (the initial attempt plus a reconnect after the failed presence subscribe), but saw {connectCount}.");

            // A successful classic presence flow ends by subscribing to the twin/desired/direct-method topics, so that
            // SUBSCRIBE must be the last traffic on the wire once the presence flow has completed.
            var lastTraffic = mockMqttClient.SentMqttTrafficInOrder.Last();
            Assert.NotNull(lastTraffic.Subscribe);
            Assert.Contains(lastTraffic.Subscribe.TopicFilters, topicFilter => topicFilter.Topic == ClassicTwinResponseTopicFilter);
        }
    }
}
