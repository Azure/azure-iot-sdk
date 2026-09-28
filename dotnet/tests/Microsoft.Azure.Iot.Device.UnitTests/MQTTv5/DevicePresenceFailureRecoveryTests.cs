// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.MQTTv5.Connection;
using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Mqtt;
using Xunit;

namespace Microsoft.Azure.Iot.Device.UnitTests.MQTTv5
{
    /// <summary>
    /// Tests for the failure paths of <c>ConnectionClient.AnnounceDevicePresenceAsync</c> during the initial connect.
    /// </summary>
    /// <remarks>
    /// Every failure in the device presence (birth) flow is expected to tear down the connection with
    /// <c>DisconnectAsync(desireReconnection: true)</c>. The <c>MqttConnectionManager</c> then reconnects on its own and
    /// runs the birth flow again, so a single transient failure is expected to eventually recover into a successful
    /// announcement.
    ///
    /// These tests exercise that recovery on the very first connect attempt: the birth flow runs fire-and-forget as soon
    /// as the broker accepts the CONNECT, and the connection layer now marks the connection as desired before that first
    /// attempt, so a failure there disconnects and reconnects just like it would on an already-established connection.
    /// Because the connection client's <c>ConnectAsync</c> does not complete until device presence has been announced
    /// successfully, awaiting it is enough to prove that the client recovered from the injected failure.
    /// </remarks>
    public class DevicePresenceFailureRecoveryTests
    {
        private static ConnectionContext GetMockConnectionContext()
        {
#pragma warning disable SYSLIB0026 // Type or member is obsolete (Mock certificate, don't need to load a real one using typical X509 certificate loader
            return new ConnectionContext()
            {
                AuthenticationProvider = new X509AuthenticationProvider(new System.Security.Cryptography.X509Certificates.X509Certificate2()),
                DeviceId = "someDeviceId",
                IotHubHostName = "someHostName",
                ConnectionProfile = Provisioning.Models.ConnectionProfile.MqttV5
            };
#pragma warning restore SYSLIB0026 // Type or member is obsolete
        }

        [Fact]
        public async Task AnnounceDevicePresenceRecoversWhenSubackIsMalformed()
        {
            // A SUBACK with no items is malformed, so the birth flow disconnects and lets the manager reconnect.
            await RunInitialConnectBirthFailureTestAsync(
                birthSubscribeInjector: (subscribe, attempt) => attempt == 1
                    ? new MqttSubscribeAck() { Items = new List<MqttSubscribeAckItem>() }
                    : null,
                birthPublishInjector: null,
                cancellationToken: TestContext.Current.CancellationToken);
        }

        [Fact]
        public async Task AnnounceDevicePresenceRecoversWhenSubackReasonCodeIsUnsuccessful()
        {
            // The birth SUBSCRIBE is requested at QoS 1, so anything other than GrantedQoS1 is a failed subscribe.
            await RunInitialConnectBirthFailureTestAsync(
                birthSubscribeInjector: (subscribe, attempt) => attempt == 1
                    ? new MqttSubscribeAck()
                    {
                        Items = new List<MqttSubscribeAckItem>()
                        {
                            new MqttSubscribeAckItem()
                            {
                                ReasonCode = MqttClientSubscribeReasonCode.GrantedQoS0,
                                TopicFilter = new(subscribe.TopicFilters.First().Topic, MqttQualityOfServiceLevel.AtLeastOnce),
                            }
                        }
                    }
                    : null,
                birthPublishInjector: null,
                cancellationToken: TestContext.Current.CancellationToken);
        }

        [Fact]
        public async Task AnnounceDevicePresenceRecoversWhenSubscribeThrows()
        {
            // An exception raised while subscribing to the devicebound topic is caught and treated as a failed birth.
            await RunInitialConnectBirthFailureTestAsync(
                birthSubscribeInjector: (subscribe, attempt) => attempt == 1
                    ? throw new Exception("mock exception thrown while subscribing to devicebound topic")
                    : null,
                birthPublishInjector: null,
                cancellationToken: TestContext.Current.CancellationToken);
        }

        [Fact]
        public async Task AnnounceDevicePresenceRecoversWhenBirthPublishThrows()
        {
            // An exception raised while publishing the birth message is caught and treated as a failed birth.
            await RunInitialConnectBirthFailureTestAsync(
                birthSubscribeInjector: null,
                birthPublishInjector: (publish, attempt) => attempt == 1
                    ? throw new Exception("mock exception thrown while publishing birth message")
                    : null,
                cancellationToken: TestContext.Current.CancellationToken);
        }

