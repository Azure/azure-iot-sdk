// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Unified.Connection;
using Microsoft.Azure.Iot.Device.Unified.Telemetry;
using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Mqtt;
using Xunit;
using Microsoft.Azure.Iot.Device.Authentication;
using Microsoft.Azure.Iot.Device.Retry;
using Microsoft.Azure.Iot.Device.Exceptions;
using Microsoft.Azure.Iot.Device.Models.CertificateManagement;
using System.Reflection;
using System.Text;
using System.Text.Json;

namespace Microsoft.Azure.Iot.Device.UnitTests.Unified
{
    public class ConnectionClientUnitTests
    {
        private const string AcceptedResponse = """{"correlationId":"correlation-id","operationExpires":"2026-10-20T00:00:00Z"}""";
        private const string CompletedResponse = """{"certificates":["issued-certificate"],"correlationId":"correlation-id"}""";

        [Fact]
        public async Task MqttV5ForwardingStubAwaitsEveryPublishHandlerEvenWhenOneThrows()
        {
            MockFeatureConnectionClient connection = new();
            using Stub stub = new(connection);
            TaskCompletionSource firstHandlerCanFinish = new(TaskCreationOptions.RunContinuationsAsynchronously);
            InvalidOperationException failure = new("first handler failed");
            bool secondHandlerRan = false;
            Func<MqttPublishReceivedEventArgs, Task> first = async _ =>
            {
                await firstHandlerCanFinish.Task;
                throw failure;
            };
            Func<MqttPublishReceivedEventArgs, Task> second = _ =>
            {
                secondHandlerRan = true;
                return Task.CompletedTask;
            };
            stub.PublishReceivedAsync += first + second;

            Task delivery = connection.SimulateReceiveAsync(new MqttPublish() { Topic = "topic" });
            try
            {
                Assert.False(delivery.IsCompleted);
                Assert.False(secondHandlerRan);
            }
            finally
            {
                firstHandlerCanFinish.TrySetResult();
            }

            Assert.Same(failure, await Assert.ThrowsAsync<InvalidOperationException>(() => delivery));
            Assert.True(secondHandlerRan);
        }

        [Theory]
        [InlineData(false)]
        [InlineData(true)]
        public async Task CancellationCancelsAndReleasesOnlyTheMatchingCertificateSigningOperation(bool acceptedFirst)
        {
            using MockConnectionMqttClient mockMqttClient = new();
            ConnectionContext context = GetMockConnectionContext(false);
            using ConnectionClient client = new(new() { MqttClient = mockMqttClient });
            await client.ConnectToHubAsync(context, TestContext.Current.CancellationToken);
            using CancellationTokenSource cancellation = CancellationTokenSource.CreateLinkedTokenSource(TestContext.Current.CancellationToken);
            IotHubCertificateSigningRequest request = new(context.DeviceId, "csr");
            CertificateSigningOperation operation = await client.SendCertificateSigningRequestAsync(request, cancellation.Token);
            IotHubCertificateSigningRequest otherRequest = new(context.DeviceId, "other-csr");
            CertificateSigningOperation otherOperation = await client.SendCertificateSigningRequestAsync(otherRequest, TestContext.Current.CancellationToken);
            if (acceptedFirst)
            {
                await ReceiveCertificateSigningResponseAsync(client, request.RequestId, "202", AcceptedResponse);
            }

            cancellation.Cancel();

            Assert.True(operation.Completed.IsCanceled);
            OperationCanceledException completedCancellation = await Assert.ThrowsAnyAsync<OperationCanceledException>(() => operation.Completed);
            Assert.Equal(cancellation.Token, completedCancellation.CancellationToken);
            if (acceptedFirst)
            {
                Assert.True(operation.Accepted.IsCompletedSuccessfully);
            }
            else
            {
                Assert.True(operation.Accepted.IsCanceled);
                OperationCanceledException acceptedCancellation = await Assert.ThrowsAnyAsync<OperationCanceledException>(() => operation.Accepted);
                Assert.Equal(cancellation.Token, acceptedCancellation.CancellationToken);
            }

            Assert.False(otherOperation.Accepted.IsCompleted);
            Assert.False(otherOperation.Completed.IsCompleted);
            await ReceiveCertificateSigningResponseAsync(client, request.RequestId, "202", AcceptedResponse);
            await ReceiveCertificateSigningResponseAsync(client, request.RequestId, "200", CompletedResponse);
            Assert.Null(context.IssuedClientCertificates);
            await ReceiveCertificateSigningResponseAsync(client, otherRequest.RequestId, "202", AcceptedResponse);
            await ReceiveCertificateSigningResponseAsync(client, otherRequest.RequestId, "200", CompletedResponse);
            Assert.True(otherOperation.Completed.IsCompletedSuccessfully);
            await client.SendCertificateSigningRequestAsync(request, TestContext.Current.CancellationToken);
        }

        [Theory]
        [InlineData("200", CompletedResponse)]
        [InlineData("400", "{}")]
        public async Task CancellationAfterCertificateSigningResponseDoesNotAffectReusedRequest(string status, string payload)
        {
            using MockConnectionMqttClient mockMqttClient = new();
            using ConnectionClient client = new(new() { MqttClient = mockMqttClient });
            await client.ConnectToHubAsync(GetMockConnectionContext(false), TestContext.Current.CancellationToken);
            using CancellationTokenSource cancellation = CancellationTokenSource.CreateLinkedTokenSource(TestContext.Current.CancellationToken);
            IotHubCertificateSigningRequest request = new("someDeviceId", "csr");
            CertificateSigningOperation operation = await client.SendCertificateSigningRequestAsync(request, cancellation.Token);
            await ReceiveCertificateSigningResponseAsync(client, request.RequestId, "202", AcceptedResponse);
            await ReceiveCertificateSigningResponseAsync(client, request.RequestId, status, payload);
            if (status == "200")
            {
                Assert.True(operation.Completed.IsCompletedSuccessfully);
            }
            else
            {
                await Assert.ThrowsAsync<CertificateSigningRequestFailedException>(() => operation.Completed);
            }

            CertificateSigningOperation replacement = await client.SendCertificateSigningRequestAsync(request, TestContext.Current.CancellationToken);
            cancellation.Cancel();

            Assert.True(operation.Accepted.IsCompletedSuccessfully);
            Assert.False(operation.Completed.IsCanceled);
            Assert.False(replacement.Accepted.IsCompleted);
            Assert.False(replacement.Completed.IsCompleted);
            await ReceiveCertificateSigningResponseAsync(client, request.RequestId, "202", AcceptedResponse);
            await ReceiveCertificateSigningResponseAsync(client, request.RequestId, "200", CompletedResponse);
            Assert.True(replacement.Completed.IsCompletedSuccessfully);
        }

        [Fact]
        public async Task AlreadyCanceledCertificateSigningRequestIsNotSentOrRetained()
        {
            using MockConnectionMqttClient mockMqttClient = new();
            using ConnectionClient client = new(new() { MqttClient = mockMqttClient });
            await client.ConnectToHubAsync(GetMockConnectionContext(false), TestContext.Current.CancellationToken);
            using CancellationTokenSource cancellation = CancellationTokenSource.CreateLinkedTokenSource(TestContext.Current.CancellationToken);
            IotHubCertificateSigningRequest request = new("someDeviceId", "csr");
            int sentCount = mockMqttClient.SentMqttTrafficInOrder.Count;
            cancellation.Cancel();

            OperationCanceledException exception = await Assert.ThrowsAnyAsync<OperationCanceledException>(
                () => client.SendCertificateSigningRequestAsync(request, cancellation.Token));

            Assert.Equal(cancellation.Token, exception.CancellationToken);
            Assert.Equal(sentCount, mockMqttClient.SentMqttTrafficInOrder.Count);
            await client.SendCertificateSigningRequestAsync(request, TestContext.Current.CancellationToken);
        }

