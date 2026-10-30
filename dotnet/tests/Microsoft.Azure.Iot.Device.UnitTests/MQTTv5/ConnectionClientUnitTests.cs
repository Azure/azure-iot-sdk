// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Authentication;
using Microsoft.Azure.Iot.Device.MQTTv5.Connection;
using Microsoft.Azure.Iot.Device.MQTTv5.Telemetry;
using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Mqtt;
using Microsoft.Azure.Iot.Device.Retry;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using Xunit;
using MqttV5ConnectionClientOptions = Microsoft.Azure.Iot.Device.MQTTv5.Models.ConnectionClientOptions;
using Microsoft.Azure.Iot.Device.Unified.Models;

namespace Microsoft.Azure.Iot.Device.UnitTests.MQTTv5
{
    public class ConnectionClientUnitTests
    {
        private static ConnectionContext GetMockConnectionContext()
        {
#pragma warning disable SYSLIB0026 // Type or member is obsolete (Mock certificate, don't need to load a real one using typical X509 certificate loader
            return new ConnectionContext()
            {
                AuthenticationProvider = new X509AuthenticationProvider(new System.Security.Cryptography.X509Certificates.X509Certificate2(), null),
                DeviceId = "someDeviceId",
                IotHubHostName = "someHostName",
                ConnectionProfile = IotHubConnectionType.Mqttv5
            };
#pragma warning restore SYSLIB0026 // Type or member is obsolete
        }

        [Theory]
        [InlineData(false)]
        [InlineData(true)]
        public async Task PresenceAnnouncementAwaitsEveryCompletionHandlerEvenWhenOneThrows(bool useUnifiedClient)
        {
            using MockConnectionMqttClient mockMqttClient = new() { AutoRespondToBirth = true };
            ConnectionClientOptions options = new() { MqttClient = mockMqttClient };
            AbstractConnectionClient connectionClient = useUnifiedClient
                ? new Device.Unified.Connection.ConnectionClient(options)
                : new ConnectionClient(new() { MqttClient = mockMqttClient });
            TaskCompletionSource firstHandlerCanFinish = new(TaskCreationOptions.RunContinuationsAsynchronously);

            try
            {
                await connectionClient.ConnectToHubAsync(GetMockConnectionContext(), TestContext.Current.CancellationToken);
                InvalidOperationException failure = new("first handler failed");
                bool secondHandlerRan = false;
                connectionClient.DevicePresenceFlowCompletedAsync += async _ =>
                {
                    await firstHandlerCanFinish.Task;
                    throw failure;
                };
                connectionClient.DevicePresenceFlowCompletedAsync += _ =>
                {
                    secondHandlerRan = true;
                    return Task.CompletedTask;
                };

                Task announcement = connectionClient.HandleConnectedToHubAsync(new()
                {
                    ConnectAck = new() { ResultCode = MqttConnectReasonCode.Success },
                });
                Assert.False(announcement.IsCompleted);
                Assert.False(secondHandlerRan);
                firstHandlerCanFinish.SetResult();

                Assert.Same(failure, await Assert.ThrowsAsync<InvalidOperationException>(
                    () => announcement.WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken)));
                Assert.True(secondHandlerRan);
            }
            finally
            {
                firstHandlerCanFinish.TrySetResult();
                connectionClient.Dispose();
            }
        }

        // The password comes from the authentication provider, so this client's connect customization must not overwrite it.
        [Fact]
        public async Task ConnectSendsAuthenticationProviderPassword()
        {
            using MockConnectionMqttClient mockMqttClient = new() { AutoRespondToBirth = true };
            using ConnectionClient connectionClient = new(new() { MqttClient = mockMqttClient });

            byte[]? sentPassword = null;
            mockMqttClient.OnConnect = connect =>
            {
                sentPassword = connect.Password;
                return Task.FromResult(new MqttConnectAck() { ResultCode = MqttConnectReasonCode.Success });
            };

            await connectionClient.ConnectToHubAsync(
                new ConnectionContext()
                {
                    AuthenticationProvider = new SymmetricKeyAuthenticationProvider("someDeviceId", "0ne00000000", new byte[] { 1, 2, 3 }, null),
                    DeviceId = "someDeviceId",
                    IotHubHostName = "someHostName",
                    ConnectionProfile = IotHubConnectionType.Mqttv5,
                },
                cancellationToken: TestContext.Current.CancellationToken);

            Assert.NotNull(sentPassword);
            Assert.StartsWith("SharedAccessSignature ", System.Text.Encoding.UTF8.GetString(sentPassword));
        }

