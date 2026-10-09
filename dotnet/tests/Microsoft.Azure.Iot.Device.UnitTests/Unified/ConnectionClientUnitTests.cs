// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Unified.Connection;
using Microsoft.Azure.Iot.Device.Unified.Telemetry;
using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Models.CertificateManagement;
using Microsoft.Azure.Iot.Device.Mqtt;
using Xunit;

namespace Microsoft.Azure.Iot.Device.UnitTests.Unified
{
    public class ConnectionClientUnitTests
    {
        private static ConnectionContext GetMockConnectionContext(bool isMQTTv5)
        {
#pragma warning disable SYSLIB0026 // Type or member is obsolete (Mock certificate, don't need to load a real one using typical X509 certificate loader
            return new ConnectionContext()
            {
                AuthenticationProvider = new X509AuthenticationProvider(new System.Security.Cryptography.X509Certificates.X509Certificate2()),
                DeviceId = "someDeviceId",
                IotHubHostName = "someHostName",
                ConnectionProfile = isMQTTv5 ? Provisioning.Models.ConnectionProfile.MqttV5 : Provisioning.Models.ConnectionProfile.Classic,
            };
#pragma warning restore SYSLIB0026 // Type or member is obsolete
        }

        private const string AcceptedPayload = "{\"correlationId\":\"someCorrelationId\",\"operationExpires\":\"2030-01-01T00:00:00Z\"}";

        private static readonly TimeSpan s_certSigningTimeout = TimeSpan.FromSeconds(10);

        private static async Task<(ConnectionClient Client, MockMqttClient Mqtt, CertificateSigningOperation Operation, string RequestId)> StartCertificateSigningAsync(
            Func<IReadOnlyList<string>, Task<X509AuthenticationProvider>>? completeCallback = null)
        {
            MockMqttClient mockMqttClient = new(false);
            ConnectionClient connectionClient = new(new() { MqttClient = mockMqttClient });
            connectionClient.HandleCertificateSigningCompleteAsync = completeCallback;
            await connectionClient.ConnectToHubAsync(GetMockConnectionContext(false), cancellationToken: TestContext.Current.CancellationToken);

            IotHubCertificateSigningRequest request = new("someDeviceId", "c29tZWNzcg==");
            CertificateSigningOperation operation = await connectionClient.SendCertificateSigningRequestAsync(request, TestContext.Current.CancellationToken);
            return (connectionClient, mockMqttClient, operation, request.RequestId);
        }

        // Delivers a response from IoT hub. Exceptions thrown by the handler are swallowed, as the MQTT layer would do, so tests can assert on the operation's tasks instead.
        private static async Task DeliverCertificateSigningResponseAsync(MockMqttClient mqtt, string status, string requestId, string payload)
        {
            try
            {
                await mqtt.SimulateNewMessageAsync(new MqttPublish()
                {
                    Topic = $"$iothub/credentials/res/{status}/?$rid={requestId}",
                    Payload = System.Text.Encoding.UTF8.GetBytes(payload),
                });
            }
            catch (Exception)
            {
            }
        }

        private static async Task<CertificateSigningRequestFailedException> AssertFailsAsync(Task task)
        {
            return await Assert.ThrowsAsync<CertificateSigningRequestFailedException>(
                () => task.WaitAsync(s_certSigningTimeout, TestContext.Current.CancellationToken));
        }

        [Theory]
        [InlineData("400", 400040)]
        [InlineData("409", 409005)]
        [InlineData("429", 429002)]
        [InlineData("500", 500001)]
        public async Task CertificateSigningErrorBeforeAcceptancePropagatesToBothTasks(string status, int errorCode)
        {
            var (client, mqtt, operation, requestId) = await StartCertificateSigningAsync();
            using (client)
            {
                string payload = $"{{\"errorCode\":{errorCode},\"message\":\"some message\",\"trackingId\":\"tid\",\"retryAfter\":5}}";
                await DeliverCertificateSigningResponseAsync(mqtt, status, requestId, payload);

                var acceptedEx = await AssertFailsAsync(operation.Accepted);
                var completedEx = await AssertFailsAsync(operation.Completed);

                Assert.Equal(errorCode, acceptedEx.Error.ErrorCode);
                Assert.Equal("some message", acceptedEx.Error.Message);
                Assert.Equal(5, acceptedEx.Error.RetryAfterSeconds);
                Assert.Equal(errorCode, completedEx.Error.ErrorCode);
            }
        }