        [Theory]
        [InlineData(MqttDisconnectReason.NormalDisconnection, true)]
        [InlineData(MqttDisconnectReason.ImplementationSpecificError, false)]
        [InlineData(MqttDisconnectReason.NotAuthorized, false)]
        public async Task DisconnectCancelsAndReleasesAllPendingCertificateSigningOperations(MqttDisconnectReason reason, bool clientInitiated)
        {
            using MockConnectionMqttClient mockMqttClient = new();
            ConnectionContext context = GetMockConnectionContext(false);
            using ConnectionClient client = new(new() { MqttClient = mockMqttClient });
            await client.ConnectToHubAsync(context, TestContext.Current.CancellationToken);
            IotHubCertificateSigningRequest unacceptedRequest = new(context.DeviceId, "csr");
            IotHubCertificateSigningRequest acceptedRequest = new(context.DeviceId, "csr");
            CertificateSigningOperation unacceptedOperation = await client.SendCertificateSigningRequestAsync(unacceptedRequest, TestContext.Current.CancellationToken);
            CertificateSigningOperation acceptedOperation = await client.SendCertificateSigningRequestAsync(acceptedRequest, TestContext.Current.CancellationToken);
            await ReceiveCertificateSigningResponseAsync(client, acceptedRequest.RequestId, "202", AcceptedResponse);

            if (clientInitiated)
            {
                await client.DisconnectAsync(TestContext.Current.CancellationToken);
            }
            else
            {
                await mockMqttClient.SimulateServerDisconnectAsync(reason);
            }

            Assert.True(unacceptedOperation.Accepted.IsCanceled);
            Assert.True(unacceptedOperation.Completed.IsCanceled);
            Assert.True(acceptedOperation.Accepted.IsCompletedSuccessfully);
            Assert.True(acceptedOperation.Completed.IsCanceled);

            if (clientInitiated || reason == MqttDisconnectReason.NotAuthorized)
            {
                await client.ConnectToHubAsync(context, TestContext.Current.CancellationToken);
            }
            await ReceiveCertificateSigningResponseAsync(client, unacceptedRequest.RequestId, "202", AcceptedResponse);
            await ReceiveCertificateSigningResponseAsync(client, acceptedRequest.RequestId, "200", CompletedResponse);
            Assert.True(unacceptedOperation.Accepted.IsCanceled);
            Assert.True(acceptedOperation.Completed.IsCanceled);
            Assert.Null(context.IssuedClientCertificates);
            await client.SendCertificateSigningRequestAsync(unacceptedRequest, TestContext.Current.CancellationToken);
            await client.SendCertificateSigningRequestAsync(acceptedRequest, TestContext.Current.CancellationToken);
        }

        [Fact]
        public async Task DisconnectCancelsCertificateSigningWithoutWaitingForReconnection()
        {
            using MockConnectionMqttClient mockMqttClient = new();
            using ConnectionClient client = new(new() { MqttClient = mockMqttClient });
            await client.ConnectToHubAsync(GetMockConnectionContext(false), TestContext.Current.CancellationToken);
            CertificateSigningOperation operation = await client.SendCertificateSigningRequestAsync(
                new("someDeviceId", "csr"), TestContext.Current.CancellationToken);
            TaskCompletionSource<MqttConnectAck> reconnect = new(TaskCreationOptions.RunContinuationsAsynchronously);
            TaskCompletionSource presenceCompleted = new(TaskCreationOptions.RunContinuationsAsynchronously);
            mockMqttClient.OnConnect = _ => reconnect.Task;
            client.DevicePresenceFlowCompletedAsync += _ =>
            {
                presenceCompleted.TrySetResult();
                return Task.CompletedTask;
            };

            try
            {
                await mockMqttClient.SimulateServerDisconnectAsync(MqttDisconnectReason.ImplementationSpecificError)
                    .WaitAsync(TimeSpan.FromSeconds(1), TestContext.Current.CancellationToken);

                Assert.False(mockMqttClient.IsConnected());
                Assert.True(operation.Accepted.IsCanceled);
                Assert.True(operation.Completed.IsCanceled);
            }
            finally
            {
                reconnect.TrySetResult(new() { ResultCode = MqttConnectReasonCode.Success });
                await presenceCompleted.Task.WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);
            }
        }

        [Theory]
        [InlineData(false, false)]
        [InlineData(false, true)]
        [InlineData(true, false)]
        [InlineData(true, true)]
        public async Task DisposeCancelsPendingCertificateSigningOperations(bool useDisposeBool, bool acceptedFirst)
        {
            using MockConnectionMqttClient mockMqttClient = new();
            ConnectionClient client = new(new() { MqttClient = mockMqttClient });
            await client.ConnectToHubAsync(GetMockConnectionContext(false), TestContext.Current.CancellationToken);
            IotHubCertificateSigningRequest request = new("someDeviceId", "csr");
            CertificateSigningOperation operation = await client.SendCertificateSigningRequestAsync(
                request, TestContext.Current.CancellationToken);
            if (acceptedFirst)
            {
                await ReceiveCertificateSigningResponseAsync(client, request.RequestId, "202", AcceptedResponse);
            }

            if (useDisposeBool)
            {
                client.Dispose(true);
            }
            else
            {
                client.Dispose();
            }

            Assert.Equal(acceptedFirst, operation.Accepted.IsCompletedSuccessfully);
            Assert.True(operation.Completed.IsCanceled);
            if (!acceptedFirst)
            {
                Assert.True(operation.Accepted.IsCanceled);
            }
        }

        [Theory]
        [InlineData(400, 400037, "CertificateSigningRequestInvalid", false)]
        [InlineData(400, 400037, "CertificateSigningRequestInvalid", true)]
        [InlineData(400, 400037, "CertificateSigningRequestSubjectMismatch", true)]
        [InlineData(409, 409005, "Conflict", false)]
        [InlineData(429, 429002, "Throttled", false)]
        [InlineData(429, 429003, "Throttled", true)]
        [InlineData(500, 500000, "InternalServerError", true)]
        public async Task CertificateSigningServiceErrorsFaultPendingTasks(int status, int errorCode, string credentialError, bool acceptedFirst)
        {
            using MockConnectionMqttClient mockMqttClient = new();
            ConnectionContext context = GetMockConnectionContext(false);
            using ConnectionClient client = new(new() { MqttClient = mockMqttClient });
            await client.ConnectToHubAsync(context, TestContext.Current.CancellationToken);
            IotHubCertificateSigningRequest request = new(context.DeviceId, "csr");
            CertificateSigningOperation operation = await client.SendCertificateSigningRequestAsync(request, TestContext.Current.CancellationToken);
            if (acceptedFirst)
            {
                await ReceiveCertificateSigningResponseAsync(client, request.RequestId, "202", AcceptedResponse);
                Assert.True(operation.Accepted.IsCompletedSuccessfully);
                Assert.False(operation.Completed.IsCompleted);
            }

            string payload = $$"""
                {
                    "errorCode": {{errorCode}},
                    "message": "Unable to complete the certificate request.",
                    "trackingId": "tracking-id",
                    "timestampUtc": "2026-10-04T07:59:52.467247529Z",
                    "retryAfter": 10,
                    "info": {
                        "credentialError": "{{credentialError}}",
                        "credentialMessage": "Certificate request rejected.",
                        "correlationId": "correlation-id"
                    },
                    "unknownField": true
                }
                """;
            await ReceiveCertificateSigningResponseAsync(client, request.RequestId, status.ToString(), payload);

            CertificateSigningRequestFailedException failure = await Assert.ThrowsAsync<CertificateSigningRequestFailedException>(
                () => operation.Completed.WaitAsync(TimeSpan.FromSeconds(1), TestContext.Current.CancellationToken));
            Assert.Equal(request.RequestId, failure.RequestId);
            Assert.Equal(errorCode, failure.Error.ErrorCode);
            Assert.Equal(credentialError, failure.Error.Info.CredentialError);
            Assert.Equal("Certificate request rejected.", failure.Error.Info.CredentialMessage);
            Assert.Equal("correlation-id", failure.Error.Info.CorrelationId);
            Assert.Equal("tracking-id", failure.Error.TrackingId);
            Assert.Equal(10, failure.Error.RetryAfterSeconds);
            if (acceptedFirst)
            {
                Assert.True(operation.Accepted.IsCompletedSuccessfully);
            }
            else
            {
                Assert.Same(failure, await Assert.ThrowsAsync<CertificateSigningRequestFailedException>(() => operation.Accepted));
            }

            Assert.Null(context.IssuedClientCertificates);
            Assert.True(mockMqttClient.IsConnected());
            await client.SendCertificateSigningRequestAsync(request, TestContext.Current.CancellationToken);
        }

        [Theory]
        [InlineData("{}")]
        [InlineData("""{"errorCode":400037}""")]
        [InlineData("""{"info":{"credentialError":"CertificateSigningRequestInvalid"},"extra":{"value":true}}""")]
        public async Task CertificateSigningErrorsAllowMissingOptionalAndUnknownFields(string payload)
        {
            using MockConnectionMqttClient mockMqttClient = new();
            using ConnectionClient client = new(new() { MqttClient = mockMqttClient });
            await client.ConnectToHubAsync(GetMockConnectionContext(false), TestContext.Current.CancellationToken);
            IotHubCertificateSigningRequest request = new("someDeviceId", "csr");
            CertificateSigningOperation operation = await client.SendCertificateSigningRequestAsync(request, TestContext.Current.CancellationToken);

            await ReceiveCertificateSigningResponseAsync(client, request.RequestId, "400", payload);

            CertificateSigningRequestFailedException failure = await Assert.ThrowsAsync<CertificateSigningRequestFailedException>(
                () => operation.Completed.WaitAsync(TimeSpan.FromSeconds(1), TestContext.Current.CancellationToken));
            Assert.Equal(request.RequestId, failure.RequestId);
            Assert.Same(failure, await Assert.ThrowsAsync<CertificateSigningRequestFailedException>(() => operation.Accepted));
        }

