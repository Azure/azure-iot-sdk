// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Authentication;
using Microsoft.Azure.Iot.Device.Exceptions;
using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Mqtt;
using Microsoft.Azure.Iot.Device.Provisioning.Models;
using Microsoft.Azure.Iot.Device.Retry;
using System.Collections.Concurrent;
using System.Diagnostics;
using System.Net;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using Xunit;

namespace Microsoft.Azure.Iot.Device.UnitTests
{
    /// <summary>
    /// Tests provisioning over the Device Provisioning Service HTTPS REST API, selected by
    /// <see cref="ProvisioningSettings.Transport"/>.
    /// </summary>
    public class HttpProvisioningUnitTests
    {
        private const string GlobalDeviceEndpoint = "global.azure-devices-provisioning.net";
        private const string IdScope = "0ne00000000";
        private const string RegistrationId = "someRegistrationId";
        private const string DeviceId = "someDeviceId";
        private const string AssignedHub = "some-hub.azure-devices.net";
        private const string OperationId = "someOperationId";
        private static readonly byte[] PrimaryKey = Encoding.UTF8.GetBytes("some-primary-key-for-unit-tests");
        private static readonly byte[] SecondaryKey = Encoding.UTF8.GetBytes("some-secondary-key-for-unit-tests");

        private static readonly TimeSpan s_testTimeout = TimeSpan.FromSeconds(30);

        [Fact]
        public async Task ProvisioningOverHttpRegistersPollsAndConnectsToTheAssignedHubOverMqtt()
        {
            FakeDpsHandler dps = new(request => request.Method == HttpMethod.Put
                ? Respond(HttpStatusCode.Accepted, Assigning())
                : Respond(HttpStatusCode.OK, Assigned()));

            List<MqttConnect> connects = new();
            using MockConnectionMqttClient mockMqttClient = new();
            mockMqttClient.OnConnect = connect =>
            {
                lock (connects)
                {
                    connects.Add(connect);
                }

                return Task.FromResult(new MqttConnectAck() { ResultCode = MqttConnectReasonCode.Success });
            };

            using TestConnectionClient connectionClient = CreateClient(mockMqttClient, dps);

            ProvisioningSettings settings = CreateSettings();
            settings.ProvisioningPayload = new JsonObject { ["some"] = "value" };
            settings.CertificateSigningRequest = new(System.Security.Cryptography.RSA.Create(2048), "someCsr");

            ConnectionContext context = await connectionClient
                .ProvisionAndConnectAsync(settings, new SymmetricKeyAuthenticationProvider(RegistrationId, IdScope, PrimaryKey, null), cancellationToken: TestContext.Current.CancellationToken)
                .WaitAsync(s_testTimeout, TestContext.Current.CancellationToken);

            Assert.Equal(AssignedHub, context.IotHubHostName);
            Assert.Equal(DeviceId, context.DeviceId);

            Assert.Equal(2, dps.Requests.Count);
            RecordedRequest register = dps.Requests[0];
            Assert.Equal(HttpMethod.Put, register.Method);
            Assert.Equal($"https://{GlobalDeviceEndpoint}/{IdScope}/registrations/{RegistrationId}/register?api-version=2026-11-02-preview", register.Uri);
            Assert.StartsWith("SharedAccessSignature ", register.Authorization);
            Assert.Contains("skn=registration", register.Authorization);
            Assert.StartsWith("Microsoft.Azure.Devices.Provisioning.Client/", register.UserAgent);

            JsonNode body = JsonNode.Parse(register.Body!)!;
            Assert.Equal(RegistrationId, (string?)body["registrationId"]);
            Assert.Equal("value", (string?)body["payload"]!["some"]);
            Assert.Equal("someCsr", (string?)body["csr"]);

            RecordedRequest poll = dps.Requests[1];
            Assert.Equal(HttpMethod.Get, poll.Method);
            Assert.Equal($"https://{GlobalDeviceEndpoint}/{IdScope}/registrations/{RegistrationId}/operations/{OperationId}?api-version=2026-11-02-preview", poll.Uri);
            Assert.NotNull(poll.Authorization);

            // DPS was never contacted over MQTT: the only CONNECT went to the assigned hub.
            MqttConnect onlyConnect = Assert.Single(connects);
            Assert.Equal(AssignedHub, onlyConnect.HostName);
        }

        [Fact]
        public async Task ProvisioningOverHttpRetriesWithTheSecondaryKeyWhenThePrimaryIsRejected()
        {
            int requestCount = 0;
            FakeDpsHandler dps = new(_ => Interlocked.Increment(ref requestCount) == 1
                ? Respond(HttpStatusCode.Unauthorized, "{\"errorCode\":401002}")
                : Respond(HttpStatusCode.OK, Assigned()));

            using MockConnectionMqttClient mockMqttClient = CreateAcceptingMqttClient();
            using TestConnectionClient connectionClient = CreateClient(mockMqttClient, dps);

            await connectionClient
                .ProvisionAndConnectAsync(CreateSettings(), new SymmetricKeyAuthenticationProvider(RegistrationId, IdScope, PrimaryKey, SecondaryKey), cancellationToken: TestContext.Current.CancellationToken)
                .WaitAsync(s_testTimeout, TestContext.Current.CancellationToken);

            Assert.Equal(2, dps.Requests.Count);
            Assert.NotEqual(dps.Requests[0].Authorization, dps.Requests[1].Authorization);
        }