        [Fact]
        public async Task CertificateSigningErrorAfterAcceptanceFailsOnlyCompletedTask()
        {
            var (client, mqtt, operation, requestId) = await StartCertificateSigningAsync();
            using (client)
            {
                await DeliverCertificateSigningResponseAsync(mqtt, "202", requestId, AcceptedPayload);
                await operation.Accepted.WaitAsync(s_certSigningTimeout, TestContext.Current.CancellationToken);

                await DeliverCertificateSigningResponseAsync(mqtt, "500", requestId, "{\"errorCode\":500001,\"message\":\"issuance failed\"}");

                var ex = await AssertFailsAsync(operation.Completed);
                Assert.Equal(500001, ex.Error.ErrorCode);

                // Acceptance already succeeded, so a later failure must not rewrite it.
                Assert.True(operation.Accepted.IsCompletedSuccessfully);
            }
        }

        [Fact]
        public async Task CertificateSigningErrorWithMinimalPayloadStillFailsBothTasks()
        {
            var (client, mqtt, operation, requestId) = await StartCertificateSigningAsync();
            using (client)
            {
                await DeliverCertificateSigningResponseAsync(mqtt, "400", requestId, "{}");

                var acceptedEx = await AssertFailsAsync(operation.Accepted);
                var completedEx = await AssertFailsAsync(operation.Completed);
                Assert.Equal(0, acceptedEx.Error.ErrorCode);
                Assert.Same(acceptedEx, completedEx);
            }
        }

        [Theory]
        [InlineData("400", "not json")]
        [InlineData("400", "")]
        [InlineData("400", "null")]
        [InlineData("202", "not json")]
        [InlineData("200", "not json")]
        public async Task CertificateSigningUnreadableResponseFailsBothTasksInsteadOfHanging(string status, string payload)
        {
            var (client, mqtt, operation, requestId) = await StartCertificateSigningAsync(certs => Task.FromResult(GetMockConnectionContext(false).AuthenticationProvider));
            using (client)
            {
                await DeliverCertificateSigningResponseAsync(mqtt, status, requestId, payload);

                await AssertFailsAsync(operation.Accepted);
                await AssertFailsAsync(operation.Completed);
            }
        }

        [Fact]
        public async Task CertificateSigningCompleteCallbackThrowingFailsCompletedTask()
        {
            InvalidOperationException callbackException = new("callback failed");
            var (client, mqtt, operation, requestId) = await StartCertificateSigningAsync(certs => throw callbackException);
            using (client)
            {
                await DeliverCertificateSigningResponseAsync(mqtt, "202", requestId, AcceptedPayload);
                await operation.Accepted.WaitAsync(s_certSigningTimeout, TestContext.Current.CancellationToken);

                await DeliverCertificateSigningResponseAsync(mqtt, "200", requestId, "{\"certificates\":[\"cert1\"],\"correlationId\":\"someCorrelationId\"}");

                var ex = await Assert.ThrowsAsync<InvalidOperationException>(
                    () => operation.Completed.WaitAsync(s_certSigningTimeout, TestContext.Current.CancellationToken));
                Assert.Same(callbackException, ex);
            }
        }

        [Fact]
        public async Task CertificateSigningErrorForOneRequestDoesNotAffectAnother()
        {
            var (client, mqtt, failingOperation, failingId) = await StartCertificateSigningAsync();
            using (client)
            {
                IotHubCertificateSigningRequest otherRequest = new("someDeviceId", "c29tZWNzcg==");
                CertificateSigningOperation otherOperation = await client.SendCertificateSigningRequestAsync(otherRequest, TestContext.Current.CancellationToken);

                await DeliverCertificateSigningResponseAsync(mqtt, "400", failingId, "{\"errorCode\":400040}");

                await AssertFailsAsync(failingOperation.Accepted);
                await AssertFailsAsync(failingOperation.Completed);
                Assert.False(otherOperation.Accepted.IsCompleted);
                Assert.False(otherOperation.Completed.IsCompleted);
            }
        }