        [Theory]
        [InlineData("202", "{", false)]
        [InlineData("200", "{", true)]
        [InlineData("400", "{", true)]
        [InlineData("409", "{", false)]
        [InlineData("429", "{", true)]
        [InlineData("500", "{", false)]
        [InlineData("202", "null", false)]
        [InlineData("200", "null", true)]
        [InlineData("400", "null", false)]
        [InlineData("202", "[]", false)]
        [InlineData("200", "{}", true)]
        [InlineData("202", "{}", false)]
        [InlineData("400", """{"errorCode":"invalid"}""", false)]
        [InlineData("202", """{"correlationId":null,"operationExpires":"2026-10-20T00:00:00Z"}""", false)]
        [InlineData("200", """{"certificates":null,"correlationId":"correlation-id"}""", true)]
        public async Task InvalidCertificateSigningResponsesFaultOperationWithDiagnostics(string status, string payload, bool acceptedFirst)
        {
            using MockConnectionMqttClient mockMqttClient = new();
            ConnectionContext context = GetMockConnectionContext(false);
            using ConnectionClient client = new(new() { MqttClient = mockMqttClient });
            await client.ConnectToHubAsync(context, TestContext.Current.CancellationToken);
            IotHubCertificateSigningRequest request = new(context.DeviceId, "csr");
            CertificateSigningOperation operation = await client.SendCertificateSigningRequestAsync(request, TestContext.Current.CancellationToken);
            if (acceptedFirst)
            {
                await ReceiveCertificateSigningResponseAsync(client, request.RequestId, "202", AcceptedResponse);
            }

            await ReceiveCertificateSigningResponseAsync(client, request.RequestId, status, payload);

            DeviceException failure = await Assert.ThrowsAsync<DeviceException>(
                () => operation.Completed.WaitAsync(TimeSpan.FromSeconds(1), TestContext.Current.CancellationToken));
            Assert.IsType<JsonException>(failure.InnerException);
            Assert.Contains(status, failure.Message);
            Assert.Contains(request.RequestId, failure.Message);
            Assert.Contains(payload, failure.Message);
            Assert.True(failure.IsContained);
            if (acceptedFirst)
            {
                Assert.True(operation.Accepted.IsCompletedSuccessfully);
            }
            else
            {
                Assert.Same(failure, await Assert.ThrowsAsync<DeviceException>(() => operation.Accepted));
            }

            Assert.Null(context.IssuedClientCertificates);
            Assert.True(mockMqttClient.IsConnected());
            await client.SendCertificateSigningRequestAsync(request, TestContext.Current.CancellationToken);
        }

        [Fact]
        public async Task CertificateSigningCompletionReleasesRequestAndIgnoresLateResponses()
        {
            using MockConnectionMqttClient mockMqttClient = new();
            ConnectionContext context = GetMockConnectionContext(false);
            using ConnectionClient client = new(new() { MqttClient = mockMqttClient });
            await client.ConnectToHubAsync(context, TestContext.Current.CancellationToken);
            IotHubCertificateSigningRequest request = new(context.DeviceId, "csr");
            CertificateSigningOperation operation = await client.SendCertificateSigningRequestAsync(request, TestContext.Current.CancellationToken);

            await ReceiveCertificateSigningResponseAsync(client, request.RequestId, "202", AcceptedResponse);
            await Assert.ThrowsAsync<InvalidOperationException>(() => client.SendCertificateSigningRequestAsync(request, TestContext.Current.CancellationToken));
            await ReceiveCertificateSigningResponseAsync(client, request.RequestId, "200", CompletedResponse);

            CertificateSigningResponse response = await operation.Completed.WaitAsync(TimeSpan.FromSeconds(1), TestContext.Current.CancellationToken);
            Assert.Same(response.Certificates, context.IssuedClientCertificates);
            await ReceiveCertificateSigningResponseAsync(client, request.RequestId, "200",
                """{"certificates":["late-certificate"],"correlationId":"late-response"}""");
            Assert.Same(response.Certificates, context.IssuedClientCertificates);
            await client.SendCertificateSigningRequestAsync(request, TestContext.Current.CancellationToken);
        }

        [Theory]
        [InlineData(true)]
        [InlineData(false)]
        public async Task CertificateSigningSendFailureReleasesRequest(bool subscribeFails)
        {
            using MockConnectionMqttClient mockMqttClient = new();
            using ConnectionClient client = new(new() { MqttClient = mockMqttClient });
            await client.ConnectToHubAsync(GetMockConnectionContext(false), TestContext.Current.CancellationToken);
            IotHubCertificateSigningRequest request = new("someDeviceId", "csr");
            if (subscribeFails)
            {
                mockMqttClient.OnSubscribe = _ => throw new InvalidOperationException("Subscribe failed.");
            }
            else
            {
                mockMqttClient.OnPublish = _ => throw new InvalidOperationException("Publish failed.");
            }

            await Assert.ThrowsAsync<DeviceException>(() => client.SendCertificateSigningRequestAsync(request, TestContext.Current.CancellationToken));
            mockMqttClient.OnSubscribe = null;
            mockMqttClient.OnPublish = null;
            await client.SendCertificateSigningRequestAsync(request, TestContext.Current.CancellationToken);
        }

        [Fact]
        public async Task CertificateSigningAcknowledgmentFailureFaultsPendingOperation()
        {
            using MockConnectionMqttClient mockMqttClient = new();
            using ConnectionClient client = new(new() { MqttClient = mockMqttClient });
            await client.ConnectToHubAsync(GetMockConnectionContext(false), TestContext.Current.CancellationToken);
            IotHubCertificateSigningRequest request = new("someDeviceId", "csr");
            CertificateSigningOperation operation = await client.SendCertificateSigningRequestAsync(request, TestContext.Current.CancellationToken);

            await ReceiveCertificateSigningResponseAsync(client, request.RequestId, "202", AcceptedResponse, acknowledgeFails: true);

            Assert.True(operation.Accepted.IsCompletedSuccessfully);
            DeviceException failure = await Assert.ThrowsAsync<DeviceException>(
                () => operation.Completed.WaitAsync(TimeSpan.FromSeconds(1), TestContext.Current.CancellationToken));
            Assert.IsType<InvalidOperationException>(failure.InnerException);
            await client.SendCertificateSigningRequestAsync(request, TestContext.Current.CancellationToken);
        }

        [Theory]
        [InlineData("400/extra", "unknown-request")]
        [InlineData("400", "unknown-request")]
        public async Task UnmatchedCertificateSigningResponsesAreAcknowledgedWithoutFaultingPendingOperations(string status, string requestId)
        {
            using MockConnectionMqttClient mockMqttClient = new();
            using ConnectionClient client = new(new() { MqttClient = mockMqttClient });
            await client.ConnectToHubAsync(GetMockConnectionContext(false), TestContext.Current.CancellationToken);
            IotHubCertificateSigningRequest request = new("someDeviceId", "csr");
            CertificateSigningOperation operation = await client.SendCertificateSigningRequestAsync(request, TestContext.Current.CancellationToken);

            await ReceiveCertificateSigningResponseAsync(client, requestId, status, "{");

            Assert.False(operation.Accepted.IsCompleted);
            Assert.False(operation.Completed.IsCompleted);
            await ReceiveCertificateSigningResponseAsync(client, request.RequestId, "202", AcceptedResponse);
            await ReceiveCertificateSigningResponseAsync(client, request.RequestId, "200", CompletedResponse);
            Assert.True(operation.Completed.IsCompletedSuccessfully);
        }

        private static async Task ReceiveCertificateSigningResponseAsync(
            ConnectionClient client, string requestId, string status, string payload, bool acknowledgeFails = false)
        {
            // Invoke directly so a receive-handler exception cannot be hidden by the MQTT manager's event dispatch.
            var handler = typeof(ConnectionClient)
                .GetMethod("HandleReceivedCertificateSigningPublish", BindingFlags.NonPublic | BindingFlags.Instance)!
                .CreateDelegate<Func<MqttPublishReceivedEventArgs, Task>>(client);
            CertificateSigningPublishEventArgs args = new()
            {
                Publish = new()
                {
                    Topic = $"$iothub/credentials/res/{status}/?$rid={requestId}",
                    Payload = Encoding.UTF8.GetBytes(payload),
                },
                AcknowledgeFails = acknowledgeFails,
            };
            await handler(args).WaitAsync(TimeSpan.FromSeconds(1), TestContext.Current.CancellationToken);
            Assert.True(args.IsAcknowledged);
        }

        private sealed class CertificateSigningPublishEventArgs : MqttPublishReceivedEventArgs
        {
            public bool IsAcknowledged { get; private set; }
            public bool AcknowledgeFails { get; init; }

            public override Task AcknowledgeAsync(CancellationToken cancellationToken)
            {
                IsAcknowledged = true;
                return AcknowledgeFails
                    ? Task.FromException(new InvalidOperationException("Acknowledgment failed."))
                    : Task.CompletedTask;
            }
        }

        private const string TwinGetTopicPrefix = "$iothub/twin/GET/?$rid=";