        [Fact]
        public async Task ProvisioningOverHttpFailsWithAnIdentityErrorWhenThereIsNoSecondaryKey()
        {
            FakeDpsHandler dps = new(_ => Respond(HttpStatusCode.Unauthorized, "{\"errorCode\":401002}"));

            using MockConnectionMqttClient mockMqttClient = CreateAcceptingMqttClient();
            using TestConnectionClient connectionClient = CreateClient(mockMqttClient, dps);

            DeviceException exception = await Assert.ThrowsAsync<DeviceException>(
                async () => await connectionClient
                    .ProvisionAndConnectAsync(CreateSettings(), new SymmetricKeyAuthenticationProvider(RegistrationId, IdScope, PrimaryKey, null), cancellationToken: TestContext.Current.CancellationToken)
                    .WaitAsync(s_testTimeout, TestContext.Current.CancellationToken));

            Assert.Equal(ErrorRetryability.IdentityTerminal, exception.Retryability);
            Assert.Contains("401002", exception.Message);
            Assert.Single(dps.Requests);
        }

        [Fact]
        public async Task ProvisioningOverHttpRetriesATransientFailureNoSoonerThanTheServiceAsked()
        {
            int requestCount = 0;
            FakeDpsHandler dps = new(_ =>
            {
                if (Interlocked.Increment(ref requestCount) == 1)
                {
                    HttpResponseMessage throttled = Respond(HttpStatusCode.ServiceUnavailable, "{}");
                    throttled.Headers.RetryAfter = new(TimeSpan.FromSeconds(1));
                    return throttled;
                }

                return Respond(HttpStatusCode.OK, Assigned());
            });

            using MockConnectionMqttClient mockMqttClient = CreateAcceptingMqttClient();
            ImmediateRetryPolicy retryPolicy = new();
            using TestConnectionClient connectionClient = CreateClient(mockMqttClient, dps, retryPolicy);

            Stopwatch stopwatch = Stopwatch.StartNew();
            await connectionClient
                .ProvisionAndConnectAsync(CreateSettings(), new SymmetricKeyAuthenticationProvider(RegistrationId, IdScope, PrimaryKey, null), cancellationToken: TestContext.Current.CancellationToken)
                .WaitAsync(s_testTimeout, TestContext.Current.CancellationToken);

            Assert.Equal(2, dps.Requests.Count);
            Assert.Contains(ConnectionEndpoint.DeviceProvisioningService, retryPolicy.Endpoints);

            // The policy asked for no delay; the service's Retry-After is what held the retry back.
            Assert.True(stopwatch.Elapsed >= TimeSpan.FromMilliseconds(900), $"Retried after only {stopwatch.Elapsed}.");
        }

        [Fact]
        public async Task ProvisioningOverHttpReportsWhatTheServiceSaidWhenItRefusesTheRegistration()
        {
            const string errorBody = "{\"errorCode\":400004,\"message\":\"Bad request\"}";
            FakeDpsHandler dps = new(_ => Respond(HttpStatusCode.BadRequest, errorBody));

            using MockConnectionMqttClient mockMqttClient = CreateAcceptingMqttClient();
            using TestConnectionClient connectionClient = CreateClient(mockMqttClient, dps);

            Exception exception = await Assert.ThrowsAnyAsync<Exception>(
                async () => await connectionClient
                    .ProvisionAndConnectAsync(CreateSettings(), new SymmetricKeyAuthenticationProvider(RegistrationId, IdScope, PrimaryKey, null), cancellationToken: TestContext.Current.CancellationToken)
                    .WaitAsync(s_testTimeout, TestContext.Current.CancellationToken));

            Assert.Contains("400", exception.Message);
            Assert.Contains(errorBody, exception.Message);
            Assert.Single(dps.Requests);
        }