        [Fact]
        public async Task CertificateSigningResponseForUnknownRequestIsIgnored()
        {
            var (client, mqtt, operation, requestId) = await StartCertificateSigningAsync();
            using (client)
            {
                await DeliverCertificateSigningResponseAsync(mqtt, "400", "unknown-request-id", "{\"errorCode\":400040}");

                Assert.False(operation.Accepted.IsCompleted);
                Assert.False(operation.Completed.IsCompleted);
            }
        }

        [Fact]
        public async Task CertificateSigningRequestPublishFailureThrowsToCaller()
        {
            MockMqttClient mockMqttClient = new(false);
            using ConnectionClient connectionClient = new(new() { MqttClient = mockMqttClient });
            await connectionClient.ConnectToHubAsync(GetMockConnectionContext(false), cancellationToken: TestContext.Current.CancellationToken);

            mockMqttClient.OnPublishAttempt += _ => Task.FromResult(new MqttPublishAck() { ReasonCode = MqttPublishAckReasonCode.NotAuthorized });

            await Assert.ThrowsAnyAsync<Exception>(() => connectionClient.SendCertificateSigningRequestAsync(
                new IotHubCertificateSigningRequest("someDeviceId", "c29tZWNzcg=="),
                TestContext.Current.CancellationToken));
        }

        [Fact]
        public async Task ProvisionAndConnectUsesSeededConnectionContext()
        {
            MockMqttClient mockMqttClient = new(false);
            ConnectionContext connectionContext = GetMockConnectionContext(false);
            using ConnectionClient connectionClient = new(new()
            {
                MqttClient = mockMqttClient,
            }, connectionContext);

            // The seeded assignment connects, so this device connects directly using it rather than provisioning.
            ConnectionContext result = await connectionClient.ProvisionAndConnectAsync(
                new ProvisioningSettings("someIdScope"),
                connectionContext.AuthenticationProvider,
                cancellationToken: TestContext.Current.CancellationToken);

            Assert.Same(connectionContext, result);
            Assert.Same(connectionContext, connectionClient.GetCurrentConnectionContext());
        }

        [Theory]
        [InlineData(true)]
        [InlineData(false)]
        public async Task ConnectionClientReannouncesBirthBeforeContinuingPublish(bool isSessionResumed)
        {
            MockMqttClient mockMqttClient = new(true);
            ConnectionClient connectionClient = new(new()
            {
                MqttClient = mockMqttClient
            });

            TelemetryClient telemetryClient = new(connectionClient);

            await connectionClient.ConnectToHubAsync(GetMockConnectionContext(false), cancellationToken: TestContext.Current.CancellationToken);

            // Setup mock MQTT layer to lose connection when telemetry client sends a publish for the first time (subsequent retries will work normally)
            int retryCount = 0;
            mockMqttClient.OnConnectAttempt += async (connect) =>
            {
                return new MqttConnectAck() { IsSessionPresent = isSessionResumed, ResultCode = MqttConnectReasonCode.Success };
            };
            mockMqttClient.OnPublishAttempt += async (publish) =>
            {
                if (retryCount == 0)
                {
                    _ = mockMqttClient.SimulateServerInitiatedDisconnectAsync(new Exception("mock exception"));
                    retryCount++;
                    throw new MqttClientNotConnectedException("mock client not connected exception");
                }

                return new MqttPublishAck() { ReasonCode = MqttPublishAckReasonCode.Success };
            };

            await telemetryClient.SendTelemetryAsync(new Models.Telemetry.DeviceToCloudTelemetry(), TestContext.Current.CancellationToken);

            if (isSessionResumed)
            {
                // With session resumed, the reconnect flow doesn't need to send the subscribe on the direct methods/telemetry/twin topics
                Assert.Equal(2, mockMqttClient.SentMqttTrafficInOrder.Count);
            }
            else
            {
                Assert.Equal(3, mockMqttClient.SentMqttTrafficInOrder.Count);
            }

            var lastTraffic = mockMqttClient.SentMqttTrafficInOrder.Last();

            // Verify that the telemetry sent by the telemetry client is the last piece of traffic "sent" in this test. It must be preceded by the intial device presence flow traffic and by the reconnection device presence flow traffic
            Assert.NotNull(lastTraffic.Publish);
            Assert.StartsWith("devices/", lastTraffic.Publish.Topic);
            Assert.EndsWith("/messages/events/", lastTraffic.Publish.Topic);
        }