        [Fact]
        public async Task AnnounceDevicePresenceRecoversWhenBirthPubackIsUnsuccessful()
        {
            // A non-success PUBACK on the birth message means the announcement did not land, so the birth flow disconnects.
            await RunInitialConnectBirthFailureTestAsync(
                birthSubscribeInjector: null,
                birthPublishInjector: (publish, attempt) => attempt == 1
                    ? new MqttPublishAck() { ReasonCode = MqttPublishAckReasonCode.NoMatchingSubscribers }
                    : null,
                cancellationToken: TestContext.Current.CancellationToken);
        }

        /// <summary>
        /// Injects a single birth-flow failure into the very first connect attempt and asserts that the client
        /// disconnects, reconnects on its own, and eventually announces device presence successfully so that
        /// <see cref="ConnectionClient.ConnectAsync"/> completes.
        /// </summary>
        /// <param name="birthSubscribeInjector">
        /// Optional override for the birth SUBSCRIBE. Receives the subscribe request and the 1-based attempt number.
        /// Return a SUBACK (or throw) to fail that attempt, or return null to send a normal successful SUBACK.
        /// </param>
        /// <param name="birthPublishInjector">
        /// Optional override for the birth PUBLISH. Receives the birth publish and the 1-based attempt number. Return a
        /// PUBACK (or throw) to fail that attempt, or return null to send a normal successful PUBACK.
        /// </param>
        private static async Task RunInitialConnectBirthFailureTestAsync(
            Func<MqttSubscribe, int, MqttSubscribeAck?>? birthSubscribeInjector,
            Func<MqttPublish, int, MqttPublishAck?>? birthPublishInjector,
            CancellationToken cancellationToken)
        {
            MockMqttClient mockMqttClient = new(true);
            ConnectionClient connectionClient = new(new()
            {
                MqttClient = mockMqttClient
            });

            int connectCount = 0;
            int disconnectCount = 0;
            int birthSubscribeCount = 0;
            int birthPublishCount = 0;

            // Sessions never resume, so every connect's birth flow re-sends the presence SUBSCRIBE that the failure
            // cases target.
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
                // Only the birth flow subscribes in this test, so every subscribe here is a device presence subscribe.
                int attempt = Interlocked.Increment(ref birthSubscribeCount);
                MqttSubscribeAck? injected = birthSubscribeInjector?.Invoke(subscribe, attempt);
                return injected ?? MqttObjectHelpers.CreateSuccessfulSuback(subscribe);
            };

            mockMqttClient.OnPublishAttempt += async (publish) =>
            {
                // Only the birth message is published in this test, so every publish here is a device presence publish.
                if (publish.Topic.EndsWith("/srv/presence") && birthPublishInjector != null)
                {
                    int attempt = Interlocked.Increment(ref birthPublishCount);
                    MqttPublishAck? injected = birthPublishInjector.Invoke(publish, attempt);
                    if (injected != null)
                    {
                        return injected;
                    }
                }

                return new MqttPublishAck() { ReasonCode = MqttPublishAckReasonCode.Success };
            };

            // ConnectAsync only completes once the birth flow has successfully announced device presence, which requires
            // recovering from the injected failure on the first attempt. A timeout guards against a regression where the
            // failure is not retried (which would hang forever).
            await connectionClient.ConnectAsync(GetMockConnectionContext(), cancellationToken: cancellationToken)
                .WaitAsync(TimeSpan.FromSeconds(30), cancellationToken);

            // The birth flow's failure handling must have disconnected at least once...
            Assert.True(disconnectCount >= 1, $"Expected at least one disconnect from the failed birth flow, but saw {disconnectCount}.");

            // ...and the manager must have reconnected on its own for the initial connect to eventually succeed.
            Assert.True(connectCount >= 2, $"Expected at least two connects (the initial attempt plus a reconnect after the failed birth flow), but saw {connectCount}.");

            // A successful birth flow ends by publishing the birth message on the presence topic, so that must be the
            // last traffic on the wire once device presence has been announced.
            var lastTraffic = mockMqttClient.SentMqttTrafficInOrder.Last();
            Assert.NotNull(lastTraffic.Publish);
            Assert.EndsWith("/srv/presence", lastTraffic.Publish.Topic);
        }
    }
}