        /// <summary>
        /// Like IoT hubs that use MQTTv5 push the twin upon connecting, the MQTTv3 device presence flow gets the twin
        /// once its subscriptions are in place (or resumed), and only completes once IoT hub responds.
        /// </summary>
        [Theory]
        [InlineData(false)]
        [InlineData(true)]
        public async Task PresenceFlowGetsTwinBeforeConnectionIsReady(bool isSessionPresent)
        {
            using MockConnectionMqttClient mockMqttClient = new() { AutoRespondToTwinGet = false };
            using ConnectionClient connectionClient = new(new() { MqttClient = mockMqttClient });
            IConnectionClient featureView = connectionClient;
            mockMqttClient.OnConnect = _ => Task.FromResult(new MqttConnectAck() { IsSessionPresent = isSessionPresent, ResultCode = MqttConnectReasonCode.Success });
            TaskCompletionSource<MqttPublish> twinGetSent = new(TaskCreationOptions.RunContinuationsAsynchronously);
            mockMqttClient.OnPublish = publish =>
            {
                if (publish.Topic.StartsWith(TwinGetTopicPrefix, StringComparison.Ordinal))
                {
                    twinGetSent.TrySetResult(publish);
                }

                return Task.FromResult(new MqttPublishAck() { ReasonCode = MqttPublishAckReasonCode.Success });
            };

            Task connecting = connectionClient.ConnectToHubAsync(GetMockConnectionContext(false), TestContext.Current.CancellationToken);
            MqttPublish twinGet = await twinGetSent.Task.WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);

            // The first connection always subscribes, even on a resumed session, since that session may have been left
            // behind by a previous process.
            Assert.Equal(1, mockMqttClient.SentMqttTrafficInOrder.Count(traffic => traffic.Subscribe != null));
            await Task.Delay(100, TestContext.Current.CancellationToken);
            Assert.False(connecting.IsCompleted, "The connection must not be ready before IoT hub responds with the twin.");
            Assert.False(featureView.IsConnectionReady);

