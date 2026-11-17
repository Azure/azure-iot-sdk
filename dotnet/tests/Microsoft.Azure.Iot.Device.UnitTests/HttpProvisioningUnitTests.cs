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
    /// Tests provisioning over the Device Provisioning Service HTTPS REST API, which the connection client uses for
    /// <see cref="TpmAuthenticationProvider"/> because DPS only supports TPM attestation over HTTPS.
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

        private static readonly TimeSpan s_testTimeout = TimeSpan.FromSeconds(30);

        [Fact]
        public async Task ProvisioningOverHttpRetriesATransientFailureNoSoonerThanTheServiceAsked()
        {
            int requestCount = 0;
            FakeDpsHandler dps = new(request =>
            {
                if (Interlocked.Increment(ref requestCount) == 1)
                {
                    HttpResponseMessage throttled = Respond(HttpStatusCode.ServiceUnavailable, "{}");
                    throttled.Headers.RetryAfter = new(TimeSpan.FromSeconds(1));
                    return throttled;
                }

                return !request.Headers.Contains("Authorization")
                    ? Respond(HttpStatusCode.Unauthorized, new JsonObject { ["authenticationKey"] = Convert.ToBase64String(PrimaryKey) }.ToJsonString())
                    : Respond(HttpStatusCode.OK, AssignedTpm(PrimaryKey));
            });

            using MockConnectionMqttClient mockMqttClient = CreateAcceptingMqttClient();
            ImmediateRetryPolicy retryPolicy = new();
            using TestConnectionClient connectionClient = CreateClient(mockMqttClient, dps, retryPolicy);

            Stopwatch stopwatch = Stopwatch.StartNew();
            await connectionClient
                .ProvisionAndConnectAsync(CreateSettings(), new TpmAuthenticationProvider(RegistrationId, IdScope, new FakeTpm()), cancellationToken: TestContext.Current.CancellationToken)
                .WaitAsync(s_testTimeout, TestContext.Current.CancellationToken);

            // Throttled, challenged, then registered.
            Assert.Equal(3, dps.Requests.Count);
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
                    .ProvisionAndConnectAsync(CreateSettings(), new TpmAuthenticationProvider(RegistrationId, IdScope, new FakeTpm()), cancellationToken: TestContext.Current.CancellationToken)
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
                new TpmAuthenticationProvider(RegistrationId, IdScope, new FakeTpm()),
                cancellationToken: TestContext.Current.CancellationToken);

            await requestReceived.Task.WaitAsync(s_testTimeout, TestContext.Current.CancellationToken);
            await connectionClient.DisconnectAsync(cancellationToken: TestContext.Current.CancellationToken);

            await Assert.ThrowsAnyAsync<OperationCanceledException>(
                async () => await provisioning.WaitAsync(s_testTimeout, TestContext.Current.CancellationToken));
        }

        [Fact]
        public async Task TpmProvisioningAnswersTheChallengeAndAuthenticatesWithTheImportedKeys()
        {
            byte[] dpsKey = Encoding.UTF8.GetBytes("some-dps-key-for-unit-tests");
            byte[] hubKey = Encoding.UTF8.GetBytes("some-hub-key-for-unit-tests");
            FakeDpsHandler dps = new(request =>
            {
                if (request.Method == HttpMethod.Put && !request.Headers.Contains("Authorization"))
                {
                    return Respond(HttpStatusCode.Unauthorized, new JsonObject
                    {
                        ["authenticationKey"] = Convert.ToBase64String(dpsKey),
                        ["keyName"] = "someKeyName",
                    }.ToJsonString());
                }

                return request.Method == HttpMethod.Put
                    ? Respond(HttpStatusCode.Accepted, Assigning())
                    : Respond(HttpStatusCode.OK, AssignedTpm(hubKey));
            });

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
            FakeTpm tpm = new();

            ProvisioningSettings settings = CreateSettings();
            settings.ProvisioningPayload = new JsonObject { ["some"] = "value" };

            ConnectionContext context = await connectionClient
                .ProvisionAndConnectAsync(settings, new TpmAuthenticationProvider(RegistrationId, IdScope, tpm), cancellationToken: TestContext.Current.CancellationToken)
                .WaitAsync(s_testTimeout, TestContext.Current.CancellationToken);

            Assert.Equal(AssignedHub, context.IotHubHostName);
            Assert.Equal(DeviceId, context.DeviceId);
            Assert.Equal(3, dps.Requests.Count);

            // The first register request is unauthenticated and carries the TPM's public keys.
            RecordedRequest challenged = dps.Requests[0];
            Assert.Equal(HttpMethod.Put, challenged.Method);
            Assert.Equal($"https://{GlobalDeviceEndpoint}/{IdScope}/registrations/{RegistrationId}/register?api-version=2026-11-02-preview", challenged.Uri);
            Assert.StartsWith("Microsoft.Azure.Devices.Provisioning.Client/", challenged.UserAgent);
            Assert.Null(challenged.Authorization);
            JsonNode body = JsonNode.Parse(challenged.Body!)!;
            Assert.Equal(RegistrationId, (string?)body["registrationId"]);
            Assert.Equal("value", (string?)body["payload"]!["some"]);
            Assert.Equal(Convert.ToBase64String(FakeTpm.EndorsementKey), (string?)body["tpm"]!["endorsementKey"]);
            Assert.Equal(Convert.ToBase64String(FakeTpm.StorageRootKey), (string?)body["tpm"]!["storageRootKey"]);

            // The same register request, then the poll, are signed with the key from the challenge.
            Assert.Equal(HttpMethod.Put, dps.Requests[1].Method);
            Assert.Equal(challenged.Body, dps.Requests[1].Body);
            AssertSignedSasToken(dps.Requests[1].Authorization, dpsKey, $"{IdScope}/registrations/{RegistrationId}", "someKeyName");
            RecordedRequest poll = dps.Requests[2];
            Assert.Equal(HttpMethod.Get, poll.Method);
            Assert.Equal($"https://{GlobalDeviceEndpoint}/{IdScope}/registrations/{RegistrationId}/operations/{OperationId}?api-version=2026-11-02-preview", poll.Uri);
            AssertSignedSasToken(poll.Authorization, dpsKey, $"{IdScope}/registrations/{RegistrationId}", "someKeyName");

            // DPS was never contacted over MQTT: the only CONNECT went to the assigned hub, authenticated with the key
            // from the registration result.
            Assert.Equal(new[] { dpsKey, hubKey }, tpm.ImportedKeys);
            MqttConnect connect = Assert.Single(connects);
            Assert.Equal(AssignedHub, connect.HostName);
            AssertSignedSasToken(Encoding.UTF8.GetString(connect.Password!), hubKey, $"{AssignedHub}/devices/{DeviceId}", null);
        }

        [Fact]
        public async Task TpmProvisioningFailsWithAnIdentityErrorWhenTheAnsweredChallengeIsRejected()
        {
            FakeDpsHandler dps = new(_ => Respond(HttpStatusCode.Unauthorized, new JsonObject
            {
                ["authenticationKey"] = Convert.ToBase64String(PrimaryKey),
            }.ToJsonString()));

            using MockConnectionMqttClient mockMqttClient = CreateAcceptingMqttClient();
            using TestConnectionClient connectionClient = CreateClient(mockMqttClient, dps);
            FakeTpm tpm = new();

            DeviceException exception = await Assert.ThrowsAsync<DeviceException>(
                async () => await connectionClient
                    .ProvisionAndConnectAsync(CreateSettings(), new TpmAuthenticationProvider(RegistrationId, IdScope, tpm), cancellationToken: TestContext.Current.CancellationToken)
                    .WaitAsync(s_testTimeout, TestContext.Current.CancellationToken));

            Assert.Equal(ErrorRetryability.IdentityTerminal, exception.Retryability);
            Assert.Equal(2, dps.Requests.Count);
            Assert.Null(dps.Requests[0].Authorization);
            AssertSignedSasToken(dps.Requests[1].Authorization, PrimaryKey, $"{IdScope}/registrations/{RegistrationId}", "registration");
            Assert.Single(tpm.ImportedKeys);
        }

        [Fact]
        public async Task TpmProvisioningFailsWhenTheAssignmentHasNoKeyForTheHub()
        {
            FakeDpsHandler dps = new(request => !request.Headers.Contains("Authorization")
                ? Respond(HttpStatusCode.Unauthorized, new JsonObject { ["authenticationKey"] = Convert.ToBase64String(PrimaryKey) }.ToJsonString())
                : Respond(HttpStatusCode.OK, Assigned()));

            using MockConnectionMqttClient mockMqttClient = CreateAcceptingMqttClient();
            using TestConnectionClient connectionClient = CreateClient(mockMqttClient, dps);

            Exception exception = await Assert.ThrowsAnyAsync<Exception>(
                async () => await connectionClient
                    .ProvisionAndConnectAsync(CreateSettings(), new TpmAuthenticationProvider(RegistrationId, IdScope, new FakeTpm()), cancellationToken: TestContext.Current.CancellationToken)
                    .WaitAsync(s_testTimeout, TestContext.Current.CancellationToken));

            Assert.Contains("without the key", exception.Message);
        }

        [Fact]
        public async Task TpmProvisioningWithACertificateSigningRequestConnectsToTheHubWithTheIssuedCertificate()
        {
            byte[] dpsKey = Encoding.UTF8.GetBytes("some-dps-key-for-unit-tests");
            using System.Security.Cryptography.RSA privateKey = System.Security.Cryptography.RSA.Create(2048);
            using System.Security.Cryptography.X509Certificates.X509Certificate2 issuedCertificate = new System.Security.Cryptography.X509Certificates.CertificateRequest(
                    $"CN={RegistrationId}", privateKey, System.Security.Cryptography.HashAlgorithmName.SHA256, System.Security.Cryptography.RSASignaturePadding.Pkcs1)
                .CreateSelfSigned(DateTimeOffset.UtcNow.AddDays(-1), DateTimeOffset.UtcNow.AddDays(1));
            string issuedCertificateBase64 = Convert.ToBase64String(issuedCertificate.RawData);

            // The assignment carries the issued certificate but no IoT Hub key, which a certificate-issuing enrollment need not send.
            FakeDpsHandler dps = new(request =>
            {
                if (!request.Headers.Contains("Authorization"))
                {
                    return Respond(HttpStatusCode.Unauthorized, new JsonObject { ["authenticationKey"] = Convert.ToBase64String(dpsKey) }.ToJsonString());
                }

                RegistrationOperationStatus assigned = Assigned();
                assigned.RegistrationState!.IssuedClientCertificateChain = new[] { issuedCertificateBase64 };
                return Respond(HttpStatusCode.OK, assigned);
            });

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
            FakeTpm tpm = new();
            ProvisioningSettings settings = CreateSettings();
            settings.CertificateSigningRequest = new(privateKey, "someCsr");

            ConnectionContext context = await connectionClient
                .ProvisionAndConnectAsync(settings, new TpmAuthenticationProvider(RegistrationId, IdScope, tpm, privateKey), cancellationToken: TestContext.Current.CancellationToken)
                .WaitAsync(s_testTimeout, TestContext.Current.CancellationToken);

            // The register request is TPM-attested and carries the certificate signing request.
            JsonNode body = JsonNode.Parse(dps.Requests[^1].Body!)!;
            Assert.Equal("someCsr", (string?)body["csr"]);
            Assert.NotNull(body["tpm"]);
            AssertSignedSasToken(dps.Requests[^1].Authorization, dpsKey, $"{IdScope}/registrations/{RegistrationId}", "registration");
            Assert.Equal(new[] { dpsKey }, tpm.ImportedKeys);

            // IoT Hub is authenticated with the issued certificate, not a TPM-signed SAS token.
            Assert.Equal(new[] { issuedCertificateBase64 }, context.IssuedClientCertificates);
            MqttConnect connect = Assert.Single(connects);
            Assert.NotNull(connect.ClientCertificate);
            Assert.Equal(issuedCertificate.Thumbprint, connect.ClientCertificate.Thumbprint);
            Assert.True(connect.ClientCertificate.HasPrivateKey);
            Assert.True(connect.Password == null || connect.Password.Length == 0);
        }

        private static void AssertSignedSasToken(string? authorization, byte[] key, string target, string? keyName)
        {
            Assert.NotNull(authorization);
            const string prefix = "SharedAccessSignature ";
            Assert.StartsWith(prefix, authorization);
            Dictionary<string, string> fields = authorization[prefix.Length..]
                .Split('&')
                .Select(field => field.Split('=', 2))
                .ToDictionary(field => field[0], field => WebUtility.UrlDecode(field[1]));

            Assert.Equal(target, fields["sr"]);
            using var hmac = new System.Security.Cryptography.HMACSHA256(key);
            string expectedSignature = Convert.ToBase64String(hmac.ComputeHash(Encoding.UTF8.GetBytes($"{WebUtility.UrlEncode(target)}\n{fields["se"]}")));
            Assert.Equal(expectedSignature, fields["sig"]);

            if (keyName == null)
            {
                Assert.False(fields.ContainsKey("skn"));
            }
            else
            {
                Assert.Equal(keyName, fields["skn"]);
            }
        }

        private static RegistrationOperationStatus AssignedTpm(byte[] hubKey)
        {
            RegistrationOperationStatus status = Assigned();
            status.RegistrationState!.Tpm = new TpmRegistrationResult { AuthenticationKey = Convert.ToBase64String(hubKey) };
            return status;
        }

        /// <summary>
        /// Stands in for a TPM whose imported keys are not encrypted, so that each key it signs with is the one sent to it.
        /// </summary>
        private sealed class FakeTpm : ITpmProvider
        {
            public static readonly byte[] EndorsementKey = Encoding.UTF8.GetBytes("someEndorsementKey");
            public static readonly byte[] StorageRootKey = Encoding.UTF8.GetBytes("someStorageRootKey");

            private byte[]? _key;

            public List<byte[]> ImportedKeys { get; } = new();

            public Task<byte[]> GetEndorsementKeyAsync(CancellationToken cancellationToken = default) => Task.FromResult(EndorsementKey);

            public Task<byte[]> GetStorageRootKeyAsync(CancellationToken cancellationToken = default) => Task.FromResult(StorageRootKey);

            public Task ImportKeyAsync(byte[] encryptedKey, CancellationToken cancellationToken = default)
            {
                ImportedKeys.Add(encryptedKey);
                _key = encryptedKey;
                return Task.CompletedTask;
            }

            public Task<byte[]> SignAsync(byte[] data, CancellationToken cancellationToken = default)
            {
                using var hmac = new System.Security.Cryptography.HMACSHA256(_key ?? throw new InvalidOperationException("No key was imported."));
                return Task.FromResult(hmac.ComputeHash(data));
            }
        }

        private static ProvisioningSettings CreateSettings() => new(IdScope)
        {
            RegistrationId = RegistrationId,
            GlobalEndpointAddress = GlobalDeviceEndpoint,
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
