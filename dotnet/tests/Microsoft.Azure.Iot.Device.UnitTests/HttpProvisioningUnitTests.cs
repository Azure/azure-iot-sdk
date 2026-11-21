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
        public async Task ProvisioningOverHttpGoesThroughTheConfiguredProxy()
        {
            using LoopbackConnectProxy proxy = new();
            using MockConnectionMqttClient mockMqttClient = CreateAcceptingMqttClient();

            // The default handler factory is kept so that the proxy is applied the way it is in production.
            using TestConnectionClient connectionClient = new(new Device.Unified.Models.ConnectionClientOptions
            {
                MqttClient = mockMqttClient,
                Proxy = new WebProxy(proxy.Address),
                ConnectionRetryPolicy = new NoRetry(),
            });

            await Assert.ThrowsAnyAsync<Exception>(
                async () => await connectionClient
                    .ProvisionAndConnectAsync(CreateSettings(), new TpmAuthenticationProvider(RegistrationId, IdScope, new FakeTpm()), cancellationToken: TestContext.Current.CancellationToken)
                    .WaitAsync(s_testTimeout, TestContext.Current.CancellationToken));

            Assert.Contains($"CONNECT {GlobalDeviceEndpoint}:443 HTTP/1.1", proxy.RequestLines);
            Assert.Equal(0, mockMqttClient.ConnectAttemptCount);
        }

        [Fact]
        public async Task ProvisioningOverHttpCreatesEachAttemptsHttpClientWithTheSuppliedFactoryAndDisposesIt()
        {
            int requestCount = 0;
            FakeDpsHandler dps = new(request =>
            {
                if (Interlocked.Increment(ref requestCount) == 1)
                {
                    return Respond(HttpStatusCode.ServiceUnavailable, "{}");
                }

                return !request.Headers.Contains("Authorization")
                    ? Respond(HttpStatusCode.Unauthorized, new JsonObject { ["authenticationKey"] = Convert.ToBase64String(PrimaryKey) }.ToJsonString())
                    : Respond(HttpStatusCode.OK, AssignedTpm(PrimaryKey));
            });
            ConcurrentQueue<DisposalTrackingHttpClient> httpClients = new();

            using MockConnectionMqttClient mockMqttClient = CreateAcceptingMqttClient();
            using TestConnectionClient connectionClient = new(new Device.Unified.Models.ConnectionClientOptions
            {
                MqttClient = mockMqttClient,
                ConnectionRetryPolicy = new ImmediateRetryPolicy(),
                HttpClientFactory = _ =>
                {
                    DisposalTrackingHttpClient httpClient = new(dps);
                    httpClients.Enqueue(httpClient);
                    return httpClient;
                },
            });

            await connectionClient
                .ProvisionAndConnectAsync(CreateSettings(), new TpmAuthenticationProvider(RegistrationId, IdScope, new FakeTpm()), cancellationToken: TestContext.Current.CancellationToken)
                .WaitAsync(s_testTimeout, TestContext.Current.CancellationToken);

            // The throttled attempt and the attempt that was challenged then registered each got their own HTTP client.
            Assert.Equal(3, dps.Requests.Count);
            Assert.Equal(2, httpClients.Count);
            Assert.All(httpClients, httpClient => Assert.True(httpClient.IsDisposed));
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
        public async Task DisconnectDuringTheFinalTpmKeyImportDoesNotConnectToTheHub()
        {
            byte[] hubKey = Encoding.UTF8.GetBytes("some-hub-key-for-unit-tests");
            FakeDpsHandler dps = new(request => !request.Headers.Contains("Authorization")
                ? Respond(HttpStatusCode.Unauthorized, new JsonObject { ["authenticationKey"] = Convert.ToBase64String(PrimaryKey) }.ToJsonString())
                : Respond(HttpStatusCode.OK, AssignedTpm(hubKey)));

            TaskCompletionSource hubKeyImportStarted = new(TaskCreationOptions.RunContinuationsAsynchronously);
            TaskCompletionSource finishHubKeyImport = new(TaskCreationOptions.RunContinuationsAsynchronously);
            FakeTpm tpm = new()
            {
                OnImport = key =>
                {
                    if (!key.SequenceEqual(hubKey))
                    {
                        return Task.CompletedTask;
                    }

                    hubKeyImportStarted.TrySetResult();
                    return finishHubKeyImport.Task;
                },
            };

            using MockConnectionMqttClient mockMqttClient = CreateAcceptingMqttClient();
            using TestConnectionClient connectionClient = CreateClient(mockMqttClient, dps);

            Task<ConnectionContext> provisioning = connectionClient.ProvisionAndConnectAsync(
                CreateSettings(),
                new TpmAuthenticationProvider(RegistrationId, IdScope, tpm),
                cancellationToken: TestContext.Current.CancellationToken);

            await hubKeyImportStarted.Task.WaitAsync(s_testTimeout, TestContext.Current.CancellationToken);
            await connectionClient.DisconnectAsync(cancellationToken: TestContext.Current.CancellationToken);

            // The TPM finishes importing the key regardless, but the disconnect already completed.
            finishHubKeyImport.SetResult();

            await Assert.ThrowsAnyAsync<OperationCanceledException>(
                async () => await provisioning.WaitAsync(s_testTimeout, TestContext.Current.CancellationToken));
            Assert.Equal(0, mockMqttClient.ConnectAttemptCount);
        }

        [Fact]
        public async Task ARunCanceledDuringTheFinalTpmKeyImportLeavesTheRunThatReplacedItAlone()
        {
            byte[] firstHubKey = Encoding.UTF8.GetBytes("some-first-hub-key-for-unit-tests");
            byte[] secondHubKey = Encoding.UTF8.GetBytes("some-second-hub-key-for-unit-tests");
            int registrations = 0;
            TaskCompletionSource secondRegistrationReceived = new(TaskCreationOptions.RunContinuationsAsynchronously);
            TaskCompletionSource finishSecondRegistration = new(TaskCreationOptions.RunContinuationsAsynchronously);
            FakeDpsHandler dps = new(async (request, cancellationToken) =>
            {
                if (!request.Headers.Contains("Authorization"))
                {
                    return Respond(HttpStatusCode.Unauthorized, new JsonObject { ["authenticationKey"] = Convert.ToBase64String(PrimaryKey) }.ToJsonString());
                }

                if (Interlocked.Increment(ref registrations) == 1)
                {
                    return Respond(HttpStatusCode.OK, AssignedTpm(firstHubKey));
                }

                secondRegistrationReceived.TrySetResult();
                await finishSecondRegistration.Task.WaitAsync(cancellationToken);
                return Respond(HttpStatusCode.OK, AssignedTpm(secondHubKey));
            });

            TaskCompletionSource firstHubKeyImportStarted = new(TaskCreationOptions.RunContinuationsAsynchronously);
            TaskCompletionSource finishFirstHubKeyImport = new(TaskCreationOptions.RunContinuationsAsynchronously);
            FakeTpm firstTpm = new()
            {
                OnImport = key =>
                {
                    if (!key.SequenceEqual(firstHubKey))
                    {
                        return Task.CompletedTask;
                    }

                    firstHubKeyImportStarted.TrySetResult();
                    return finishFirstHubKeyImport.Task;
                },
            };

            using MockConnectionMqttClient mockMqttClient = CreateAcceptingMqttClient();
            using TestConnectionClient connectionClient = CreateClient(mockMqttClient, dps);

            Task<ConnectionContext> firstProvisioning = connectionClient.ProvisionAndConnectAsync(
                CreateSettings(),
                new TpmAuthenticationProvider(RegistrationId, IdScope, firstTpm),
                cancellationToken: TestContext.Current.CancellationToken);

            await firstHubKeyImportStarted.Task.WaitAsync(s_testTimeout, TestContext.Current.CancellationToken);
            await connectionClient.DisconnectAsync(cancellationToken: TestContext.Current.CancellationToken);

            // The application provisions again while the TPM is still importing the first run's key.
            Task<ConnectionContext> secondProvisioning = connectionClient.ProvisionAndConnectAsync(
                CreateSettings(),
                new TpmAuthenticationProvider(RegistrationId, IdScope, new FakeTpm()),
                cancellationToken: TestContext.Current.CancellationToken);
            await secondRegistrationReceived.Task.WaitAsync(s_testTimeout, TestContext.Current.CancellationToken);

            finishFirstHubKeyImport.SetResult();
            await Assert.ThrowsAnyAsync<OperationCanceledException>(
                async () => await firstProvisioning.WaitAsync(s_testTimeout, TestContext.Current.CancellationToken));

            // The first run ending did not take the endpoint away from the run that replaced it.
            Assert.Equal(ConnectionEndpoint.DeviceProvisioningService, connectionClient.CurrentEndpoint);

            finishSecondRegistration.SetResult();
            ConnectionContext context = await secondProvisioning.WaitAsync(s_testTimeout, TestContext.Current.CancellationToken);
            Assert.Equal(AssignedHub, context.IotHubHostName);
            Assert.Equal(ConnectionEndpoint.IotHub, connectionClient.CurrentEndpoint);
            Assert.Equal(1, mockMqttClient.ConnectAttemptCount);
        }

        [Fact]
        public async Task IotHubTokensWaitForTheIotHubKeyWhileRegistrationHoldsTheChallengeKey()
        {
            byte[] dpsKey = Encoding.UTF8.GetBytes("some-dps-key-for-unit-tests");
            byte[] hubKey = Encoding.UTF8.GetBytes("some-hub-key-for-unit-tests");
            FakeTpm tpm = new();
            TpmAuthenticationProvider authentication = new(RegistrationId, IdScope, tpm);
            await authentication.ImportIotHubKeyAsync(PrimaryKey, TestContext.Current.CancellationToken);

            await authentication.ImportProvisioningKeyAsync(dpsKey, TestContext.Current.CancellationToken);
            Task<AuthenticationCredentials> credentials = authentication.GetPrimaryOperationalAuthenticationCredentialsAsync(
                AssignedHub, DeviceId, null, TestContext.Current.CancellationToken);

            await Task.Delay(100, TestContext.Current.CancellationToken);
            Assert.False(credentials.IsCompleted);

            await authentication.ImportIotHubKeyAsync(hubKey, TestContext.Current.CancellationToken);
            AuthenticationCredentials signed = await credentials.WaitAsync(s_testTimeout, TestContext.Current.CancellationToken);
            AssertSignedSasToken(Encoding.UTF8.GetString(signed.Password!), hubKey, $"{AssignedHub}/devices/{DeviceId}", null);
        }

        [Fact]
        public async Task IotHubTokensFailUntilAnIotHubKeyIsImportedWhenRegistrationEndsWithoutOne()
        {
            FakeDpsHandler dps = new(_ => Respond(HttpStatusCode.Unauthorized, new JsonObject
            {
                ["authenticationKey"] = Convert.ToBase64String(PrimaryKey),
            }.ToJsonString()));

            using MockConnectionMqttClient mockMqttClient = CreateAcceptingMqttClient();
            using TestConnectionClient connectionClient = CreateClient(mockMqttClient, dps);
            TpmAuthenticationProvider authentication = new(RegistrationId, IdScope, new FakeTpm());

            await Assert.ThrowsAsync<DeviceException>(
                async () => await connectionClient
                    .ProvisionAndConnectAsync(CreateSettings(), authentication, cancellationToken: TestContext.Current.CancellationToken)
                    .WaitAsync(s_testTimeout, TestContext.Current.CancellationToken));

            // Nothing is left waiting for a key that registration will never install, and nothing is signed with the
            // challenge key the TPM still holds.
            DeviceException exception = await Assert.ThrowsAsync<DeviceException>(
                async () => await authentication
                    .GetPrimaryOperationalAuthenticationCredentialsAsync(AssignedHub, DeviceId, null, TestContext.Current.CancellationToken)
                    .WaitAsync(s_testTimeout, TestContext.Current.CancellationToken));
            Assert.Equal(ErrorRetryability.IdentityTerminal, exception.Retryability);

            byte[] hubKey = Encoding.UTF8.GetBytes("some-hub-key-for-unit-tests");
            await authentication.ImportIotHubKeyAsync(hubKey, TestContext.Current.CancellationToken);
            AuthenticationCredentials credentials = await authentication
                .GetPrimaryOperationalAuthenticationCredentialsAsync(AssignedHub, DeviceId, null, TestContext.Current.CancellationToken)
                .WaitAsync(s_testTimeout, TestContext.Current.CancellationToken);
            AssertSignedSasToken(Encoding.UTF8.GetString(credentials.Password!), hubKey, $"{AssignedHub}/devices/{DeviceId}", null);
        }

        [Fact]
        public async Task IotHubTokensWaitingForTheIotHubKeyFailWhenRegistrationEndsWithoutOne()
        {
            byte[] dpsKey = Encoding.UTF8.GetBytes("some-dps-key-for-unit-tests");
            TpmAuthenticationProvider authentication = new(RegistrationId, IdScope, new FakeTpm());
            await authentication.ImportIotHubKeyAsync(PrimaryKey, TestContext.Current.CancellationToken);

            await authentication.ImportProvisioningKeyAsync(dpsKey, TestContext.Current.CancellationToken);
            Task<AuthenticationCredentials> credentials = authentication.GetPrimaryOperationalAuthenticationCredentialsAsync(
                AssignedHub, DeviceId, null, TestContext.Current.CancellationToken);

            await Task.Delay(100, TestContext.Current.CancellationToken);
            Assert.False(credentials.IsCompleted);

            authentication.EndProvisioningKeyUse();
            DeviceException exception = await Assert.ThrowsAsync<DeviceException>(
                async () => await credentials.WaitAsync(s_testTimeout, TestContext.Current.CancellationToken));
            Assert.Equal(ErrorRetryability.IdentityTerminal, exception.Retryability);
        }

        [Fact]
        public async Task IotHubTokensThatWaitedForTheIotHubKeyExpireAfterTheirFullLifetime()
        {
            TimeSpan timeToLive = TimeSpan.FromSeconds(2);
            byte[] dpsKey = Encoding.UTF8.GetBytes("some-dps-key-for-unit-tests");
            byte[] hubKey = Encoding.UTF8.GetBytes("some-hub-key-for-unit-tests");
            TpmAuthenticationProvider authentication = new(RegistrationId, IdScope, new FakeTpm(), iotHubSasTokenTimeToLive: timeToLive);

            await authentication.ImportProvisioningKeyAsync(dpsKey, TestContext.Current.CancellationToken);
            Task<AuthenticationCredentials> credentials = authentication.GetPrimaryOperationalAuthenticationCredentialsAsync(
                AssignedHub, DeviceId, null, TestContext.Current.CancellationToken);

            // Registration takes longer than the token's lifetime to install the IoT Hub key.
            await Task.Delay(timeToLive + TimeSpan.FromMilliseconds(500), TestContext.Current.CancellationToken);
            long hubKeyImportedAt = DateTimeOffset.UtcNow.ToUnixTimeSeconds();
            await authentication.ImportIotHubKeyAsync(hubKey, TestContext.Current.CancellationToken);

            AuthenticationCredentials signed = await credentials.WaitAsync(s_testTimeout, TestContext.Current.CancellationToken);
            string sasToken = Encoding.UTF8.GetString(signed.Password!);
            AssertSignedSasToken(sasToken, hubKey, $"{AssignedHub}/devices/{DeviceId}", null);
            long expiresOn = long.Parse(WebUtility.UrlDecode(sasToken.Split('&').Single(field => field.StartsWith("se=", StringComparison.Ordinal))[3..]), System.Globalization.CultureInfo.InvariantCulture);
            Assert.True(expiresOn >= hubKeyImportedAt + (long)timeToLive.TotalSeconds, $"The token expires at {expiresOn}, before its lifetime from when it was signed.");
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

        [Theory]
        [InlineData("identity fault")]
        [InlineData("hub unreachable")]
        public async Task TpmDeviceReprovisionsOverHttpAndConnectsToTheNewlyAssignedHub(string trigger)
        {
            const string secondHub = "second-hub.azure-devices.net";
            byte[] firstHubKey = Encoding.UTF8.GetBytes("some-first-hub-key-for-unit-tests");
            byte[] secondHubKey = Encoding.UTF8.GetBytes("some-second-hub-key-for-unit-tests");

            int registrations = 0;
            FakeDpsHandler dps = new(request =>
            {
                if (!request.Headers.Contains("Authorization"))
                {
                    return Respond(HttpStatusCode.Unauthorized, new JsonObject { ["authenticationKey"] = Convert.ToBase64String(PrimaryKey) }.ToJsonString());
                }

                bool isFirstRegistration = Interlocked.Increment(ref registrations) == 1;
                RegistrationOperationStatus status = AssignedTpm(isFirstRegistration ? firstHubKey : secondHubKey);
                status.RegistrationState!.AssignedHub = isFirstRegistration ? AssignedHub : secondHub;
                return Respond(HttpStatusCode.OK, status);
            });

            TaskCompletionSource connectedToSecondHub = new(TaskCreationOptions.RunContinuationsAsynchronously);
            List<MqttConnect> connects = new();
            using MockConnectionMqttClient mockMqttClient = new();
            mockMqttClient.OnConnect = connect =>
            {
                int firstHubConnects;
                lock (connects)
                {
                    connects.Add(connect);
                    firstHubConnects = connects.Count(c => c.HostName == AssignedHub);
                }

                if (connect.HostName == secondHub)
                {
                    connectedToSecondHub.TrySetResult();
                }
                else if (firstHubConnects > 1)
                {
                    // The first hub no longer accepts this device: it was deleted there, or the hub was vacated.
                    return trigger == "identity fault"
                        ? Task.FromResult(new MqttConnectAck() { ResultCode = MqttConnectReasonCode.NotAuthorized })
                        : throw new Exception("simulated unreachable hub");
                }

                return Task.FromResult(new MqttConnectAck() { ResultCode = MqttConnectReasonCode.Success });
            };

            using TestConnectionClient connectionClient = CreateClient(
                mockMqttClient,
                dps,
                new ImmediateRetryPolicy(maxHubConnectAttemptsBeforeReprovision: trigger == "hub unreachable" ? 3u : 0u));
            FakeTpm tpm = new();

            await connectionClient
                .ProvisionAndConnectAsync(CreateSettings(), new TpmAuthenticationProvider(RegistrationId, IdScope, tpm), cancellationToken: TestContext.Current.CancellationToken)
                .WaitAsync(s_testTimeout, TestContext.Current.CancellationToken);

            await mockMqttClient.SimulateServerDisconnectAsync(trigger == "identity fault" ? MqttDisconnectReason.NotAuthorized : MqttDisconnectReason.ServerBusy);
            await connectedToSecondHub.Task.WaitAsync(s_testTimeout, TestContext.Current.CancellationToken);

            Assert.Equal(secondHub, connectionClient.GetCurrentConnectionContext()!.IotHubHostName);

            // Re-provisioning registered over HTTPS again, answering a fresh TPM challenge, rather than over MQTT.
            Assert.Equal(2, registrations);
            Assert.Equal(4, dps.Requests.Count);
            Assert.Null(dps.Requests[2].Authorization);
            AssertSignedSasToken(dps.Requests[3].Authorization, PrimaryKey, $"{IdScope}/registrations/{RegistrationId}", "registration");

            // The device connected to the new hub with the key that DPS sent for it, and never connected to DPS over MQTT.
            lock (connects)
            {
                Assert.DoesNotContain(connects, connect => connect.HostName == GlobalDeviceEndpoint);
                MqttConnect secondHubConnect = connects.Last();
                Assert.Equal(secondHub, secondHubConnect.HostName);
                AssertSignedSasToken(Encoding.UTF8.GetString(secondHubConnect.Password!), secondHubKey, $"{secondHub}/devices/{DeviceId}", null);
            }

            Assert.Equal(new[] { PrimaryKey, firstHubKey, PrimaryKey, secondHubKey }, tpm.ImportedKeys);
        }

        [Fact]
        public async Task ProvisioningOverHttpPollsUntilAssignedNoSoonerThanTheServiceAsks()
        {
            int polls = 0;
            FakeDpsHandler dps = new(request =>
            {
                if (!request.Headers.Contains("Authorization"))
                {
                    return Respond(HttpStatusCode.Unauthorized, new JsonObject { ["authenticationKey"] = Convert.ToBase64String(PrimaryKey) }.ToJsonString());
                }

                if (request.Method == HttpMethod.Put)
                {
                    return RespondAssigning(retryAfter: TimeSpan.FromSeconds(3));
                }

                // A Retry-After shorter than the minimum polling interval is not honored.
                return Interlocked.Increment(ref polls) == 1
                    ? RespondAssigning(retryAfter: TimeSpan.FromSeconds(1))
                    : Respond(HttpStatusCode.OK, AssignedTpm(PrimaryKey));
            });

            using MockConnectionMqttClient mockMqttClient = CreateAcceptingMqttClient();
            using TestConnectionClient connectionClient = CreateClient(mockMqttClient, dps);

            ConnectionContext context = await connectionClient
                .ProvisionAndConnectAsync(CreateSettings(), new TpmAuthenticationProvider(RegistrationId, IdScope, new FakeTpm()), cancellationToken: TestContext.Current.CancellationToken)
                .WaitAsync(s_testTimeout, TestContext.Current.CancellationToken);

            Assert.Equal(AssignedHub, context.IotHubHostName);

            // Challenged, registered, then polled twice for the same operation.
            Assert.Equal(4, dps.Requests.Count);
            RecordedRequest registered = dps.Requests[1];
            RecordedRequest[] pollRequests = { dps.Requests[2], dps.Requests[3] };
            foreach (RecordedRequest poll in pollRequests)
            {
                Assert.Equal(HttpMethod.Get, poll.Method);
                Assert.Equal($"https://{GlobalDeviceEndpoint}/{IdScope}/registrations/{RegistrationId}/operations/{OperationId}?api-version=2026-11-02-preview", poll.Uri);
                AssertSignedSasToken(poll.Authorization, PrimaryKey, $"{IdScope}/registrations/{RegistrationId}", "registration");
            }

            TimeSpan firstPollDelay = Stopwatch.GetElapsedTime(registered.Timestamp, pollRequests[0].Timestamp);
            TimeSpan secondPollDelay = Stopwatch.GetElapsedTime(pollRequests[0].Timestamp, pollRequests[1].Timestamp);
            Assert.True(firstPollDelay >= TimeSpan.FromMilliseconds(2900), $"Polled {firstPollDelay} after registering, sooner than the service's Retry-After of 3s.");
            Assert.True(secondPollDelay >= TimeSpan.FromMilliseconds(1900), $"Polled again after {secondPollDelay}, sooner than the minimum polling interval of 2s.");
        }

        [Fact]
        public async Task ProvisioningOverHttpRegistersAgainWhenPollingFailsTransiently()
        {
            int polls = 0;
            FakeDpsHandler dps = new(request =>
            {
                if (!request.Headers.Contains("Authorization"))
                {
                    return Respond(HttpStatusCode.Unauthorized, new JsonObject { ["authenticationKey"] = Convert.ToBase64String(PrimaryKey) }.ToJsonString());
                }

                if (request.Method == HttpMethod.Put)
                {
                    return Volatile.Read(ref polls) == 0 ? Respond(HttpStatusCode.Accepted, Assigning()) : Respond(HttpStatusCode.OK, AssignedTpm(PrimaryKey));
                }

                Interlocked.Increment(ref polls);
                return Respond(HttpStatusCode.ServiceUnavailable, "{}");
            });

            using MockConnectionMqttClient mockMqttClient = CreateAcceptingMqttClient();
            ImmediateRetryPolicy retryPolicy = new();
            using TestConnectionClient connectionClient = CreateClient(mockMqttClient, dps, retryPolicy);
            FakeTpm tpm = new();

            ConnectionContext context = await connectionClient
                .ProvisionAndConnectAsync(CreateSettings(), new TpmAuthenticationProvider(RegistrationId, IdScope, tpm), cancellationToken: TestContext.Current.CancellationToken)
                .WaitAsync(s_testTimeout, TestContext.Current.CancellationToken);

            Assert.Equal(AssignedHub, context.IotHubHostName);
            Assert.Contains(ConnectionEndpoint.DeviceProvisioningService, retryPolicy.Endpoints);

            // The failed poll abandoned that registration, so the retry starts over with a fresh TPM challenge.
            Assert.Equal(
                new[] { (HttpMethod.Put, false), (HttpMethod.Put, true), (HttpMethod.Get, true), (HttpMethod.Put, false), (HttpMethod.Put, true) },
                dps.Requests.Select(request => (request.Method, request.Authorization != null)));
        }

        [Fact]
        public async Task ProvisioningOverHttpThrowsTheTransientFailureOnceTheRetryPolicyAbandons()
        {
            FakeDpsHandler dps = new(_ => Respond(HttpStatusCode.ServiceUnavailable, "{}"));
            using MockConnectionMqttClient mockMqttClient = CreateAcceptingMqttClient();
            ImmediateRetryPolicy retryPolicy = new(maxProvisioningRetries: 2);
            using TestConnectionClient connectionClient = CreateClient(mockMqttClient, dps, retryPolicy);

            DeviceException exception = await Assert.ThrowsAsync<DeviceException>(
                async () => await connectionClient
                    .ProvisionAndConnectAsync(CreateSettings(), new TpmAuthenticationProvider(RegistrationId, IdScope, new FakeTpm()), cancellationToken: TestContext.Current.CancellationToken)
                    .WaitAsync(s_testTimeout, TestContext.Current.CancellationToken));

            Assert.Equal(ErrorRetryability.Retryable, exception.Retryability);
            Assert.Contains("503", exception.Message);

            // The first attempt and two retries, after which the policy's third answer abandoned.
            Assert.Equal(3, dps.Requests.Count);
            Assert.Equal(new[] { ConnectionEndpoint.DeviceProvisioningService, ConnectionEndpoint.DeviceProvisioningService, ConnectionEndpoint.DeviceProvisioningService }, retryPolicy.Endpoints);
            Assert.Equal(0, mockMqttClient.ConnectAttemptCount);
        }

        [Fact]
        public async Task TpmReprovisioningOverHttpFaultsTheConnectionOnceTheRetryPolicyAbandons()
        {
            byte[] hubKey = Encoding.UTF8.GetBytes("some-hub-key-for-unit-tests");
            int registrations = 0;
            FakeDpsHandler dps = new(request =>
            {
                if (Volatile.Read(ref registrations) > 0)
                {
                    // Device Provisioning Service is unavailable for as long as the device tries to re-provision.
                    return Respond(HttpStatusCode.ServiceUnavailable, "{}");
                }

                if (!request.Headers.Contains("Authorization"))
                {
                    return Respond(HttpStatusCode.Unauthorized, new JsonObject { ["authenticationKey"] = Convert.ToBase64String(PrimaryKey) }.ToJsonString());
                }

                Interlocked.Increment(ref registrations);
                return Respond(HttpStatusCode.OK, AssignedTpm(hubKey));
            });

            using MockConnectionMqttClient mockMqttClient = new();
            int connects = 0;
            mockMqttClient.OnConnect = _ => Task.FromResult(new MqttConnectAck()
            {
                // The hub accepts this device only once; it rejects it as unauthorized from then on.
                ResultCode = Interlocked.Increment(ref connects) == 1 ? MqttConnectReasonCode.Success : MqttConnectReasonCode.NotAuthorized,
            });

            using TestConnectionClient connectionClient = CreateClient(mockMqttClient, dps, new ImmediateRetryPolicy(maxProvisioningRetries: 2));
            TaskCompletionSource<ConnectionFaultedEventArgs> connectionFaulted = new(TaskCreationOptions.RunContinuationsAsynchronously);
            connectionClient.ConnectionFaultedAsync += args =>
            {
                connectionFaulted.TrySetResult(args);
                return Task.CompletedTask;
            };

            await connectionClient
                .ProvisionAndConnectAsync(CreateSettings(), new TpmAuthenticationProvider(RegistrationId, IdScope, new FakeTpm()), cancellationToken: TestContext.Current.CancellationToken)
                .WaitAsync(s_testTimeout, TestContext.Current.CancellationToken);

            await mockMqttClient.SimulateServerDisconnectAsync(MqttDisconnectReason.NotAuthorized);

            // Re-provisioning is the only way left to recover, so giving up on it tells the application the connection is gone.
            await connectionFaulted.Task.WaitAsync(s_testTimeout, TestContext.Current.CancellationToken);
            Assert.Equal(1, registrations);

            // Re-provisioning owns the retries: the first attempt and two retries each sent one register request.
            Assert.Equal(3, dps.Requests.Skip(2).Count(request => request.Method == HttpMethod.Put));
        }

        [Fact]
        public async Task ProvisioningOverHttpRetriesARequestThatTimesOut()
        {
            int requests = 0;
            FakeDpsHandler dps = new(async (request, cancellationToken) =>
            {
                if (Interlocked.Increment(ref requests) == 1)
                {
                    // Device Provisioning Service never answers the first request.
                    await Task.Delay(Timeout.Infinite, cancellationToken);
                }

                return !request.Headers.Contains("Authorization")
                    ? Respond(HttpStatusCode.Unauthorized, new JsonObject { ["authenticationKey"] = Convert.ToBase64String(PrimaryKey) }.ToJsonString())
                    : Respond(HttpStatusCode.OK, AssignedTpm(PrimaryKey));
            });

            using MockConnectionMqttClient mockMqttClient = CreateAcceptingMqttClient();
            ImmediateRetryPolicy retryPolicy = new();
            using TestConnectionClient connectionClient = CreateClient(mockMqttClient, dps, retryPolicy, connectionAttemptTimeout: TimeSpan.FromMilliseconds(500));

            ConnectionContext context = await connectionClient
                .ProvisionAndConnectAsync(CreateSettings(), new TpmAuthenticationProvider(RegistrationId, IdScope, new FakeTpm()), cancellationToken: TestContext.Current.CancellationToken)
                .WaitAsync(s_testTimeout, TestContext.Current.CancellationToken);

            Assert.Equal(AssignedHub, context.IotHubHostName);
            Assert.Equal(new[] { ConnectionEndpoint.DeviceProvisioningService }, retryPolicy.Endpoints);
            Assert.Equal(3, dps.Requests.Count);
        }

        [Fact]
        public async Task ProvisioningOverHttpDoesNotRetryARequestTheServiceRejectsAsInvalid()
        {
            FakeDpsHandler dps = new(_ => Respond(HttpStatusCode.BadRequest, "{\"errorCode\":400004,\"message\":\"Invalid registration request.\"}"));
            using MockConnectionMqttClient mockMqttClient = CreateAcceptingMqttClient();
            ImmediateRetryPolicy retryPolicy = new();
            using TestConnectionClient connectionClient = CreateClient(mockMqttClient, dps, retryPolicy);

            Exception exception = await Assert.ThrowsAnyAsync<Exception>(
                async () => await connectionClient
                    .ProvisionAndConnectAsync(CreateSettings(), new TpmAuthenticationProvider(RegistrationId, IdScope, new FakeTpm()), cancellationToken: TestContext.Current.CancellationToken)
                    .WaitAsync(s_testTimeout, TestContext.Current.CancellationToken));

            Assert.Contains("400", exception.Message);
            Assert.Single(dps.Requests);
            Assert.Empty(retryPolicy.Endpoints);
        }

        [Theory]
        [InlineData("failed")]
        [InlineData("no hub")]
        [InlineData("no device id")]
        [InlineData("unsupported connection profile")]
        public async Task ProvisioningOverHttpThatDoesNotAssignAUsableHubThrowsARetryableFault(string assignment)
        {
            FakeDpsHandler dps = new(request => !request.Headers.Contains("Authorization")
                ? Respond(HttpStatusCode.Unauthorized, new JsonObject { ["authenticationKey"] = Convert.ToBase64String(PrimaryKey) }.ToJsonString())
                : Respond(HttpStatusCode.OK, UnusableAssignment(assignment)));

            using MockConnectionMqttClient mockMqttClient = CreateAcceptingMqttClient();
            using TestConnectionClient connectionClient = CreateClient(mockMqttClient, dps);

            DeviceException exception = await Assert.ThrowsAsync<DeviceException>(
                async () => await connectionClient
                    .ProvisionAndConnectAsync(CreateSettings(), new TpmAuthenticationProvider(RegistrationId, IdScope, new FakeTpm()), cancellationToken: TestContext.Current.CancellationToken)
                    .WaitAsync(s_testTimeout, TestContext.Current.CancellationToken));

            // The device registered once and refused the result rather than connecting with missing or wrong parameters.
            Assert.Equal(ErrorRetryability.Retryable, exception.Retryability);
            Assert.Equal(2, dps.Requests.Count);
            Assert.Equal(0, mockMqttClient.ConnectAttemptCount);
            Assert.Null(connectionClient.GetCurrentConnectionContext());
        }

        [Fact]
        public async Task TpmReprovisioningOverHttpRegistersAgainUntilItIsAssignedAUsableHub()
        {
            const string secondHub = "second-hub.azure-devices.net";
            byte[] hubKey = Encoding.UTF8.GetBytes("some-hub-key-for-unit-tests");

            int registrations = 0;
            FakeDpsHandler dps = new(request =>
            {
                if (!request.Headers.Contains("Authorization"))
                {
                    return Respond(HttpStatusCode.Unauthorized, new JsonObject { ["authenticationKey"] = Convert.ToBase64String(PrimaryKey) }.ToJsonString());
                }

                switch (Interlocked.Increment(ref registrations))
                {
                    case 1:
                        return Respond(HttpStatusCode.OK, AssignedTpm(hubKey));
                    case 2:
                        return Respond(HttpStatusCode.OK, UnusableAssignment("no hub"));
                    default:
                        RegistrationOperationStatus status = AssignedTpm(hubKey);
                        status.RegistrationState!.AssignedHub = secondHub;
                        return Respond(HttpStatusCode.OK, status);
                }
            });

            TaskCompletionSource connectedToSecondHub = new(TaskCreationOptions.RunContinuationsAsynchronously);
            int firstHubConnects = 0;
            using MockConnectionMqttClient mockMqttClient = new();
            mockMqttClient.OnConnect = connect =>
            {
                if (connect.HostName == secondHub)
                {
                    connectedToSecondHub.TrySetResult();
                    return Task.FromResult(new MqttConnectAck() { ResultCode = MqttConnectReasonCode.Success });
                }

                return Task.FromResult(new MqttConnectAck()
                {
                    ResultCode = Interlocked.Increment(ref firstHubConnects) == 1 ? MqttConnectReasonCode.Success : MqttConnectReasonCode.NotAuthorized,
                });
            };

            ImmediateRetryPolicy retryPolicy = new();
            using TestConnectionClient connectionClient = CreateClient(mockMqttClient, dps, retryPolicy);

            await connectionClient
                .ProvisionAndConnectAsync(CreateSettings(), new TpmAuthenticationProvider(RegistrationId, IdScope, new FakeTpm()), cancellationToken: TestContext.Current.CancellationToken)
                .WaitAsync(s_testTimeout, TestContext.Current.CancellationToken);

            await mockMqttClient.SimulateServerDisconnectAsync(MqttDisconnectReason.NotAuthorized);
            await connectedToSecondHub.Task.WaitAsync(s_testTimeout, TestContext.Current.CancellationToken);

            // The unusable assignment was retried under the policy as a provisioning failure, and never connected to.
            Assert.Equal(3, registrations);
            Assert.Equal(secondHub, connectionClient.GetCurrentConnectionContext()!.IotHubHostName);
            Assert.Contains(ConnectionEndpoint.DeviceProvisioningService, retryPolicy.Endpoints);
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

        private static RegistrationOperationStatus UnusableAssignment(string assignment)
        {
            if (assignment == "failed")
            {
                // DPS completes the registration without assigning a hub, as it does when the enrollment does not exist yet.
                return new RegistrationOperationStatus
                {
                    OperationId = OperationId,
                    Status = ProvisioningRegistrationStatus.Failed,
                    RegistrationState = new DeviceRegistrationResult()
                    {
                        RegistrationId = RegistrationId,
                        Status = ProvisioningRegistrationStatus.Failed,
                        ErrorCode = 404201,
                        ErrorMessage = "Enrollment not found.",
                    },
                };
            }

            // Otherwise DPS reports "assigned", but the assignment lacks something the device needs to connect.
            RegistrationOperationStatus status = AssignedTpm(Encoding.UTF8.GetBytes("some-hub-key-for-unit-tests"));
            switch (assignment)
            {
                case "no hub":
                    status.RegistrationState!.AssignedHub = null;
                    break;
                case "no device id":
                    status.RegistrationState!.DeviceId = null;
                    break;
                case "unsupported connection profile":
                    status.RegistrationState!.ConnectionProfile = IotHubConnectionType.Mqttv5;
                    break;
                default:
                    throw new ArgumentOutOfRangeException(nameof(assignment), assignment, null);
            }

            return status;
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

            /// <summary>Runs before each import completes, without observing cancellation, as a TPM command does.</summary>
            public Func<byte[], Task>? OnImport { get; set; }

            public Task<byte[]> GetEndorsementKeyAsync(CancellationToken cancellationToken = default) => Task.FromResult(EndorsementKey);

            public Task<byte[]> GetStorageRootKeyAsync(CancellationToken cancellationToken = default) => Task.FromResult(StorageRootKey);

            public async Task ImportKeyAsync(byte[] encryptedKey, CancellationToken cancellationToken = default)
            {
                if (OnImport != null)
                {
                    await OnImport(encryptedKey);
                }

                ImportedKeys.Add(encryptedKey);
                _key = encryptedKey;
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

        private static TestConnectionClient CreateClient(
            MockConnectionMqttClient mqttClient,
            FakeDpsHandler dps,
            IRetryPolicy? retryPolicy = null,
            TimeSpan? connectionAttemptTimeout = null)
        {
            Device.Unified.Models.ConnectionClientOptions options = new() { MqttClient = mqttClient };
            if (retryPolicy != null)
            {
                options.ConnectionRetryPolicy = retryPolicy;
            }

            if (connectionAttemptTimeout != null)
            {
                options.ConnectionAttemptTimeout = connectionAttemptTimeout.Value;
            }

            return new TestConnectionClient(options)
            {
                CreateHttpClient = _ => new HttpClient(dps),
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

        private static HttpResponseMessage RespondAssigning(TimeSpan retryAfter)
        {
            HttpResponseMessage response = Respond(HttpStatusCode.Accepted, Assigning());
            response.Headers.RetryAfter = new(retryAfter);
            return response;
        }

        private static HttpResponseMessage Respond(HttpStatusCode statusCode, string body)
        {
            return new HttpResponseMessage(statusCode)
            {
                Content = new StringContent(body, Encoding.UTF8, "application/json"),
            };
        }

        private sealed record RecordedRequest(HttpMethod Method, string Uri, string? Authorization, string? UserAgent, string? Body, long Timestamp);

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
                long timestamp = Stopwatch.GetTimestamp();
                _requests.Enqueue(new RecordedRequest(
                    request.Method,
                    request.RequestUri!.ToString(),
                    request.Headers.TryGetValues("Authorization", out IEnumerable<string>? authorization) ? authorization.Single() : null,
                    request.Headers.TryGetValues("User-Agent", out IEnumerable<string>? userAgent) ? string.Join(" ", userAgent) : null,
                    request.Content == null ? null : await request.Content.ReadAsStringAsync(cancellationToken),
                    timestamp));

                return await _respond(request, cancellationToken);
            }
        }

        private sealed class DisposalTrackingHttpClient : HttpClient
        {
            public DisposalTrackingHttpClient(HttpMessageHandler handler)
                : base(handler)
            {
            }

            public bool IsDisposed { get; private set; }

            protected override void Dispose(bool disposing)
            {
                IsDisposed = true;
                base.Dispose(disposing);
            }
        }

        private sealed class ImmediateRetryPolicy : IRetryPolicy
        {
            private readonly uint _maxHubConnectAttemptsBeforeReprovision;
            private readonly uint _maxProvisioningRetries;

            /// <param name="maxHubConnectAttemptsBeforeReprovision">
            /// How many failed attempts to reconnect to IoT hub to make before re-provisioning, or zero to never re-provision.
            /// </param>
            /// <param name="maxProvisioningRetries">
            /// How many times to retry Device Provisioning Service before abandoning, or zero to never abandon.
            /// </param>
            public ImmediateRetryPolicy(uint maxHubConnectAttemptsBeforeReprovision = 0, uint maxProvisioningRetries = 0)
            {
                _maxHubConnectAttemptsBeforeReprovision = maxHubConnectAttemptsBeforeReprovision;
                _maxProvisioningRetries = maxProvisioningRetries;
            }

            public ConcurrentQueue<ConnectionEndpoint> Endpoints { get; } = new();

            public RetryGuidance GetRetryGuidance(uint currentRetryCount, Exception? lastException, ConnectionEndpoint connectionEndpoint, out TimeSpan retryDelay)
            {
                Endpoints.Enqueue(connectionEndpoint);
                retryDelay = TimeSpan.Zero;

                if (connectionEndpoint == ConnectionEndpoint.DeviceProvisioningService
                    && _maxProvisioningRetries > 0
                    && currentRetryCount > _maxProvisioningRetries)
                {
                    return RetryGuidance.AbandonRetry;
                }

                return connectionEndpoint == ConnectionEndpoint.IotHub
                    && _maxHubConnectAttemptsBeforeReprovision > 0
                    && currentRetryCount > _maxHubConnectAttemptsBeforeReprovision
                        ? RetryGuidance.Reprovision
                        : RetryGuidance.Retry;
            }
        }
    }
}