        [Fact]
        public async Task DisconnectCancelsProvisioningOverHttp()
        {
            TaskCompletionSource requestReceived = new(TaskCreationOptions.RunContinuationsAsynchronously);
            FakeDpsHandler dps = new(async (_, cancellationToken) =>
            {
                requestReceived.TrySetResult();
                await Task.Delay(Timeout.Infinite, cancellationToken);
                throw new UnreachableException();
            });

            using MockConnectionMqttClient mockMqttClient = CreateAcceptingMqttClient();
            using TestConnectionClient connectionClient = CreateClient(mockMqttClient, dps);

            Task<ConnectionContext> provisioning = connectionClient.ProvisionAndConnectAsync(
                CreateSettings(),
                new SymmetricKeyAuthenticationProvider(RegistrationId, IdScope, PrimaryKey, null),
                cancellationToken: TestContext.Current.CancellationToken);

            await requestReceived.Task.WaitAsync(s_testTimeout, TestContext.Current.CancellationToken);
            await connectionClient.DisconnectAsync(cancellationToken: TestContext.Current.CancellationToken);

            await Assert.ThrowsAnyAsync<OperationCanceledException>(
                async () => await provisioning.WaitAsync(s_testTimeout, TestContext.Current.CancellationToken));
        }

        private static ProvisioningSettings CreateSettings() => new(IdScope)
        {
            RegistrationId = RegistrationId,
            GlobalEndpointAddress = GlobalDeviceEndpoint,
            Transport = ProvisioningTransport.Http,
        };

        private static TestConnectionClient CreateClient(MockConnectionMqttClient mqttClient, FakeDpsHandler dps, IRetryPolicy? retryPolicy = null)
        {
            Device.Unified.Models.ConnectionClientOptions options = new() { MqttClient = mqttClient };
            if (retryPolicy != null)
            {
                options.ConnectionRetryPolicy = retryPolicy;
            }

            return new TestConnectionClient(options)
            {
                CreateProvisioningHttpMessageHandler = _ => dps,
            };
        }

        private static MockConnectionMqttClient CreateAcceptingMqttClient()
        {
            MockConnectionMqttClient mockMqttClient = new();
            mockMqttClient.OnConnect = _ => Task.FromResult(new MqttConnectAck() { ResultCode = MqttConnectReasonCode.Success });
            return mockMqttClient;
        }

        private static RegistrationOperationStatus Assigning() => new()
        {
            OperationId = OperationId,
            Status = ProvisioningRegistrationStatus.Assigning,
        };

        private static RegistrationOperationStatus Assigned() => new()
        {
            OperationId = OperationId,
            Status = ProvisioningRegistrationStatus.Assigned,
            RegistrationState = new DeviceRegistrationResult()
            {
                RegistrationId = RegistrationId,
                DeviceId = DeviceId,
                AssignedHub = AssignedHub,
                Status = ProvisioningRegistrationStatus.Assigned,
            },
        };

        private static HttpResponseMessage Respond(HttpStatusCode statusCode, RegistrationOperationStatus status)
        {
            return Respond(statusCode, JsonSerializer.Serialize(status, JsonSerializationSettings.Options));
        }

        private static HttpResponseMessage Respond(HttpStatusCode statusCode, string body)
        {
            return new HttpResponseMessage(statusCode)
            {
                Content = new StringContent(body, Encoding.UTF8, "application/json"),
            };
        }

        private sealed record RecordedRequest(HttpMethod Method, string Uri, string? Authorization, string? UserAgent, string? Body);

        /// <summary>
        /// Stands in for Device Provisioning Service, recording each request before answering it. The connection client
                /// disposes each handler it is given; disposing an <see cref="HttpMessageHandler"/> is a no-op, so this one
                /// can serve every attempt.
        /// </summary>
        private sealed class FakeDpsHandler : HttpMessageHandler
        {
            private readonly Func<HttpRequestMessage, CancellationToken, Task<HttpResponseMessage>> _respond;
            private readonly ConcurrentQueue<RecordedRequest> _requests = new();

            public FakeDpsHandler(Func<HttpRequestMessage, HttpResponseMessage> respond)
                : this((request, _) => Task.FromResult(respond(request)))
            {
            }

            public FakeDpsHandler(Func<HttpRequestMessage, CancellationToken, Task<HttpResponseMessage>> respond)
            {
                _respond = respond;
            }

            public IReadOnlyList<RecordedRequest> Requests => _requests.ToArray();

            protected override async Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken cancellationToken)
            {
                _requests.Enqueue(new RecordedRequest(
                    request.Method,
                    request.RequestUri!.ToString(),
                    request.Headers.TryGetValues("Authorization", out IEnumerable<string>? authorization) ? authorization.Single() : null,
                    request.Headers.TryGetValues("User-Agent", out IEnumerable<string>? userAgent) ? string.Join(" ", userAgent) : null,
                    request.Content == null ? null : await request.Content.ReadAsStringAsync(cancellationToken)));

                return await _respond(request, cancellationToken);
            }
        }

        private sealed class ImmediateRetryPolicy : IRetryPolicy
        {
            public ConcurrentQueue<ConnectionEndpoint> Endpoints { get; } = new();

            public RetryGuidance GetRetryGuidance(uint currentRetryCount, Exception? lastException, ConnectionEndpoint connectionEndpoint, out TimeSpan retryDelay)
            {
                Endpoints.Enqueue(connectionEndpoint);
                retryDelay = TimeSpan.Zero;
                return RetryGuidance.Retry;
            }
        }
    }
}