        [Theory]
        [InlineData(true)]
        [InlineData(false)]
        public async Task ConnectionClientReannouncesBirthBeforeContinuingSubscribe(bool isSessionResumed)
        {
            MockMqttClient mockMqttClient = new(true);
            ConnectionClient connectionClient = new(new()
            {
                MqttClient = mockMqttClient
            });

            await connectionClient.ConnectToHubAsync(GetMockConnectionContext(false), cancellationToken: TestContext.Current.CancellationToken);

            // Setup mock MQTT layer to lose connection when telemetry client sends a publish for the first time (subsequent retries will work normally)
            int retryCount = 0;
            mockMqttClient.OnConnectAttempt += async (connect) =>
            {
                return new MqttConnectAck() { IsSessionPresent = isSessionResumed, ResultCode = MqttConnectReasonCode.Success };
            };

            string expectedTopicString = Guid.NewGuid().ToString();

            mockMqttClient.OnSubscribeAttempt += async (subscribe) =>
            {
                if (retryCount == 0 && subscribe.TopicFilters.FirstOrDefault()!.Topic.Equals(expectedTopicString))
                {
                    _ = mockMqttClient.SimulateServerInitiatedDisconnectAsync(new Exception("mock exception"));
                    retryCount++;
                    throw new MqttClientNotConnectedException("mock client not connected exception");
                }

                return MqttObjectHelpers.CreateSuccessfulSuback(subscribe);
            };

            MqttSubscribe featureClientLevelSubscribeRequest = new(expectedTopicString, MqttQualityOfServiceLevel.AtLeastOnce);
            await connectionClient.SubscribeAsync(featureClientLevelSubscribeRequest, TestContext.Current.CancellationToken);

            if (isSessionResumed)
            {
                // With session resumed, the reconnect flow doesn't need to send the subscribe on the direct methods/telemetry/twin topics
                Assert.Equal(2, mockMqttClient.SentMqttTrafficInOrder.Count);

            }
            else
            {
                Assert.Equal(3, mockMqttClient.SentMqttTrafficInOrder.Count);
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
            MockMqttClient mockMqttClient = new(true);
            ConnectionClient connectionClient = new(new()
            {
                MqttClient = mockMqttClient
            });

            await connectionClient.ConnectToHubAsync(GetMockConnectionContext(false), cancellationToken: TestContext.Current.CancellationToken);

            // Setup mock MQTT layer to lose connection when telemetry client sends a publish for the first time (subsequent retries will work normally)
            int retryCount = 0;
            mockMqttClient.OnConnectAttempt += async (connect) =>
            {
                return new MqttConnectAck() { IsSessionPresent = isSessionResumed, ResultCode = MqttConnectReasonCode.Success };
            };

            string expectedTopicString = Guid.NewGuid().ToString();

            mockMqttClient.OnUnsubscribeAttempt += async (unsubscribe) =>
            {
                if (retryCount == 0 && unsubscribe.TopicFilters.FirstOrDefault()!.Equals(expectedTopicString))
                {
                    _ = mockMqttClient.SimulateServerInitiatedDisconnectAsync(new Exception("mock exception"));
                    retryCount++;
                    throw new MqttClientNotConnectedException("mock client not connected exception");
                }

                return MqttObjectHelpers.CreateSuccessfulUnsuback(unsubscribe);
            };

            MqttUnsubscribe featureClientLevelUnsubscribeRequest = new(expectedTopicString);
            await connectionClient.UnsubscribeAsync(featureClientLevelUnsubscribeRequest, TestContext.Current.CancellationToken);

            if (isSessionResumed)
            {
                // With session resumed, the reconnect flow doesn't need to send the subscribe on the direct methods/telemetry/twin topics
                Assert.Equal(2, mockMqttClient.SentMqttTrafficInOrder.Count);

            }
            else
            {
                Assert.Equal(3, mockMqttClient.SentMqttTrafficInOrder.Count);
            }

            var lastTraffic = mockMqttClient.SentMqttTrafficInOrder.Last();

            // Verify that the subscribe sent directly from this test is the last piece of traffic "sent" in this test. It must be preceded by the intial device presence flow traffic and by the reconnection device presence flow traffic
            Assert.NotNull(lastTraffic.Unsubscribe);
            Assert.Single(lastTraffic.Unsubscribe.TopicFilters);
            Assert.Equal(expectedTopicString, lastTraffic.Unsubscribe.TopicFilters.FirstOrDefault()!);
        }
    }
}