        [Theory]
        [InlineData(false)]
        [InlineData(true)]
        public async Task RejectedPrimaryCertificateFallsBackToSecondaryAndKeepsItForReconnect(bool useUnifiedClient)
        {
            using RSA key = RSA.Create(2048);
            CertificateRequest request = new("CN=someDeviceId", key, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);
            using X509Certificate2 primaryCertificate = request.CreateSelfSigned(DateTimeOffset.UtcNow.AddDays(-1), DateTimeOffset.UtcNow.AddDays(1));
            using X509Certificate2 secondaryCertificate = request.CreateSelfSigned(DateTimeOffset.UtcNow.AddDays(-1), DateTimeOffset.UtcNow.AddDays(1));
            using MockConnectionMqttClient mockMqttClient = new() { AutoRespondToBirth = true };
            AbstractConnectionClient connectionClient = useUnifiedClient
                ? new Device.Unified.Connection.ConnectionClient(new() { MqttClient = mockMqttClient })
                : new ConnectionClient(new() { MqttClient = mockMqttClient });
            using IDisposable connectionClientLifetime = (IDisposable)connectionClient;

            List<(X509Certificate2? Certificate, MqttProtocolVersion ProtocolVersion, string? Username)> connectAttempts = new();
            mockMqttClient.OnConnect = connect =>
            {
                lock (connectAttempts)
                {
                    connectAttempts.Add((connect.ClientCertificate, connect.ProtocolVersion, connect.Username));
                }

                return Task.FromResult(new MqttConnectAck()
                {
                    ResultCode = ReferenceEquals(connect.ClientCertificate, secondaryCertificate)
                        ? MqttConnectReasonCode.Success
                        : MqttConnectReasonCode.NotAuthorized,
                });
            };

            await connectionClient.ConnectToHubAsync(new()
            {
                AuthenticationProvider = new X509AuthenticationProvider(primaryCertificate, secondaryCertificate),
                DeviceId = "someDeviceId",
                IotHubHostName = "someHostName",
                ConnectionProfile = IotHubConnectionType.Mqttv5,
            }, TestContext.Current.CancellationToken).WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);