            string rid = twinGet.Topic.Substring(TwinGetTopicPrefix.Length);
            await mockMqttClient.SimulatePublishReceivedAsync(new MqttPublish()
            {
                Topic = $"$iothub/twin/res/200/?$rid={rid}",
                Payload = Encoding.UTF8.GetBytes("""{"desired":{"a":1,"$version":5},"reported":{"b":2,"$version":3}}"""),
            });
            await connecting.WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);

            Assert.True(featureView.IsConnectionReady);
            ConnectTwinSnapshot snapshot = Assert.IsType<ConnectTwinSnapshot>(featureView.LatestConnectTwinSnapshot);
            Assert.Equal(featureView.HubConnectionId, snapshot.ConnectionId);
            Assert.Equal(5UL, snapshot.DesiredVersion);
            Assert.Equal(3UL, snapshot.ReportedVersion);
            Assert.Equal("""{"a":1}""", snapshot.Desired.ToJsonString());
            Assert.Equal("""{"b":2}""", snapshot.Reported.ToJsonString());
        }

        [Fact]
        public async Task PresenceFlowDoesNotGetTwinWhenTwinIsDisabled()
        {
            using MockConnectionMqttClient mockMqttClient = new();
            using ConnectionClient connectionClient = new(new() { MqttClient = mockMqttClient, EnableTwin = false });

            await connectionClient.ConnectToHubAsync(GetMockConnectionContext(false), TestContext.Current.CancellationToken);

            Assert.DoesNotContain(mockMqttClient.SentMqttTrafficInOrder, traffic => traffic.Publish != null);
            Assert.Null(((IConnectionClient)connectionClient).LatestConnectTwinSnapshot);
        }

        /// <summary>
        /// Failing to get the twin doesn't fail the connection; the twin client gets the twin itself instead.
        /// </summary>
        [Theory]
        [InlineData("error status")]
        [InlineData("timeout")]
        [InlineData("malformed")]
        public async Task PresenceFlowCompletesWithoutTwinWhenGettingTwinFails(string failure)
        {
            using MockConnectionMqttClient mockMqttClient = new();
            using ConnectionClient connectionClient = new(new() { MqttClient = mockMqttClient });
            switch (failure)
            {
                case "error status":
                    mockMqttClient.TwinGetResponseStatus = 500;
                    break;
                case "timeout":
                    mockMqttClient.AutoRespondToTwinGet = false;
                    connectionClient.ConnectTwinGetTimeout = TimeSpan.FromMilliseconds(100);
                    break;
                default:
                    mockMqttClient.TwinGetResponsePayload = "not json";
                    break;
            }

            int disconnectCount = 0;
            mockMqttClient.OnDisconnect = _ =>
            {
                Interlocked.Increment(ref disconnectCount);
                return Task.CompletedTask;
            };

            await connectionClient.ConnectToHubAsync(GetMockConnectionContext(false), TestContext.Current.CancellationToken)
                .WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);

            IConnectionClient featureView = connectionClient;
            Assert.True(featureView.IsConnectionReady);
            Assert.Null(featureView.LatestConnectTwinSnapshot);
            Assert.Equal(0, disconnectCount);
            Assert.Equal(1, mockMqttClient.ConnectAttemptCount);
        }

        /// <summary>
        /// Losing the connection while waiting for the twin abandons that presence flow; the next connection gets the twin
        /// again, and the twin got on a connection is cleared once it is lost.
        /// </summary>
        [Fact]
        public async Task ConnectionLostWhileGettingTwinIsAbandonedAndRetriedOnNextConnection()
        {
            using MockConnectionMqttClient mockMqttClient = new() { AutoRespondToTwinGet = false };
            using ConnectionClient connectionClient = new(new()
            {
                MqttClient = mockMqttClient,
                ConnectionRetryPolicy = new ExponentialBackoffRetryPolicy(uint.MaxValue, TimeSpan.Zero, useJitter: false),
            });
            IConnectionClient featureView = connectionClient;

            int presenceCompletionCount = 0;
            connectionClient.DevicePresenceFlowCompletedAsync += _ =>
            {
                Interlocked.Increment(ref presenceCompletionCount);
                return Task.CompletedTask;
            };

            int twinGetCount = 0;
            mockMqttClient.OnPublish = publish =>
            {
                if (publish.Topic.StartsWith(TwinGetTopicPrefix, StringComparison.Ordinal))
                {
                    if (Interlocked.Increment(ref twinGetCount) == 1)
                    {
                        _ = mockMqttClient.SimulateServerDisconnectAsync(MqttDisconnectReason.ImplementationSpecificError, new IOException("mock connection reset"));
                    }
                    else
                    {
                        _ = mockMqttClient.SimulatePublishReceivedAsync(new MqttPublish()
                        {
                            Topic = $"$iothub/twin/res/200/?$rid={publish.Topic.Substring(TwinGetTopicPrefix.Length)}",
                            Payload = Encoding.UTF8.GetBytes("""{"desired":{"$version":2},"reported":{"$version":1}}"""),
                        });
                    }
                }

                return Task.FromResult(new MqttPublishAck() { ReasonCode = MqttPublishAckReasonCode.Success });
            };

            await connectionClient.ConnectToHubAsync(GetMockConnectionContext(false), TestContext.Current.CancellationToken)
                .WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);

            Assert.Equal(2, twinGetCount);
            Assert.Equal(1, presenceCompletionCount);
            ConnectTwinSnapshot snapshot = Assert.IsType<ConnectTwinSnapshot>(featureView.LatestConnectTwinSnapshot);
            Assert.Equal(featureView.HubConnectionId, snapshot.ConnectionId);
            Assert.Equal(2UL, snapshot.DesiredVersion);

            mockMqttClient.OnPublish = null;
            mockMqttClient.AutoRespondToTwinGet = true;
            TaskCompletionSource reconnected = new(TaskCreationOptions.RunContinuationsAsynchronously);
            connectionClient.DevicePresenceFlowCompletedAsync += _ =>
            {
                reconnected.TrySetResult();
                return Task.CompletedTask;
            };
            ConnectTwinSnapshot? snapshotWhenReconnecting = snapshot;
            mockMqttClient.OnConnect = _ =>
            {
                snapshotWhenReconnecting = featureView.LatestConnectTwinSnapshot;
                return Task.FromResult(new MqttConnectAck() { ResultCode = MqttConnectReasonCode.Success });
            };

            await mockMqttClient.SimulateServerDisconnectAsync(MqttDisconnectReason.ImplementationSpecificError, new IOException("mock connection reset"))
                .WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);
            await reconnected.Task.WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);

            // The twin got on the lost connection is cleared before the next connection is made.
            Assert.Null(snapshotWhenReconnecting);

            snapshot = Assert.IsType<ConnectTwinSnapshot>(featureView.LatestConnectTwinSnapshot);
            Assert.Equal(featureView.HubConnectionId, snapshot.ConnectionId);
            Assert.Equal(1UL, snapshot.DesiredVersion);
        }

        private static ConnectionContext GetMockConnectionContext(bool isMQTTv5)
        {
#pragma warning disable SYSLIB0026 // Type or member is obsolete (Mock certificate, don't need to load a real one using typical X509 certificate loader
            return new ConnectionContext()
            {
                AuthenticationProvider = new X509AuthenticationProvider(new System.Security.Cryptography.X509Certificates.X509Certificate2(), null),
                DeviceId = "someDeviceId",
                IotHubHostName = "someHostName",
                ConnectionProfile = isMQTTv5 ? IotHubConnectionType.Mqttv5 : IotHubConnectionType.Mqttv3,
            };
#pragma warning restore SYSLIB0026 // Type or member is obsolete
        }

        [Fact]
        public async Task ProvisionAndConnectUsesSeededConnectionContext()
        {
            using MockConnectionMqttClient mockMqttClient = new();
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
        public async Task ReconnectResumesSessionWithoutResubscribingOnlyWhenCleanSessionOptedOut(bool useCleanSession)
        {
            using MockConnectionMqttClient mockMqttClient = new();
            using ConnectionClient connectionClient = new(new()
            {
                MqttClient = mockMqttClient,
                UseCleanSession = useCleanSession,
                ConnectionRetryPolicy = new ExponentialBackoffRetryPolicy(uint.MaxValue, TimeSpan.Zero, useJitter: false),
            });

            List<(bool CleanSession, uint SessionExpiryInterval)> connectSettings = new();
            mockMqttClient.OnConnect = connect =>
            {
                lock (connectSettings)
                {
                    connectSettings.Add((connect.CleanSession, connect.SessionExpiryInterval));
                }

                return Task.FromResult(new MqttConnectAck() { IsSessionPresent = !connect.CleanSession, ResultCode = MqttConnectReasonCode.Success });
            };

            await connectionClient.ConnectToHubAsync(GetMockConnectionContext(false), TestContext.Current.CancellationToken);

            TaskCompletionSource reconnected = new(TaskCreationOptions.RunContinuationsAsynchronously);
            connectionClient.DevicePresenceFlowCompletedAsync += _ =>
            {
                reconnected.TrySetResult();
                return Task.CompletedTask;
            };

            await mockMqttClient.SimulateServerDisconnectAsync(MqttDisconnectReason.ImplementationSpecificError, new Exception("mock exception"))
                .WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);
            await reconnected.Task.WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);

            // Resumption is always requested unless opted out, but the first connection subscribes because this process had
            // not yet subscribed on any session, even though the broker reports one.
            Assert.Equal(new[] { (useCleanSession, 0u), (useCleanSession, 0u) }, connectSettings);
            Assert.Equal(useCleanSession ? 2 : 1, mockMqttClient.SentMqttTrafficInOrder.Count(traffic => traffic.Subscribe != null));
        }

        [Fact]
        public async Task FailedConnectAttemptForcesResubscribeOnResumedSession()
        {
            using MockConnectionMqttClient mockMqttClient = new();
            using ConnectionClient connectionClient = new(new()
            {
                MqttClient = mockMqttClient,
                UseCleanSession = false,
                ConnectionRetryPolicy = new ExponentialBackoffRetryPolicy(uint.MaxValue, TimeSpan.Zero, useJitter: false),
            });

            await connectionClient.ConnectToHubAsync(GetMockConnectionContext(false), TestContext.Current.CancellationToken);

            List<bool> cleanSessionFlags = new();
            mockMqttClient.OnConnect = connect =>
            {
                lock (cleanSessionFlags)
                {
                    cleanSessionFlags.Add(connect.CleanSession);
                    if (cleanSessionFlags.Count == 1)
                    {
                        // The broker may have started a new, empty session for this attempt, but its CONNACK was lost.
                        throw new TimeoutException("mock CONNACK lost");
                    }
                }

                return Task.FromResult(new MqttConnectAck() { IsSessionPresent = !connect.CleanSession, ResultCode = MqttConnectReasonCode.Success });
            };

            TaskCompletionSource reconnected = new(TaskCreationOptions.RunContinuationsAsynchronously);
            connectionClient.DevicePresenceFlowCompletedAsync += _ =>
            {
                reconnected.TrySetResult();
                return Task.CompletedTask;
            };

            await mockMqttClient.SimulateServerDisconnectAsync(MqttDisconnectReason.ImplementationSpecificError, new Exception("mock exception"))
                .WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);
            await reconnected.Task.WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);

            // The broker resumed a session on the final attempt, but the failed attempt before it may have replaced the
            // subscribed session with an empty one, so the subscriptions are sent again.
            Assert.Equal(new[] { false, false }, cleanSessionFlags);
            Assert.Equal(2, mockMqttClient.SentMqttTrafficInOrder.Count(traffic => traffic.Subscribe != null));
        }

        [Theory]
        [InlineData(true)]
        [InlineData(false)]
        public async Task FirstConnectionUnsubscribesDisabledFeaturesOnlyOnResumedSession(bool isSessionResumed)
        {
            using MockConnectionMqttClient mockMqttClient = new();
            using ConnectionClient connectionClient = new(new()
            {
                MqttClient = mockMqttClient,
                UseCleanSession = false,
                EnableCloudToDeviceMessages = false,
                EnableDirectMethods = false,
            });

            mockMqttClient.OnConnect = connect =>
                Task.FromResult(new MqttConnectAck() { IsSessionPresent = isSessionResumed, ResultCode = MqttConnectReasonCode.Success });

            await connectionClient.ConnectToHubAsync(GetMockConnectionContext(false), TestContext.Current.CancellationToken);

            // A session left behind by a previous process may still hold subscriptions for the features that are now
            // disabled, so they are removed before the enabled features are subscribed to.
            // The device presence flow then gets the twin.
            List<MockMqttOutgoingTraffic> traffic = mockMqttClient.SentMqttTrafficInOrder;
            if (isSessionResumed)
            {
                Assert.Equal(3, traffic.Count);
                Assert.NotNull(traffic[0].Unsubscribe);
                Assert.Equal(
                    new[] { "devices/someDeviceId/messages/devicebound/#", Microsoft.Azure.Iot.Device.Unified.DirectMethods.DirectMethodClient.ClassicDirectMethodsRequestTopic + "#" },
                    traffic[0].Unsubscribe!.TopicFilters);
                Assert.NotNull(traffic[1].Subscribe);
            }
            else
            {
                Assert.Equal(2, traffic.Count);
                Assert.NotNull(traffic[0].Subscribe);
            }

            Assert.StartsWith(TwinGetTopicPrefix, traffic[^1].Publish!.Topic);
        }

        [Fact]
        public async Task FailedUnsubscribeOfDisabledFeaturesOnResumedSessionReconnects()
        {
            using MockConnectionMqttClient mockMqttClient = new();
            using ConnectionClient connectionClient = new(new()
            {
                MqttClient = mockMqttClient,
                UseCleanSession = false,
                EnableCloudToDeviceMessages = false,
                EnableDirectMethods = false,
            });

            mockMqttClient.OnConnect = connect =>
                Task.FromResult(new MqttConnectAck() { IsSessionPresent = true, ResultCode = MqttConnectReasonCode.Success });

            // The first UNSUBSCRIBE fails without the connection being lost, as on an UNSUBACK timeout.
            int unsubscribeCount = 0;
            mockMqttClient.OnUnsubscribe = unsubscribe =>
            {
                if (Interlocked.Increment(ref unsubscribeCount) == 1)
                {
                    throw new TimeoutException("mock UNSUBACK timeout");
                }

                return Task.FromResult(MqttObjectHelpers.CreateSuccessfulUnsuback(unsubscribe));
            };

            TaskCompletionSource subscribed = new(TaskCreationOptions.RunContinuationsAsynchronously);
            mockMqttClient.OnSubscribe = subscribe =>
            {
                subscribed.TrySetResult();
                return Task.FromResult(MqttObjectHelpers.CreateSuccessfulSuback(subscribe));
            };

            await connectionClient.ConnectToHubAsync(GetMockConnectionContext(false), TestContext.Current.CancellationToken);
            await subscribed.Task.WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);

            // The failed presence flow tore the connection down so that the next connection could finish it.
            Assert.Single(mockMqttClient.DisconnectRequests);
            Assert.Equal(2, mockMqttClient.ConnectAttemptCount);
            Assert.Equal(2, Volatile.Read(ref unsubscribeCount));
        }

        [Theory]
        [InlineData(true, true)]
        [InlineData(true, false)]
        [InlineData(false, true)]
        [InlineData(false, false)]
        public async Task ConnectionClientReannouncesBirthBeforeContinuingPublish(bool isMQTTv5, bool isSessionResumed)
        {
            using MockConnectionMqttClient mockMqttClient = new() { AutoRespondToBirth = true };
            ConnectionClient connectionClient = new(new()
            {
                MqttClient = mockMqttClient
            });

            TelemetryClient telemetryClient = new(connectionClient);

            await connectionClient.ConnectToHubAsync(GetMockConnectionContext(isMQTTv5), cancellationToken: TestContext.Current.CancellationToken);

            // Setup mock MQTT layer to lose connection when telemetry client sends a publish for the first time (subsequent retries will work normally)
            int retryCount = 0;
            mockMqttClient.OnConnect = connect =>
            {
                return Task.FromResult(new MqttConnectAck() { IsSessionPresent = isSessionResumed, ResultCode = MqttConnectReasonCode.Success });
            };
            mockMqttClient.OnPublish = async (publish) =>
            {
                if (retryCount == 0)
                {
                    _ = mockMqttClient.SimulateServerDisconnectAsync(MqttDisconnectReason.ImplementationSpecificError, new Exception("mock exception"));
                    retryCount++;
                    throw new MqttClientNotConnectedException("mock client not connected exception");
                }

                return new MqttPublishAck() { ReasonCode = MqttPublishAckReasonCode.Success };
            };

            await telemetryClient.SendTelemetryAsync(new Models.Telemetry.DeviceToCloudTelemetry(), TestContext.Current.CancellationToken);

            if (isMQTTv5)
            {
                if (isSessionResumed)
                {
                    // With session resumed, the birth flow doesn't need to send the subscribe on the presence topic again
                    Assert.Equal(4, mockMqttClient.SentMqttTrafficInOrder.Count);
                }
                else
                {
                    Assert.Equal(5, mockMqttClient.SentMqttTrafficInOrder.Count);
                }
            }
            else
            {
                // Each MQTTv3 device presence flow also gets the twin.
                if (isSessionResumed)
                {
                    // With session resumed, the reconnect flow doesn't need to send the subscribe on the direct methods/telemetry/twin topics
                    Assert.Equal(4, mockMqttClient.SentMqttTrafficInOrder.Count);
                }
                else
                {
                    Assert.Equal(5, mockMqttClient.SentMqttTrafficInOrder.Count);
                }
            }

            var lastTraffic = mockMqttClient.SentMqttTrafficInOrder.Last();

            // Verify that the telemetry sent by the telemetry client is the last piece of traffic "sent" in this test. It must be preceded by the intial device presence flow traffic and by the reconnection device presence flow traffic
            Assert.NotNull(lastTraffic.Publish);
            if (isMQTTv5)
            {
                Assert.StartsWith("ih/", lastTraffic.Publish.Topic);
                Assert.EndsWith("/srv/telemetry", lastTraffic.Publish.Topic);
            }
            else
            {
                Assert.StartsWith("devices/", lastTraffic.Publish.Topic);
                Assert.EndsWith("/messages/events/", lastTraffic.Publish.Topic);
            }
        }

        [Fact]
        public async Task PublishRetriesWhenPresenceCompletesBeforeFailedAttemptReturns()
        {
            using MockConnectionMqttClient mockMqttClient = new();
            using TestConnectionClient connectionClient = new(new()
            {
                MqttClient = mockMqttClient,
                ConnectionRetryPolicy = new ExponentialBackoffRetryPolicy(uint.MaxValue, TimeSpan.Zero, useJitter: false),
            });

            await connectionClient.ConnectToHubAsync(GetMockConnectionContext(false), TestContext.Current.CancellationToken);

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

            int publishAttemptCount = 0;
            mockMqttClient.OnPublish = async publish =>
            {
                if (Interlocked.Increment(ref publishAttemptCount) == 1)
                {
                    firstPublishAttemptStarted.TrySetResult();
                    await releaseFirstPublishAttempt.Task;
                    throw new MqttClientNotConnectedException("mock client not connected exception");
                }

                return new MqttPublishAck() { ReasonCode = MqttPublishAckReasonCode.Success };
            };

            Task<MqttPublishAck> pendingPublish = connectionClient.PublishAsync(
                new MqttPublish() { Topic = "test/topic" },
                throwOnErrorReasonCode: true, TestContext.Current.CancellationToken);

            try
            {
                await firstPublishAttemptStarted.Task.WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);

                // Keep this on the same endpoint: the reconnect and presence flow must finish while the original publish is still pending.
                await mockMqttClient.SimulateServerDisconnectAsync(MqttDisconnectReason.ImplementationSpecificError, new Exception("mock exception"))
                    .WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);
                await reconnectPresenceCompleted.Task.WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);

                releaseFirstPublishAttempt.TrySetResult();
                await pendingPublish.WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);

                Assert.Equal(1, presenceCompletionCount);
                Assert.Equal(2, publishAttemptCount);
            }
            finally
            {
                releaseFirstPublishAttempt.TrySetResult();
            }
        }

        [Theory]
        [InlineData(true, true)]
        [InlineData(true, false)]
        [InlineData(false, true)]
        [InlineData(false, false)]
        public async Task ConnectionClientReannouncesBirthBeforeContinuingSubscribe(bool isMQTTv5, bool isSessionResumed)
        {
            using MockConnectionMqttClient mockMqttClient = new() { AutoRespondToBirth = true };
            ConnectionClient connectionClient = new(new()
            {
                MqttClient = mockMqttClient
            });

            await connectionClient.ConnectToHubAsync(GetMockConnectionContext(isMQTTv5), cancellationToken: TestContext.Current.CancellationToken);

            // Setup mock MQTT layer to lose connection when telemetry client sends a publish for the first time (subsequent retries will work normally)
            int retryCount = 0;
            mockMqttClient.OnConnect = connect =>
            {
                return Task.FromResult(new MqttConnectAck() { IsSessionPresent = isSessionResumed, ResultCode = MqttConnectReasonCode.Success });
            };

            string expectedTopicString = Guid.NewGuid().ToString();

            mockMqttClient.OnSubscribe = async (subscribe) =>
            {
                if (retryCount == 0 && subscribe.TopicFilters.FirstOrDefault()!.Topic.Equals(expectedTopicString))
                {
                    _ = mockMqttClient.SimulateServerDisconnectAsync(MqttDisconnectReason.ImplementationSpecificError, new Exception("mock exception"));
                    retryCount++;
                    throw new MqttClientNotConnectedException("mock client not connected exception");
                }

                return MqttObjectHelpers.CreateSuccessfulSuback(subscribe);
            };

            MqttSubscribe featureClientLevelSubscribeRequest = new(expectedTopicString, MqttQualityOfServiceLevel.AtLeastOnce);
            await connectionClient.SubscribeAsync(featureClientLevelSubscribeRequest, throwOnErrorReasonCode: true, TestContext.Current.CancellationToken);


            if (isMQTTv5)
            {
                if (isSessionResumed)
                {
                    // With session resumed, the birth flow doesn't need to send the subscribe on the presence topic again
                    Assert.Equal(4, mockMqttClient.SentMqttTrafficInOrder.Count);
                }
                else
                {
                    Assert.Equal(5, mockMqttClient.SentMqttTrafficInOrder.Count);
                }
            }
            else
            {
                // Each MQTTv3 device presence flow also gets the twin.
                if (isSessionResumed)
                {
                    // With session resumed, the reconnect flow doesn't need to send the subscribe on the direct methods/telemetry/twin topics
                    Assert.Equal(4, mockMqttClient.SentMqttTrafficInOrder.Count);

                }
                else
                {
                    Assert.Equal(5, mockMqttClient.SentMqttTrafficInOrder.Count);
                }
            }

            var lastTraffic = mockMqttClient.SentMqttTrafficInOrder.Last();

            // Verify that the subscribe sent directly from this test is the last piece of traffic "sent" in this test. It must be preceded by the intial device presence flow traffic and by the reconnection device presence flow traffic
            Assert.NotNull(lastTraffic.Subscribe);
            Assert.Single(lastTraffic.Subscribe.TopicFilters);
            Assert.Equal(expectedTopicString, lastTraffic.Subscribe.TopicFilters.FirstOrDefault()!.Topic);
        }

        [Theory]
        [InlineData(true, true)]
        [InlineData(true, false)]
        [InlineData(false, true)]
        [InlineData(false, false)]
        public async Task ConnectionClientReannouncesBirthBeforeContinuingUnsubscribe(bool isMQTTv5, bool isSessionResumed)
        {
            using MockConnectionMqttClient mockMqttClient = new() { AutoRespondToBirth = true };
            ConnectionClient connectionClient = new(new()
            {
                MqttClient = mockMqttClient
            });

            await connectionClient.ConnectToHubAsync(GetMockConnectionContext(isMQTTv5), cancellationToken: TestContext.Current.CancellationToken);

            // Setup mock MQTT layer to lose connection when telemetry client sends a publish for the first time (subsequent retries will work normally)
            int retryCount = 0;
            mockMqttClient.OnConnect = connect =>
            {
                return Task.FromResult(new MqttConnectAck() { IsSessionPresent = isSessionResumed, ResultCode = MqttConnectReasonCode.Success });
            };

            string expectedTopicString = Guid.NewGuid().ToString();

            mockMqttClient.OnUnsubscribe = async (unsubscribe) =>
            {
                if (retryCount == 0 && unsubscribe.TopicFilters.FirstOrDefault()!.Equals(expectedTopicString))
                {
                    _ = mockMqttClient.SimulateServerDisconnectAsync(MqttDisconnectReason.ImplementationSpecificError, new Exception("mock exception"));
                    retryCount++;
                    throw new MqttClientNotConnectedException("mock client not connected exception");
                }

                return MqttObjectHelpers.CreateSuccessfulUnsuback(unsubscribe);
            };

            MqttUnsubscribe featureClientLevelUnsubscribeRequest = new(expectedTopicString);
            await connectionClient.UnsubscribeAsync(featureClientLevelUnsubscribeRequest, throwOnErrorReasonCode: true, TestContext.Current.CancellationToken);


            if (isMQTTv5)
            {
                if (isSessionResumed)
                {
                    // With session resumed, the birth flow doesn't need to send the subscribe on the presence topic again
                    Assert.Equal(4, mockMqttClient.SentMqttTrafficInOrder.Count);
                }
                else
                {
                    Assert.Equal(5, mockMqttClient.SentMqttTrafficInOrder.Count);
                }
            }
            else
            {
                // Each MQTTv3 device presence flow also gets the twin.
                if (isSessionResumed)
                {
                    // With session resumed, the reconnect flow doesn't need to send the subscribe on the direct methods/telemetry/twin topics
                    Assert.Equal(4, mockMqttClient.SentMqttTrafficInOrder.Count);

                }
                else
                {
                    Assert.Equal(5, mockMqttClient.SentMqttTrafficInOrder.Count);
                }
            }

            var lastTraffic = mockMqttClient.SentMqttTrafficInOrder.Last();

            // Verify that the subscribe sent directly from this test is the last piece of traffic "sent" in this test. It must be preceded by the intial device presence flow traffic and by the reconnection device presence flow traffic
            Assert.NotNull(lastTraffic.Unsubscribe);
            Assert.Single(lastTraffic.Unsubscribe.TopicFilters);
            Assert.Equal(expectedTopicString, lastTraffic.Unsubscribe.TopicFilters.FirstOrDefault()!);
        }

        /// <summary>
        /// A QoS 1 PUBLISH, SUBSCRIBE or UNSUBSCRIBE whose connection is lost before its acknowledgement arrives must be
        /// sent again once the device is reconnected, rather than failing.
        /// </summary>
        [Theory]
        [InlineData("publish", true, false)]
        [InlineData("publish", false, false)]
        [InlineData("subscribe", true, false)]
        [InlineData("subscribe", false, false)]
        [InlineData("unsubscribe", true, false)]
        [InlineData("unsubscribe", false, false)]
        [InlineData("publish", true, true)]
        [InlineData("publish", false, true)]
        [InlineData("subscribe", true, true)]
        [InlineData("subscribe", false, true)]
        [InlineData("unsubscribe", true, true)]
        [InlineData("unsubscribe", false, true)]
        public async Task OperationInterruptedByConnectionLossBeforeAckIsResentAfterReconnecting(string operation, bool throwOnErrorReasonCode, bool isMQTTv5)
        {
            using MockConnectionMqttClient mockMqttClient = new() { AutoRespondToBirth = true };
            using ConnectionClient connectionClient = new(new()
            {
                MqttClient = mockMqttClient,
                ConnectionRetryPolicy = new ExponentialBackoffRetryPolicy(uint.MaxValue, TimeSpan.Zero, useJitter: false),
            });

            await connectionClient.ConnectToHubAsync(GetMockConnectionContext(isMQTTv5), TestContext.Current.CancellationToken);

            string topic = Guid.NewGuid().ToString();
            int attemptCount = 0;
            Task? disconnection = null;

            // The first attempt is sent, but the connection drops before it is acknowledged, failing it the way MQTTnet
            // fails every operation still waiting for an acknowledgement.
            void LoseConnectionOnFirstAttempt()
            {
                if (Interlocked.Increment(ref attemptCount) == 1)
                {
                    disconnection = mockMqttClient.SimulateServerDisconnectAsync(MqttDisconnectReason.ImplementationSpecificError, new IOException("mock connection reset"));
                    throw new IOException("mock connection reset");
                }
            }

            mockMqttClient.OnPublish = publish =>
            {
                if (publish.Topic == topic)
                {
                    LoseConnectionOnFirstAttempt();
                }

                return Task.FromResult(new MqttPublishAck() { ReasonCode = MqttPublishAckReasonCode.Success });
            };
            mockMqttClient.OnSubscribe = subscribe =>
            {
                if (subscribe.TopicFilters.Any(topicFilter => topicFilter.Topic == topic))
                {
                    LoseConnectionOnFirstAttempt();
                }

                return Task.FromResult(MqttObjectHelpers.CreateSuccessfulSuback(subscribe));
            };
            mockMqttClient.OnUnsubscribe = unsubscribe =>
            {
                LoseConnectionOnFirstAttempt();
                return Task.FromResult(MqttObjectHelpers.CreateSuccessfulUnsuback(unsubscribe));
            };

            Task pendingOperation = operation switch
            {
                "publish" => connectionClient.PublishAsync(new MqttPublish() { Topic = topic, QualityOfServiceLevel = MqttQualityOfServiceLevel.AtLeastOnce }, throwOnErrorReasonCode, TestContext.Current.CancellationToken),
                "subscribe" => connectionClient.SubscribeAsync(new MqttSubscribe(topic, MqttQualityOfServiceLevel.AtLeastOnce), throwOnErrorReasonCode, TestContext.Current.CancellationToken),
                _ => connectionClient.UnsubscribeAsync(new MqttUnsubscribe(topic), throwOnErrorReasonCode, TestContext.Current.CancellationToken),
            };

            await pendingOperation.WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);
            await disconnection!.WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);

            Assert.Equal(2, attemptCount);
            Assert.Equal(2, mockMqttClient.ConnectAttemptCount);

            MockMqttOutgoingTraffic lastTraffic = mockMqttClient.SentMqttTrafficInOrder.Last();
            switch (operation)
            {
                case "publish":
                    Assert.Equal(topic, lastTraffic.Publish!.Topic);
                    break;
                case "subscribe":
                    Assert.Equal(topic, Assert.Single(lastTraffic.Subscribe!.TopicFilters).Topic);
                    break;
                default:
                    Assert.Equal(topic, Assert.Single(lastTraffic.Unsubscribe!.TopicFilters));
                    break;
            }
        }

        /// <summary>
        /// An operation waiting for a reconnection is canceled when the caller deliberately disconnects.
        /// </summary>
        [Fact]
        public async Task DisconnectCancelsOperationWaitingForReconnection()
        {
            using MockConnectionMqttClient mockMqttClient = new();
            using ConnectionClient connectionClient = new(new()
            {
                MqttClient = mockMqttClient,
                ConnectionRetryPolicy = new ExponentialBackoffRetryPolicy(uint.MaxValue, TimeSpan.Zero, useJitter: false),
            });

            await connectionClient.ConnectToHubAsync(GetMockConnectionContext(false), TestContext.Current.CancellationToken);

            TaskCompletionSource reconnectStarted = new(TaskCreationOptions.RunContinuationsAsynchronously);
            TaskCompletionSource<MqttConnectAck> finishReconnect = new(TaskCreationOptions.RunContinuationsAsynchronously);
            mockMqttClient.OnConnect = _ =>
            {
                reconnectStarted.TrySetResult();
                return finishReconnect.Task;
            };

            Task? disconnection = null;
            mockMqttClient.OnPublish = async _ =>
            {
                disconnection = mockMqttClient.SimulateServerDisconnectAsync(
                    MqttDisconnectReason.ImplementationSpecificError,
                    new IOException("mock connection reset"));
                await Task.Yield();
                throw new IOException("mock connection reset");
            };

            Task<MqttPublishAck> pendingOperation = connectionClient.PublishAsync(
                new MqttPublish()
                {
                    Topic = "devices/test/messages/events/",
                    QualityOfServiceLevel = MqttQualityOfServiceLevel.AtLeastOnce,
                },
                throwOnErrorReasonCode: true, TestContext.Current.CancellationToken);

            try
            {
                await reconnectStarted.Task.WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);
                await Task.Delay(TimeSpan.FromMilliseconds(100), TestContext.Current.CancellationToken);
                Assert.False(pendingOperation.IsCompleted);

                await connectionClient.DisconnectAsync(TestContext.Current.CancellationToken);

                await Assert.ThrowsAnyAsync<OperationCanceledException>(
                    () => pendingOperation.WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken));
            }
            finally
            {
                finishReconnect.TrySetException(new OperationCanceledException());
                if (disconnection != null)
                {
                    await disconnection.WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);
                }
            }
        }

        [Fact]
        public async Task AlreadyCanceledDisconnectDoesNotCancelOperationsWaitingForRecovery()
        {
            using MockConnectionMqttClient mockMqttClient = new();
            using ConnectionClient connectionClient = new(new()
            {
                MqttClient = mockMqttClient,
                ConnectionRetryPolicy = new ExponentialBackoffRetryPolicy(uint.MaxValue, TimeSpan.Zero, useJitter: false),
            });

            await connectionClient.ConnectToHubAsync(GetMockConnectionContext(false), TestContext.Current.CancellationToken);

            using CancellationTokenSource canceledDisconnect = new();
            canceledDisconnect.Cancel();
            await Assert.ThrowsAnyAsync<OperationCanceledException>(
                () => connectionClient.DisconnectAsync(canceledDisconnect.Token));

            int attemptCount = 0;
            Task? disconnection = null;
            mockMqttClient.OnPublish = publish =>
            {
                if (publish.Topic == "test/topic" && Interlocked.Increment(ref attemptCount) == 1)
                {
                    disconnection = mockMqttClient.SimulateServerDisconnectAsync(
                        MqttDisconnectReason.ImplementationSpecificError,
                        new IOException("mock connection reset"));
                    throw new IOException("mock connection reset");
                }

                return Task.FromResult(new MqttPublishAck() { ReasonCode = MqttPublishAckReasonCode.Success });
            };

            await connectionClient.PublishAsync(
                new MqttPublish() { Topic = "test/topic", QualityOfServiceLevel = MqttQualityOfServiceLevel.AtLeastOnce },
                throwOnErrorReasonCode: true, TestContext.Current.CancellationToken)
                .WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);
            await disconnection!.WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);

            Assert.Equal(2, attemptCount);
        }

        [Fact]
        public async Task InterruptedOperationIsNotRetriedAfterItsConnectionLifetimeEnds()
        {
            using MockConnectionMqttClient mockMqttClient = new();
            using ConnectionClient connectionClient = new(new()
            {
                MqttClient = mockMqttClient,
                ConnectionRetryPolicy = new ExponentialBackoffRetryPolicy(uint.MaxValue, TimeSpan.Zero, useJitter: false),
            });

            ConnectionContext context = GetMockConnectionContext(false);
            await connectionClient.ConnectToHubAsync(context, TestContext.Current.CancellationToken);

            TaskCompletionSource firstAttemptStarted = new(TaskCreationOptions.RunContinuationsAsynchronously);
            TaskCompletionSource releaseFirstAttempt = new(TaskCreationOptions.RunContinuationsAsynchronously);
            TaskCompletionSource presenceFlowCompleted = new(TaskCreationOptions.RunContinuationsAsynchronously);
            connectionClient.DevicePresenceFlowCompletedAsync += _ =>
            {
                presenceFlowCompleted.TrySetResult();
                return Task.CompletedTask;
            };

            int attemptCount = 0;
            mockMqttClient.OnPublish = async publish =>
            {
                if (publish.Topic != "test/topic")
                {
                    return new MqttPublishAck() { ReasonCode = MqttPublishAckReasonCode.Success };
                }

                if (Interlocked.Increment(ref attemptCount) == 1)
                {
                    firstAttemptStarted.TrySetResult();
                    await releaseFirstAttempt.Task;
                    throw new IOException("mock connection reset");
                }

                return new MqttPublishAck() { ReasonCode = MqttPublishAckReasonCode.Success };
            };

            Task<MqttPublishAck> pendingOperation = connectionClient.PublishAsync(
                new MqttPublish() { Topic = "test/topic", QualityOfServiceLevel = MqttQualityOfServiceLevel.AtLeastOnce },
                throwOnErrorReasonCode: true, TestContext.Current.CancellationToken);

            try
            {
                await firstAttemptStarted.Task.WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);
                await mockMqttClient.SimulateServerDisconnectAsync(MqttDisconnectReason.ImplementationSpecificError);
                await presenceFlowCompleted.Task.WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);

                await connectionClient.DisconnectAsync(TestContext.Current.CancellationToken);
                await connectionClient.ProvisionAndConnectAsync(
                    new ProvisioningSettings("someIdScope"),
                    context.AuthenticationProvider,
                    cancellationToken: TestContext.Current.CancellationToken);
                releaseFirstAttempt.TrySetResult();

                await Assert.ThrowsAnyAsync<OperationCanceledException>(
                    () => pendingOperation.WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken));
                Assert.Equal(1, attemptCount);
            }
            finally
            {
                releaseFirstAttempt.TrySetResult();
            }
        }

        [Fact]
        public async Task ReleasedOperationWaiterIsNotRetriedAfterDisconnectAndReconnect()
        {
            using MockConnectionMqttClient mockMqttClient = new();
            using ConnectionClient connectionClient = new(new()
            {
                MqttClient = mockMqttClient,
                ConnectionRetryPolicy = new ExponentialBackoffRetryPolicy(uint.MaxValue, TimeSpan.Zero, useJitter: false),
            });

            ConnectionContext context = GetMockConnectionContext(false);
            await connectionClient.ConnectToHubAsync(context, TestContext.Current.CancellationToken);

            TaskCompletionSource firstAttemptStarted = new(TaskCreationOptions.RunContinuationsAsynchronously);
            TaskCompletionSource releaseFirstAttempt = new(TaskCreationOptions.RunContinuationsAsynchronously);
            TaskCompletionSource reconnectStarted = new(TaskCreationOptions.RunContinuationsAsynchronously);
            TaskCompletionSource presenceFlowCompleted = new(TaskCreationOptions.RunContinuationsAsynchronously);
            TaskCompletionSource<MqttConnectAck> finishReconnect = new(TaskCreationOptions.RunContinuationsAsynchronously);
            mockMqttClient.OnConnect = _ =>
            {
                reconnectStarted.TrySetResult();
                return finishReconnect.Task;
            };

            int attemptCount = 0;
            int connectionTransitionStarted = 0;
            Task? deliberateDisconnect = null;
            Task? disconnection = null;
            connectionClient.DevicePresenceFlowCompletedAsync += _ =>
            {
                if (Interlocked.Exchange(ref connectionTransitionStarted, 1) == 0)
                {
                    deliberateDisconnect = connectionClient.DisconnectAsync(TestContext.Current.CancellationToken);
                    presenceFlowCompleted.TrySetResult();
                }

                return Task.CompletedTask;
            };

            mockMqttClient.OnPublish = publish =>
            {
                if (publish.Topic != "test/topic")
                {
                    return Task.FromResult(new MqttPublishAck() { ReasonCode = MqttPublishAckReasonCode.Success });
                }

                if (Interlocked.Increment(ref attemptCount) == 1)
                {
                    firstAttemptStarted.TrySetResult();
                    return FailFirstAttemptAsync();
                }

                return Task.FromResult(new MqttPublishAck() { ReasonCode = MqttPublishAckReasonCode.Success });
            };

            async Task<MqttPublishAck> FailFirstAttemptAsync()
            {
                await releaseFirstAttempt.Task;
                disconnection = mockMqttClient.SimulateServerDisconnectAsync(MqttDisconnectReason.ImplementationSpecificError);
                throw new IOException("mock connection reset");
            }

            Task<MqttPublishAck> pendingOperation = connectionClient.PublishAsync(
                new MqttPublish() { Topic = "test/topic", QualityOfServiceLevel = MqttQualityOfServiceLevel.AtLeastOnce },
                throwOnErrorReasonCode: true, TestContext.Current.CancellationToken);

            try
            {
                await firstAttemptStarted.Task.WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);
                releaseFirstAttempt.TrySetResult();
                await reconnectStarted.Task.WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);
                finishReconnect.TrySetResult(new MqttConnectAck() { ResultCode = MqttConnectReasonCode.Success });

                await presenceFlowCompleted.Task.WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);
                await deliberateDisconnect!.WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);
                await disconnection!.WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);
                await connectionClient.ProvisionAndConnectAsync(
                    new ProvisioningSettings("someIdScope"),
                    context.AuthenticationProvider,
                    cancellationToken: TestContext.Current.CancellationToken);

                await Assert.ThrowsAnyAsync<OperationCanceledException>(
                    () => pendingOperation.WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken));
                Assert.Equal(1, attemptCount);
            }
            finally
            {
                releaseFirstAttempt.TrySetResult();
                finishReconnect.TrySetResult(new MqttConnectAck() { ResultCode = MqttConnectReasonCode.Success });
            }
        }

        /// <summary>
        /// A QoS 0 PUBLISH is never acknowledged, so losing the connection while sending it is reported to the caller
        /// rather than being sent again.
        /// </summary>
        [Theory]
        [InlineData(false)]
        [InlineData(true)]
        public async Task QoS0PublishInterruptedByConnectionLossIsNotResent(bool isMQTTv5)
        {
            using MockConnectionMqttClient mockMqttClient = new() { AutoRespondToBirth = true };
            using ConnectionClient connectionClient = new(new()
            {
                MqttClient = mockMqttClient,
                ConnectionRetryPolicy = new ExponentialBackoffRetryPolicy(uint.MaxValue, TimeSpan.Zero, useJitter: false),
            });

            await connectionClient.ConnectToHubAsync(GetMockConnectionContext(isMQTTv5), TestContext.Current.CancellationToken);

            TaskCompletionSource recoveryCompleted = new(TaskCreationOptions.RunContinuationsAsynchronously);
            connectionClient.DevicePresenceFlowCompletedAsync += _ =>
            {
                recoveryCompleted.TrySetResult();
                return Task.CompletedTask;
            };

            int attemptCount = 0;
            Task? disconnection = null;
            mockMqttClient.OnPublish = publish =>
            {
                if (publish.Topic != "test/topic")
                {
                    return Task.FromResult(new MqttPublishAck() { ReasonCode = MqttPublishAckReasonCode.Success });
                }

                Interlocked.Increment(ref attemptCount);
                disconnection = mockMqttClient.SimulateServerDisconnectAsync(MqttDisconnectReason.ImplementationSpecificError, new IOException("mock connection reset"));
                throw new IOException("mock connection reset");
            };

            await Assert.ThrowsAsync<DeviceException>(() => connectionClient.PublishAsync(
                new MqttPublish() { Topic = "test/topic", QualityOfServiceLevel = MqttQualityOfServiceLevel.AtMostOnce },
                throwOnErrorReasonCode: true, TestContext.Current.CancellationToken));
            await disconnection!.WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);
            await recoveryCompleted.Task.WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);

            Assert.Equal(1, attemptCount);
        }
    }
}