            TaskCompletionSource reconnected = new(TaskCreationOptions.RunContinuationsAsynchronously);
            connectionClient.DevicePresenceFlowCompletedAsync += _ =>
            {
                reconnected.TrySetResult();
                return Task.CompletedTask;
            };
            await mockMqttClient.SimulateServerDisconnectAsync(MqttDisconnectReason.ServerBusy)
                .WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);
            await reconnected.Task.WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);

            Assert.Equal(new[] { primaryCertificate, secondaryCertificate, secondaryCertificate },
                connectAttempts.Select(attempt => attempt.Certificate));
            Assert.All(connectAttempts, attempt => Assert.Equal(MqttProtocolVersion.V500, attempt.ProtocolVersion));
            Assert.All(connectAttempts, attempt => Assert.StartsWith("correlationId=", attempt.Username));
            Assert.Equal(3, connectAttempts.Select(attempt => attempt.Username).Distinct().Count());
            Assert.Equal(2, mockMqttClient.SentMqttTrafficInOrder.Count(traffic =>
                traffic.Publish?.Topic == "ih/someDeviceId/srv/presence"));
        }

        // Matches the C SDK: the MQTTv5 hub serves MQTT over WebSockets at "/mqtt" on port 443.
        [Fact]
        public async Task ConnectUsesMqttV5HubWebSocketUri()
        {
            using MockConnectionMqttClient mockMqttClient = new() { AutoRespondToBirth = true };
            using ConnectionClient connectionClient = new(new() { MqttClient = mockMqttClient });

            string? sentWebSocketUri = null;
            mockMqttClient.OnConnect = connect =>
            {
                sentWebSocketUri = connect.WebsocketUri;
                return Task.FromResult(new MqttConnectAck() { ResultCode = MqttConnectReasonCode.Success });
            };

            await connectionClient.ConnectToHubAsync(GetMockConnectionContext(), cancellationToken: TestContext.Current.CancellationToken);

            Assert.Equal("wss://someHostName:443/mqtt", sentWebSocketUri);
        }

        [Theory]
        [InlineData(false, false, 3600u)]
        [InlineData(true, false, 0u)]
        [InlineData(false, true, 3600u)]
        [InlineData(true, true, 0u)]
        [InlineData(false, false, 0u)]
        [InlineData(false, false, uint.MaxValue)]
        public async Task ReconnectUsesSnapshottedSessionSettings(bool useUnifiedClient, bool useCleanSession, uint sessionExpiryInterval)
        {
            using MockConnectionMqttClient mockMqttClient = new() { AutoRespondToBirth = true };
            MqttV5ConnectionClientOptions options = new()
            {
                MqttClient = mockMqttClient,
                UseCleanSession = useCleanSession,
                SessionExpiryInterval = sessionExpiryInterval,
                ConnectionRetryPolicy = new ExponentialBackoffRetryPolicy(uint.MaxValue, TimeSpan.Zero, useJitter: false),
            };
            ConnectionClientOptions unifiedOptions = new()
            {
                MqttClient = mockMqttClient,
                UseCleanSession = useCleanSession,
                ConnectionRetryPolicy = options.ConnectionRetryPolicy,
            };
            AbstractConnectionClient connectionClient = useUnifiedClient
                ? new Device.Unified.Connection.ConnectionClient(unifiedOptions)
                : new ConnectionClient(options);
            using IDisposable connectionClientLifetime = (IDisposable)connectionClient;

            options.UseCleanSession = !useCleanSession;
            options.SessionExpiryInterval = sessionExpiryInterval == 0 ? 3600u : 0u;
            unifiedOptions.UseCleanSession = !useCleanSession;

            List<(bool CleanStart, uint SessionExpiryInterval)> connectSettings = new();
            mockMqttClient.OnConnect = connect =>
            {
                lock (connectSettings)
                {
                    connectSettings.Add((connect.CleanStart, connect.SessionExpiryInterval));
                }

                return Task.FromResult(new MqttConnectAck()
                {
                    IsSessionPresent = !connect.CleanStart && connect.SessionExpiryInterval != 0,
                    ResultCode = MqttConnectReasonCode.Success,
                });
            };

            await connectionClient.ConnectToHubAsync(GetMockConnectionContext(), TestContext.Current.CancellationToken);

            TaskCompletionSource reconnected = new(TaskCreationOptions.RunContinuationsAsynchronously);
            connectionClient.DevicePresenceFlowCompletedAsync += _ =>
            {
                reconnected.TrySetResult();
                return Task.CompletedTask;
            };

            await mockMqttClient.SimulateServerDisconnectAsync(MqttDisconnectReason.ServerBusy)
                .WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);
            await reconnected.Task.WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);

            // The unified client does not expose a session expiry, so its MQTTv5 connections use the default.
            uint expectedExpiry = useUnifiedClient ? 3600u : sessionExpiryInterval;
            Assert.Equal(new[] { (useCleanSession, expectedExpiry), (useCleanSession, expectedExpiry) }, connectSettings);
            Assert.Equal(useCleanSession || expectedExpiry == 0 ? 2 : 1,
                mockMqttClient.SentMqttTrafficInOrder.Count(traffic => traffic.Subscribe != null));
            Assert.Equal(2, mockMqttClient.SentMqttTrafficInOrder.Count(traffic => traffic.Publish?.Topic.EndsWith("/srv/presence", StringComparison.Ordinal) == true));
        }

        [Theory]
        [InlineData(false)]
        [InlineData(true)]
        public async Task FailedConnectAttemptForcesResubscribeOnResumedSession(bool useUnifiedClient)
        {
            using MockConnectionMqttClient mockMqttClient = new() { AutoRespondToBirth = true };
            MqttV5ConnectionClientOptions options = new()
            {
                MqttClient = mockMqttClient,
                ConnectionRetryPolicy = new ExponentialBackoffRetryPolicy(uint.MaxValue, TimeSpan.Zero, useJitter: false),
            };
            AbstractConnectionClient connectionClient = useUnifiedClient
                ? new Device.Unified.Connection.ConnectionClient(new()
                {
                    MqttClient = mockMqttClient,
                    ConnectionRetryPolicy = options.ConnectionRetryPolicy,
                })
                : new ConnectionClient(options);
            using IDisposable connectionClientLifetime = (IDisposable)connectionClient;

            await connectionClient.ConnectToHubAsync(GetMockConnectionContext(), TestContext.Current.CancellationToken);

            List<(bool CleanStart, uint SessionExpiryInterval)> connectSettings = new();
            mockMqttClient.OnConnect = connect =>
            {
                lock (connectSettings)
                {
                    connectSettings.Add((connect.CleanStart, connect.SessionExpiryInterval));
                    if (connectSettings.Count == 1)
                    {
                        throw new TimeoutException("mock CONNACK lost");
                    }
                }

                return Task.FromResult(new MqttConnectAck() { IsSessionPresent = !connect.CleanStart, ResultCode = MqttConnectReasonCode.Success });
            };

            TaskCompletionSource reconnected = new(TaskCreationOptions.RunContinuationsAsynchronously);
            connectionClient.DevicePresenceFlowCompletedAsync += _ =>
            {
                reconnected.TrySetResult();
                return Task.CompletedTask;
            };

            await mockMqttClient.SimulateServerDisconnectAsync(MqttDisconnectReason.ServerBusy)
                .WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);
            await reconnected.Task.WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);

            // Both attempts ask to resume with the default expiry, but the lost CONNACK may have left an empty session
            // behind, so the session the final attempt resumes is subscribed again.
            Assert.Equal(new[] { (false, 3600u), (false, 3600u) }, connectSettings);
            Assert.Equal(2, mockMqttClient.SentMqttTrafficInOrder.Count(traffic => traffic.Subscribe != null));
        }

        [Theory]
        [InlineData(false)]
        [InlineData(true)]
        public async Task ConnectionLostBeforeBirthAckResumesSessionWithoutResubscribing(bool useUnifiedClient)
        {
            using MockConnectionMqttClient mockMqttClient = new();
            MqttV5ConnectionClientOptions options = new()
            {
                MqttClient = mockMqttClient,
                ConnectionRetryPolicy = new ExponentialBackoffRetryPolicy(uint.MaxValue, TimeSpan.Zero, useJitter: false),
            };
            AbstractConnectionClient connectionClient = useUnifiedClient
                ? new Device.Unified.Connection.ConnectionClient(new()
                {
                    MqttClient = mockMqttClient,
                    ConnectionRetryPolicy = options.ConnectionRetryPolicy,
                })
                : new ConnectionClient(options);
            using IDisposable connectionClientLifetime = (IDisposable)connectionClient;

            List<bool> cleanStartFlags = new();
            mockMqttClient.OnConnect = connect =>
            {
                lock (cleanStartFlags)
                {
                    cleanStartFlags.Add(connect.CleanStart);
                }

                return Task.FromResult(new MqttConnectAck() { IsSessionPresent = !connect.CleanStart, ResultCode = MqttConnectReasonCode.Success });
            };
            TaskCompletionSource birthPublished = new(TaskCreationOptions.RunContinuationsAsynchronously);
            mockMqttClient.OnPublish = _ =>
            {
                birthPublished.TrySetResult();
                return Task.FromResult(new MqttPublishAck() { ReasonCode = MqttPublishAckReasonCode.Success });
            };

            Task connecting = connectionClient.ConnectToHubAsync(GetMockConnectionContext(), TestContext.Current.CancellationToken);
            await birthPublished.Task.WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);
            Assert.False(connecting.IsCompleted);

            mockMqttClient.AutoRespondToBirth = true;
            await mockMqttClient.SimulateServerDisconnectAsync(MqttDisconnectReason.ServerBusy)
                .WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);
            await connecting.WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);

            // The SUBACK was observed on the session before the connection dropped, so the resumed session still holds
            // the subscriptions and only the birth is sent again.
            Assert.Equal(new[] { false, false }, cleanStartFlags);
            Assert.Equal(1, mockMqttClient.SentMqttTrafficInOrder.Count(traffic => traffic.Subscribe != null));
            Assert.Equal(2, mockMqttClient.SentMqttTrafficInOrder.Count(traffic => traffic.Publish?.Topic.EndsWith("/srv/presence", StringComparison.Ordinal) == true));
        }

        // A publish attempt that fails only after the reconnect and its birth flow have already completed must still be
        // retried, rather than waiting for a connection-restored wakeup that already happened.
        [Fact]
        public async Task PublishRetriesWhenPresenceCompletesBeforeFailedAttemptReturns()
        {
            using MockConnectionMqttClient mockMqttClient = new() { AutoRespondToBirth = true };
            using ConnectionClient connectionClient = new(new()
            {
                MqttClient = mockMqttClient,
                ConnectionRetryPolicy = new ExponentialBackoffRetryPolicy(uint.MaxValue, TimeSpan.Zero, useJitter: false),
            });
            TelemetryClient telemetryClient = new(connectionClient);

            await connectionClient.ConnectToHubAsync(GetMockConnectionContext(), cancellationToken: TestContext.Current.CancellationToken);

            TaskCompletionSource firstPublishAttemptStarted = new(TaskCreationOptions.RunContinuationsAsynchronously);
            TaskCompletionSource releaseFirstPublishAttempt = new(TaskCreationOptions.RunContinuationsAsynchronously);
            TaskCompletionSource reconnectPresenceCompleted = new(TaskCreationOptions.RunContinuationsAsynchronously);
            int presenceCompletionCount = 0;
            connectionClient.DevicePresenceFlowCompletedAsync += args =>
            {
                Interlocked.Increment(ref presenceCompletionCount);
                reconnectPresenceCompleted.TrySetResult();
                return Task.CompletedTask;
            };

            int telemetryAttemptCount = 0;
            mockMqttClient.OnPublish = async publish =>
            {
                if (publish.Topic.EndsWith("/srv/telemetry", StringComparison.Ordinal)
                    && Interlocked.Increment(ref telemetryAttemptCount) == 1)
                {
                    firstPublishAttemptStarted.TrySetResult();
                    await releaseFirstPublishAttempt.Task;
                    throw new MqttClientNotConnectedException("mock client not connected exception");
                }

                return new MqttPublishAck() { ReasonCode = MqttPublishAckReasonCode.Success };
            };

            Task pendingTelemetry = telemetryClient.SendTelemetryAsync(new Models.Telemetry.DeviceToCloudTelemetry(), TestContext.Current.CancellationToken);

            try
            {
                await firstPublishAttemptStarted.Task.WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);

                // The reconnect and its birth flow must finish while the original publish attempt is still pending.
                await mockMqttClient.SimulateServerDisconnectAsync(MqttDisconnectReason.ImplementationSpecificError, new Exception("mock exception"))
                    .WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);
                await reconnectPresenceCompleted.Task.WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);

                releaseFirstPublishAttempt.TrySetResult();
                await pendingTelemetry.WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);

                Assert.Equal(1, presenceCompletionCount);
                Assert.Equal(2, telemetryAttemptCount);
            }
            finally
            {
                releaseFirstPublishAttempt.TrySetResult();
            }
        }

        [Theory]
        [InlineData(true)]
        [InlineData(false)]
        public async Task ConnectionClientReannouncesBirthBeforeContinuingPublish(bool isSessionResumed)
        {
            using MockConnectionMqttClient mockMqttClient = new() { AutoRespondToBirth = true };
            ConnectionClient connectionClient = new(new()
            {
                MqttClient = mockMqttClient
            });

            TelemetryClient telemetryClient = new(connectionClient);

            await connectionClient.ConnectToHubAsync(GetMockConnectionContext(), cancellationToken: TestContext.Current.CancellationToken);

            // Setup mock MQTT layer to lose connection when telemetry client sends a publish for the first time (subsequent retries will work normally)
            int retryCount = 0;
            mockMqttClient.OnConnect = async (connect) =>
            {
                return new MqttConnectAck() { IsSessionPresent = isSessionResumed, ResultCode = MqttConnectReasonCode.Success };
            };
            mockMqttClient.OnPublish = async (publish) =>
            {
                if (retryCount == 0)
                {
                    retryCount++;
                    _ = mockMqttClient.SimulateServerDisconnectAsync(MqttDisconnectReason.ImplementationSpecificError, new IOException("mock connection reset"));
                    throw new IOException("mock connection reset");
                }

                return new MqttPublishAck() { ReasonCode = MqttPublishAckReasonCode.Success };
            };

            await telemetryClient.SendTelemetryAsync(new Models.Telemetry.DeviceToCloudTelemetry(), TestContext.Current.CancellationToken);

            if (isSessionResumed)
            {
                // With session resumed, the birth flow doesn't need to send the subscribe on the presence topic again
                Assert.Equal(4, mockMqttClient.SentMqttTrafficInOrder.Count);
            }
            else
            {
                Assert.Equal(5, mockMqttClient.SentMqttTrafficInOrder.Count);
            }

            var lastTraffic = mockMqttClient.SentMqttTrafficInOrder.Last();

            // Verify that the telemetry sent by the telemetry client is the last piece of traffic "sent" in this test. It must be preceded by the intial device presence flow traffic and by the reconnection device presence flow traffic
            Assert.NotNull(lastTraffic.Publish);
            Assert.StartsWith("ih/", lastTraffic.Publish.Topic);
            Assert.EndsWith("/srv/telemetry", lastTraffic.Publish.Topic);
        }

        [Theory]
        [InlineData(true)]
        [InlineData(false)]
        public async Task ConnectionClientReannouncesBirthBeforeContinuingSubscribe(bool isSessionResumed)
        {
            using MockConnectionMqttClient mockMqttClient = new() { AutoRespondToBirth = true };
            ConnectionClient connectionClient = new(new()
            {
                MqttClient = mockMqttClient
            });

            await connectionClient.ConnectToHubAsync(GetMockConnectionContext(), cancellationToken: TestContext.Current.CancellationToken);

            // Setup mock MQTT layer to lose connection when telemetry client sends a publish for the first time (subsequent retries will work normally)
            int retryCount = 0;
            mockMqttClient.OnConnect = async (connect) =>
            {
                return new MqttConnectAck() { IsSessionPresent = isSessionResumed, ResultCode = MqttConnectReasonCode.Success };
            };

            string expectedTopicString = Guid.NewGuid().ToString();

            mockMqttClient.OnSubscribe = async (subscribe) =>
            {
                if (retryCount == 0 && subscribe.TopicFilters.FirstOrDefault()!.Topic.Equals(expectedTopicString))
                {
                    retryCount++;
                    _ = mockMqttClient.SimulateServerDisconnectAsync(MqttDisconnectReason.ImplementationSpecificError, new IOException("mock connection reset"));
                    throw new IOException("mock connection reset");
                }

                return MqttObjectHelpers.CreateSuccessfulSuback(subscribe);
            };

            MqttSubscribe featureClientLevelSubscribeRequest = new(expectedTopicString, MqttQualityOfServiceLevel.AtLeastOnce);
            await connectionClient.SubscribeAsync(featureClientLevelSubscribeRequest, TestContext.Current.CancellationToken);

            if (isSessionResumed)
            {
                // With session resumed, the birth flow doesn't need to send the subscribe on the presence topic again
                Assert.Equal(4, mockMqttClient.SentMqttTrafficInOrder.Count);
            }
            else
            {
                Assert.Equal(5, mockMqttClient.SentMqttTrafficInOrder.Count);
            }

            var lastTraffic = mockMqttClient.SentMqttTrafficInOrder.Last();

            // Verify that the subscribe sent directly from this test is the last piece of traffic "sent" in this test. It must be preceded by the intial device presence flow traffic and by the reconnection device presence flow traffic
            Assert.NotNull(lastTraffic.Subscribe);
            Assert.Single(lastTraffic.Subscribe.TopicFilters);
            Assert.Equal(expectedTopicString, lastTraffic.Subscribe.TopicFilters.FirstOrDefault()!.Topic);
        }

        [Theory]
        [InlineData(true)]
        [InlineData(false)]
        public async Task ConnectionClientReannouncesBirthBeforeContinuingUnsubscribe(bool isSessionResumed)
        {
            using MockConnectionMqttClient mockMqttClient = new() { AutoRespondToBirth = true };
            ConnectionClient connectionClient = new(new()
            {
                MqttClient = mockMqttClient
            });

            await connectionClient.ConnectToHubAsync(GetMockConnectionContext(), cancellationToken: TestContext.Current.CancellationToken);

            // Setup mock MQTT layer to lose connection when telemetry client sends a publish for the first time (subsequent retries will work normally)
            int retryCount = 0;
            mockMqttClient.OnConnect = async (connect) =>
            {
                return new MqttConnectAck() { IsSessionPresent = isSessionResumed, ResultCode = MqttConnectReasonCode.Success };
            };

            string expectedTopicString = Guid.NewGuid().ToString();

            mockMqttClient.OnUnsubscribe = async (unsubscribe) =>
            {
                if (retryCount == 0 && unsubscribe.TopicFilters.FirstOrDefault()!.Equals(expectedTopicString))
                {
                    retryCount++;
                    _ = mockMqttClient.SimulateServerDisconnectAsync(MqttDisconnectReason.ImplementationSpecificError, new IOException("mock connection reset"));
                    throw new IOException("mock connection reset");
                }

                return MqttObjectHelpers.CreateSuccessfulUnsuback(unsubscribe);
            };

            MqttUnsubscribe featureClientLevelUnsubscribeRequest = new(expectedTopicString);
            await connectionClient.UnsubscribeAsync(featureClientLevelUnsubscribeRequest, TestContext.Current.CancellationToken);

            if (isSessionResumed)
            {
                // With session resumed, the birth flow doesn't need to send the subscribe on the presence topic again
                Assert.Equal(4, mockMqttClient.SentMqttTrafficInOrder.Count);
            }
            else
            {
                Assert.Equal(5, mockMqttClient.SentMqttTrafficInOrder.Count);
            }

            var lastTraffic = mockMqttClient.SentMqttTrafficInOrder.Last();

            // Verify that the subscribe sent directly from this test is the last piece of traffic "sent" in this test. It must be preceded by the intial device presence flow traffic and by the reconnection device presence flow traffic
            Assert.NotNull(lastTraffic.Unsubscribe);
            Assert.Single(lastTraffic.Unsubscribe.TopicFilters);
            Assert.Equal(expectedTopicString, lastTraffic.Unsubscribe.TopicFilters.FirstOrDefault());
        }

        // The auth webhook only accepts a correlationId of 32 lowercase hex characters, and it must be the same nonce
        // that rides the birth as its Correlation Data.
        [Theory]
        [InlineData(false)]
        [InlineData(true)]
        public async Task ConnectNonceIsLowercaseHexOfBirthCorrelationDataAndBirthIsProtobuf(bool useUnifiedClient)
        {
            using MockConnectionMqttClient mockMqttClient = new() { AutoRespondToBirth = true };
            AbstractConnectionClient connectionClient = useUnifiedClient
                ? new Device.Unified.Connection.ConnectionClient(new ConnectionClientOptions() { MqttClient = mockMqttClient })
                : new ConnectionClient(new() { MqttClient = mockMqttClient });
            using IDisposable connectionClientLifetime = (IDisposable)connectionClient;

            string? sentUsername = null;
            mockMqttClient.OnConnect = connect =>
            {
                sentUsername = connect.Username;
                return Task.FromResult(new MqttConnectAck() { ResultCode = MqttConnectReasonCode.Success });
            };

            await connectionClient.ConnectToHubAsync(GetMockConnectionContext(), TestContext.Current.CancellationToken)
                .WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);

            MqttPublish birth = Assert.Single(mockMqttClient.SentMqttTrafficInOrder
                .Select(traffic => traffic.Publish).OfType<MqttPublish>());
            Assert.Equal("application/protobuf", birth.ContentType);
            Assert.NotNull(birth.CorrelationData);
            Assert.Equal(16, birth.CorrelationData.Length);
            Assert.NotNull(sentUsername);
            Assert.StartsWith($"correlationId={Convert.ToHexStringLower(birth.CorrelationData)}&clientVersion=", sentUsername);
        }

        [Theory]
        [InlineData(0, 0.0, 5.0)]
        [InlineData(0, 1.0, 10.0)]
        [InlineData(1, 0.0, 6.0)]
        [InlineData(2, 0.5, 10.5)]
        [InlineData(3, 0.0, 10.0)]
        [InlineData(50, 1.0, 15.0)]
        public void BirthAckTimeoutFollowsSpecSchedule(int consecutiveTimeouts, double jitterFraction, double expectedMinutes)
        {
            Assert.Equal(TimeSpan.FromMinutes(expectedMinutes), ConnectionClient.GetBirthAckTimeout(consecutiveTimeouts, jitterFraction));
        }

        // Each consecutive birth-ack timeout moves to the next row of the schedule, and a received birth-ack starts the
        // next connection back at the first row.
        [Fact]
        public async Task BirthAckTimeoutEscalatesOnConsecutiveTimeoutsAndResetsAfterBirthAck()
        {
            using MockConnectionMqttClient mockMqttClient = new();
            using ConnectionClient connectionClient = new(new()
            {
                MqttClient = mockMqttClient,
                ConnectionRetryPolicy = new ExponentialBackoffRetryPolicy(uint.MaxValue, TimeSpan.Zero, useJitter: false),
            });

            List<int> requestedTimeouts = new();
            connectionClient.BirthAckTimeoutProvider = consecutiveTimeouts =>
            {
                lock (requestedTimeouts)
                {
                    requestedTimeouts.Add(consecutiveTimeouts);
                }

                return TimeSpan.FromMilliseconds(100);
            };

            // Leave the first two births unanswered so that they time out, then answer every birth after that.
            int birthCount = 0;
            mockMqttClient.OnPublish = publish =>
            {
                if (publish.Topic == "ih/someDeviceId/srv/presence" && Interlocked.Increment(ref birthCount) > 2)
                {
                    MqttPublish birthAck = new()
                    {
                        Topic = "ih/someDeviceId/dev/presence",
                        Payload = Array.Empty<byte>(),
                        CorrelationData = publish.CorrelationData,
                    };
                    birthAck.UserProperties.Add(new("type", System.Text.Encoding.UTF8.GetBytes("birth-ack:1")));
                    _ = Task.Run(() => mockMqttClient.SimulatePublishReceivedAsync(birthAck));
                }

                return Task.FromResult(new MqttPublishAck() { ReasonCode = MqttPublishAckReasonCode.Success });
            };

            await connectionClient.ConnectToHubAsync(GetMockConnectionContext(), TestContext.Current.CancellationToken)
                .WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);

            TaskCompletionSource reconnected = new(TaskCreationOptions.RunContinuationsAsynchronously);
            connectionClient.DevicePresenceFlowCompletedAsync += _ =>
            {
                reconnected.TrySetResult();
                return Task.CompletedTask;
            };
            await mockMqttClient.SimulateServerDisconnectAsync(MqttDisconnectReason.ServerBusy)
                .WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);
            await reconnected.Task.WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);

            lock (requestedTimeouts)
            {
                Assert.Equal(new[] { 0, 1, 2, 0 }, requestedTimeouts);
            }
        }

        // A birth-ack timeout puts the session's subscriptions in doubt, so the next connection re-subscribes even
        // though the broker resumes the session. Once a SUBACK is seen again, later resumed sessions skip the SUBSCRIBE.
        [Fact]
        public async Task BirthAckTimeoutForcesResubscribeOnResumedSession()
        {
            using MockConnectionMqttClient mockMqttClient = new();
            using ConnectionClient connectionClient = new(new()
            {
                MqttClient = mockMqttClient,
                ConnectionRetryPolicy = new ExponentialBackoffRetryPolicy(uint.MaxValue, TimeSpan.Zero, useJitter: false),
            });
            connectionClient.BirthAckTimeoutProvider = _ => TimeSpan.FromMilliseconds(100);

            List<bool> cleanStartFlags = new();
            mockMqttClient.OnConnect = connect =>
            {
                lock (cleanStartFlags)
                {
                    cleanStartFlags.Add(connect.CleanStart);
                }

                return Task.FromResult(new MqttConnectAck() { IsSessionPresent = !connect.CleanStart, ResultCode = MqttConnectReasonCode.Success });
            };

            int birthCount = 0;
            mockMqttClient.OnPublish = publish =>
            {
                if (publish.Topic == "ih/someDeviceId/srv/presence" && Interlocked.Increment(ref birthCount) > 1)
                {
                    _ = Task.Run(() => mockMqttClient.SimulatePublishReceivedAsync(CreateBirthAck("birth-ack:1", publish.CorrelationData, Array.Empty<byte>())));
                }

                return Task.FromResult(new MqttPublishAck() { ReasonCode = MqttPublishAckReasonCode.Success });
            };

            await connectionClient.ConnectToHubAsync(GetMockConnectionContext(), TestContext.Current.CancellationToken)
                .WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);

            TaskCompletionSource reconnected = new(TaskCreationOptions.RunContinuationsAsynchronously);
            connectionClient.DevicePresenceFlowCompletedAsync += _ =>
            {
                reconnected.TrySetResult();
                return Task.CompletedTask;
            };
            await mockMqttClient.SimulateServerDisconnectAsync(MqttDisconnectReason.ServerBusy)
                .WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);
            await reconnected.Task.WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);

            Assert.Equal(new[] { false, false, false }, cleanStartFlags);
            Assert.Equal(2, mockMqttClient.SentMqttTrafficInOrder.Count(traffic => traffic.Subscribe != null));
            Assert.Equal(3, Volatile.Read(ref birthCount));
        }

        // Only birth-ack:1 with this connection's nonce and a decodable payload completes the presence flow. Other
        // schema versions may not be readable as version 1, and a malformed payload would drop recovery state, so
        // those are ignored and the birth-ack timeout reconnects.
        [Theory]
        [InlineData("birth-ack:1", true, false, true)]
        [InlineData("birth-ack:1", true, true, false)]
        [InlineData("birth-ack:1", false, false, false)]
        [InlineData("birth-ack", true, false, false)]
        [InlineData("birth-ack:2", true, false, false)]
        [InlineData("birth-acknowledged:1", true, false, false)]
        [InlineData("birth:1", true, false, false)]
        public async Task OnlyWellFormedBirthAckVersion1ForThisNonceCompletesPresence(string type, bool matchNonce, bool malformedPayload, bool expectAccepted)
        {
            using MockConnectionMqttClient mockMqttClient = new();
            using ConnectionClient connectionClient = new(new()
            {
                MqttClient = mockMqttClient,
                ConnectionRetryPolicy = new ExponentialBackoffRetryPolicy(uint.MaxValue, TimeSpan.Zero, useJitter: false),
            });
            connectionClient.BirthAckTimeoutProvider = _ => TimeSpan.FromMilliseconds(100);

            // The first birth gets the birth-ack under test. If it is not accepted, the flow times out and the next
            // birth gets a well-formed birth-ack.
            int birthCount = 0;
            mockMqttClient.OnPublish = publish =>
            {
                if (publish.Topic == "ih/someDeviceId/srv/presence")
                {
                    MqttPublish birthAck = Interlocked.Increment(ref birthCount) == 1
                        ? CreateBirthAck(
                            type,
                            matchNonce ? publish.CorrelationData : Guid.NewGuid().ToByteArray(bigEndian: true),
                            malformedPayload ? new byte[] { 0xFF } : Array.Empty<byte>())
                        : CreateBirthAck("birth-ack:1", publish.CorrelationData, Array.Empty<byte>());
                    _ = Task.Run(() => mockMqttClient.SimulatePublishReceivedAsync(birthAck));
                }

                return Task.FromResult(new MqttPublishAck() { ReasonCode = MqttPublishAckReasonCode.Success });
            };

            await connectionClient.ConnectToHubAsync(GetMockConnectionContext(), TestContext.Current.CancellationToken)
                .WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);

            Assert.Equal(expectAccepted ? 1 : 2, Volatile.Read(ref birthCount));
        }

        // A connect the application starts after closing is a fresh CONNECT cycle, so it uses the first row of the
        // schedule even if the presence flows of the earlier connect timed out without a birth-ack ever arriving.
        [Theory]
        [InlineData(false)]
        [InlineData(true)]
        public async Task ConnectToHubResetsBirthAckTimeoutSchedule(bool useUnifiedClient)
        {
            using MockConnectionMqttClient mockMqttClient = new();
            IRetryPolicy retryPolicy = new ExponentialBackoffRetryPolicy(uint.MaxValue, TimeSpan.Zero, useJitter: false);
            ConnectionClient mqttv5Client;
            AbstractConnectionClient connectionClient;
            if (useUnifiedClient)
            {
                var unifiedClient = new Device.Unified.Connection.ConnectionClient(new() { MqttClient = mockMqttClient, ConnectionRetryPolicy = retryPolicy });
                mqttv5Client = GetNestedMqttV5Client(unifiedClient);
                connectionClient = unifiedClient;
            }
            else
            {
                mqttv5Client = new ConnectionClient(new() { MqttClient = mockMqttClient, ConnectionRetryPolicy = retryPolicy });
                connectionClient = mqttv5Client;
            }

            using IDisposable connectionClientLifetime = (IDisposable)connectionClient;

            // Leave births unanswered so that two time out, then abandon that connect while the third is pending.
            using CancellationTokenSource abandonFirstConnect = CancellationTokenSource.CreateLinkedTokenSource(TestContext.Current.CancellationToken);
            List<int> requestedTimeouts = new();
            mqttv5Client.BirthAckTimeoutProvider = consecutiveTimeouts =>
            {
                lock (requestedTimeouts)
                {
                    requestedTimeouts.Add(consecutiveTimeouts);
                }

                if (consecutiveTimeouts == 2)
                {
                    abandonFirstConnect.Cancel();
                    return Timeout.InfiniteTimeSpan;
                }

                return TimeSpan.FromMilliseconds(100);
            };

            bool answerBirths = false;
            mockMqttClient.OnPublish = publish =>
            {
                if (publish.Topic == "ih/someDeviceId/srv/presence" && Volatile.Read(ref answerBirths))
                {
                    _ = Task.Run(() => mockMqttClient.SimulatePublishReceivedAsync(CreateBirthAck("birth-ack:1", publish.CorrelationData, Array.Empty<byte>())));
                }

                return Task.FromResult(new MqttPublishAck() { ReasonCode = MqttPublishAckReasonCode.Success });
            };

            await Assert.ThrowsAnyAsync<OperationCanceledException>(
                () => connectionClient.ConnectToHubAsync(GetMockConnectionContext(), abandonFirstConnect.Token)
                    .WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken));

            Volatile.Write(ref answerBirths, true);
            lock (requestedTimeouts)
            {
                Assert.Equal(new[] { 0, 1, 2 }, requestedTimeouts);
            }

            await connectionClient.DisconnectAsync(TestContext.Current.CancellationToken);

            await connectionClient.ConnectToHubAsync(GetMockConnectionContext(), TestContext.Current.CancellationToken)
                .WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);

            lock (requestedTimeouts)
            {
                Assert.Equal(new[] { 0, 1, 2, 0 }, requestedTimeouts);
            }
        }

        private static ConnectionClient GetNestedMqttV5Client(Device.Unified.Connection.ConnectionClient unifiedClient)
        {
            return (ConnectionClient)typeof(Device.Unified.Connection.ConnectionClient)
                .GetField("_mqttv5ConnectionClient", System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.NonPublic)!
                .GetValue(unifiedClient)!;
        }

        private static MqttPublish CreateBirthAck(string type, byte[]? correlationData, byte[] payload)
        {
            MqttPublish birthAck = new()
            {
                Topic = "ih/someDeviceId/dev/presence",
                Payload = payload,
                CorrelationData = correlationData,
            };
            birthAck.UserProperties.Add(new("type", System.Text.Encoding.UTF8.GetBytes(type)));
            return birthAck;
        }
    }
}
