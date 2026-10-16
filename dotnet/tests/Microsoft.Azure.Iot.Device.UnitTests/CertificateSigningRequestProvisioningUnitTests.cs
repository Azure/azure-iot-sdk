// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Authentication;
using Microsoft.Azure.Iot.Device.Exceptions;
using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Models.CertificateManagement;
using Microsoft.Azure.Iot.Device.Mqtt;
using Microsoft.Azure.Iot.Device.Provisioning;
using Microsoft.Azure.Iot.Device.Provisioning.Models;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using System.Text.Json;
using Xunit;
using UnifiedConnectionClient = Microsoft.Azure.Iot.Device.Unified.Connection.ConnectionClient;

namespace Microsoft.Azure.Iot.Device.UnitTests
{
    /// <summary>
    /// Tests that a certificate signing request supplied in <see cref="ProvisioningSettings"/> reaches Device
    /// Provisioning Service, and that the certificate chain it issues reaches the caller. Dropping the request is
    /// invisible from the outside -- registration still succeeds, it just returns no issued certificate -- so it is
    /// covered here rather than only by an e2e test.
    /// </summary>
    public class CertificateSigningRequestProvisioningUnitTests
    {
        private const string GlobalDeviceEndpoint = "global.azure-devices-provisioning.net";
        private const string IdScope = "0ne00000000";
        private const string RegistrationId = "someRegistrationId";
        private const string DeviceId = "someDeviceId";
        private const string AssignedHub = "some-hub.azure-devices.net";
        private static readonly string SymmetricKey = Convert.ToBase64String(Encoding.UTF8.GetBytes("some-symmetric-key-for-unit-tests"));

        private static readonly TimeSpan s_testTimeout = TimeSpan.FromSeconds(30);

        [Fact]
        public async Task ProvisioningSendsTheCertificateSigningRequestAndReturnsTheIssuedChain()
        {
            using RSA operationalKey = RSA.Create(2048);
            string csrBase64 = CreateCertificateSigningRequest(operationalKey);
            using X509Certificate2 issuedCertificate = CreateIssuedCertificate(operationalKey);
            string[] issuedChain = new[] { Convert.ToBase64String(issuedCertificate.RawData), "second-issued-certificate" };

            using MockConnectionMqttClient mockMqttClient = new();
            MockCertificateIssuingProvisioningService mockDps = new(mockMqttClient, issuedChain);

            MqttConnect? dpsConnect = null;
            MqttConnect? hubConnect = null;
            mockMqttClient.OnConnect = connect =>
            {
                if (dpsConnect == null)
                {
                    dpsConnect = connect;
                }
                else
                {
                    hubConnect ??= connect;
                }

                return Task.FromResult(new MqttConnectAck() { ResultCode = MqttConnectReasonCode.Success });
            };

            using TestConnectionClient connectionClient = new(new() { MqttClient = mockMqttClient });

            X509AuthenticationProvider authentication = CreateAuthenticationProvider(operationalKey);

            ProvisioningSettings provisioningSettings = new(IdScope)
            {
                RegistrationId = RegistrationId,
                GlobalEndpointAddress = GlobalDeviceEndpoint,
                CertificateSigningRequest = new(operationalKey, csrBase64),
            };

            ConnectionContext connectionContext = await connectionClient
                .ProvisionAndConnectAsync(provisioningSettings, authentication, cancellationToken: TestContext.Current.CancellationToken)
                .WaitAsync(s_testTimeout, TestContext.Current.CancellationToken);

            RegistrationRequestPayload? sentPayload = mockDps.RegistrationRequestPayload;
            Assert.NotNull(sentPayload);
            Assert.Equal(csrBase64, sentPayload.ClientCertificateSigningRequest);

            Assert.NotNull(dpsConnect);
            Assert.Contains("api-version=2026-11-02-preview", dpsConnect.Username);
            Assert.Equal(authentication.ClientCertificate.Thumbprint, dpsConnect.ClientCertificate?.Thumbprint);

            // The hub connection presents the DPS-issued certificate rather than the boot certificate.
            Assert.NotNull(hubConnect);
            Assert.NotNull(hubConnect.ClientCertificate);
            Assert.Equal(issuedCertificate.Thumbprint, hubConnect.ClientCertificate.Thumbprint);
            Assert.True(hubConnect.ClientCertificate.HasPrivateKey);

            Assert.NotNull(connectionContext.IssuedClientCertificates);
            Assert.Equal(issuedChain, connectionContext.IssuedClientCertificates);
        }

        [Fact]
        public async Task ProvisioningReportsWhatTheServiceSaidWhenItRefusesTheRegistration()
        {
            using RSA operationalKey = RSA.Create(2048);
            string csrBase64 = CreateCertificateSigningRequest(operationalKey);

            using MockConnectionMqttClient mockMqttClient = new();
            const string errorTopic = "$dps/registrations/res/401/?$rid=1";
            const string errorBody = "{\"errorCode\":401002,\"trackingId\":\"someTrackingId\",\"message\":\"Unauthorized\"}";
            MockRefusingProvisioningService mockDps = new(mockMqttClient, errorTopic, errorBody);
            Assert.NotNull(mockDps);

            mockMqttClient.OnConnect = connect =>
                Task.FromResult(new MqttConnectAck() { ResultCode = MqttConnectReasonCode.Success });

            using TestConnectionClient connectionClient = new(new() { MqttClient = mockMqttClient });

            ProvisioningSettings provisioningSettings = new(IdScope)
            {
                RegistrationId = RegistrationId,
                GlobalEndpointAddress = GlobalDeviceEndpoint,
                CertificateSigningRequest = new(operationalKey, csrBase64),
            };

            Exception exception = await Assert.ThrowsAnyAsync<Exception>(
                async () => await connectionClient
                    .ProvisionAndConnectAsync(provisioningSettings, CreateAuthenticationProvider(), cancellationToken: TestContext.Current.CancellationToken)
                    .WaitAsync(s_testTimeout, TestContext.Current.CancellationToken));

            // The service's own answer, rather than a placeholder, is what says why the registration did not start.
            Assert.Contains(errorTopic, exception.Message);
            Assert.Contains(errorBody, exception.Message);
        }

        [Fact]
        public async Task ProvisioningWithoutACertificateSigningRequestSendsNone()
        {
            using MockConnectionMqttClient mockMqttClient = new();
            MockCertificateIssuingProvisioningService mockDps = new(mockMqttClient, issuedCertificateChain: null);

            MqttConnect? dpsConnect = null;
            mockMqttClient.OnConnect = connect =>
            {
                dpsConnect ??= connect;

                return Task.FromResult(new MqttConnectAck() { ResultCode = MqttConnectReasonCode.Success });
            };

            using TestConnectionClient connectionClient = new(new() { MqttClient = mockMqttClient });

            ProvisioningSettings provisioningSettings = new(IdScope)
            {
                RegistrationId = RegistrationId,
                GlobalEndpointAddress = GlobalDeviceEndpoint,
            };

            ConnectionContext connectionContext = await connectionClient
                .ProvisionAndConnectAsync(provisioningSettings, CreateAuthenticationProvider(), cancellationToken: TestContext.Current.CancellationToken)
                .WaitAsync(s_testTimeout, TestContext.Current.CancellationToken);

            RegistrationRequestPayload? sentPayload = mockDps.RegistrationRequestPayload;
            Assert.NotNull(sentPayload);
            Assert.Null(sentPayload.ClientCertificateSigningRequest);
            Assert.Null(connectionContext.IssuedClientCertificates);

            // A registration without a certificate request uses the same API version.
            Assert.NotNull(dpsConnect);
            Assert.Contains("api-version=2026-11-02-preview", dpsConnect.Username);
        }

        [Fact]
        public async Task IotHubAuthenticationPresentsTheIssuedCertificateWhenItsPrivateKeyIsSet()
        {
            using RSA operationalKey = RSA.Create(2048);
            using X509Certificate2 issuedCertificate = CreateIssuedCertificate(operationalKey);
            string[] issuedChain = new[] { Convert.ToBase64String(issuedCertificate.RawData) };

            X509AuthenticationProvider authentication = CreateAuthenticationProvider(operationalKey);
            Assert.NotNull(authentication.HandleCertificateSigningCompleteAsync);

            AuthenticationCredentials hubCredentials = await authentication.GetOperationalAuthenticationCredentialsAsync(AssignedHub, DeviceId, issuedChain);
            Assert.NotNull(hubCredentials.ClientCertificate);
            Assert.Equal(issuedCertificate.Thumbprint, hubCredentials.ClientCertificate.Thumbprint);
            Assert.True(hubCredentials.ClientCertificate.HasPrivateKey);

            // Handling the same chain again reuses the same certificate rather than rebuilding it on every connect.
            Assert.Same(hubCredentials.ClientCertificate, (await authentication.GetOperationalAuthenticationCredentialsAsync(AssignedHub, DeviceId, issuedChain)).ClientCertificate);

            // DPS is always authenticated with the original client certificate.
            Assert.Same(authentication.ClientCertificate, authentication.GetBootAuthenticationCredentials().ClientCertificate);
        }

        [Fact]
        public async Task IotHubAuthenticationPresentsTheOriginalCertificateWithoutAnIssuedCertificateOrPrivateKey()
        {
            using RSA operationalKey = RSA.Create(2048);
            using X509Certificate2 issuedCertificate = CreateIssuedCertificate(operationalKey);
            string[] issuedChain = new[] { Convert.ToBase64String(issuedCertificate.RawData) };

            // Without a private key, the default callback presents the original certificate.
            X509AuthenticationProvider authentication = CreateAuthenticationProvider();
            Assert.NotNull(authentication.HandleCertificateSigningCompleteAsync);
            Assert.Same(authentication.ClientCertificate, (await authentication.GetOperationalAuthenticationCredentialsAsync(AssignedHub, DeviceId, issuedChain)).ClientCertificate);

            authentication = CreateAuthenticationProvider(operationalKey);
            Assert.Same(authentication.ClientCertificate, (await authentication.GetOperationalAuthenticationCredentialsAsync(AssignedHub, DeviceId, null)).ClientCertificate);
        }

        [Fact]
        public async Task SymmetricKeyProvisioningWithACertificateSigningRequestConnectsToTheHubWithTheIssuedCertificate()
        {
            using RSA operationalKey = RSA.Create(2048);
            string csrBase64 = CreateCertificateSigningRequest(operationalKey);
            using X509Certificate2 issuedCertificate = CreateIssuedCertificate(operationalKey);
            string[] issuedChain = new[] { Convert.ToBase64String(issuedCertificate.RawData) };

            using MockConnectionMqttClient mockMqttClient = new();
            MockCertificateIssuingProvisioningService mockDps = new(mockMqttClient, issuedChain);

            MqttConnect? dpsConnect = null;
            MqttConnect? hubConnect = null;
            mockMqttClient.OnConnect = connect =>
            {
                if (dpsConnect == null)
                {
                    dpsConnect = connect;
                }
                else
                {
                    hubConnect ??= connect;
                }

                return Task.FromResult(new MqttConnectAck() { ResultCode = MqttConnectReasonCode.Success });
            };

            using TestConnectionClient connectionClient = new(new() { MqttClient = mockMqttClient });

            SymmetricKeyWithIssuedCertificateAuthenticationProvider authentication = new(RegistrationId, IdScope, SymmetricKey, operationalKey);

            ProvisioningSettings provisioningSettings = new(IdScope)
            {
                RegistrationId = RegistrationId,
                GlobalEndpointAddress = GlobalDeviceEndpoint,
                CertificateSigningRequest = new(operationalKey, csrBase64),
            };

            await connectionClient
                .ProvisionAndConnectAsync(provisioningSettings, authentication, cancellationToken: TestContext.Current.CancellationToken)
                .WaitAsync(s_testTimeout, TestContext.Current.CancellationToken);

            Assert.Equal(csrBase64, mockDps.RegistrationRequestPayload?.ClientCertificateSigningRequest);

            // DPS is authenticated with a SAS token derived from the symmetric key.
            Assert.NotNull(dpsConnect);
            Assert.Null(dpsConnect.ClientCertificate);
            Assert.StartsWith("SharedAccessSignature ", Encoding.UTF8.GetString(dpsConnect.Password));

            // IoT hub is authenticated with the DPS-issued certificate instead.
            Assert.NotNull(hubConnect);
            Assert.NotNull(hubConnect.ClientCertificate);
            Assert.Equal(issuedCertificate.Thumbprint, hubConnect.ClientCertificate.Thumbprint);
            Assert.True(hubConnect.ClientCertificate.HasPrivateKey);
            Assert.Empty(hubConnect.Password);
        }

        [Fact]
        public async Task SymmetricKeyWithIssuedCertificateAuthenticatesTheHubWithASasTokenUntilACertificateIsIssued()
        {
            using RSA operationalKey = RSA.Create(2048);
            SymmetricKeyWithIssuedCertificateAuthenticationProvider authentication = new(RegistrationId, IdScope, SymmetricKey, operationalKey);

            AuthenticationCredentials hubCredentials = await authentication.GetOperationalAuthenticationCredentialsAsync(AssignedHub, DeviceId, null);

            Assert.Null(hubCredentials.ClientCertificate);
            Assert.StartsWith("SharedAccessSignature ", Encoding.UTF8.GetString(hubCredentials.Password));
        }

        [Fact]
        public async Task X509ProvisioningWithACallbackConnectsToTheHubWithTheCertificateTheCallbackReturns()
        {
            using RSA operationalKey = RSA.Create(2048);
            string csrBase64 = CreateCertificateSigningRequest(operationalKey);
            using X509Certificate2 issuedCertificate = CreateIssuedCertificate(operationalKey);
            using X509Certificate2 issuedCertificateWithKey = issuedCertificate.CopyWithPrivateKey(operationalKey);
            string[] issuedChain = new[] { Convert.ToBase64String(issuedCertificate.RawData) };

            using MockConnectionMqttClient mockMqttClient = new();
            MockCertificateIssuingProvisioningService mockDps = new(mockMqttClient, issuedChain);
            Assert.NotNull(mockDps);

            MqttConnect? hubConnect = null;
            int connectCount = 0;
            mockMqttClient.OnConnect = connect =>
            {
                if (++connectCount == 2)
                {
                    hubConnect = connect;
                }

                return Task.FromResult(new MqttConnectAck() { ResultCode = MqttConnectReasonCode.Success });
            };

            using TestConnectionClient connectionClient = new(new() { MqttClient = mockMqttClient });

            List<IReadOnlyList<string>> callbackInvocations = new();
            X509AuthenticationProvider authentication = new(
                CreateAuthenticationProvider().ClientCertificate,
                issuedCertificates =>
                {
                    callbackInvocations.Add(issuedCertificates);
                    return Task.FromResult(issuedCertificateWithKey);
                });

            ProvisioningSettings provisioningSettings = new(IdScope)
            {
                RegistrationId = RegistrationId,
                GlobalEndpointAddress = GlobalDeviceEndpoint,
                CertificateSigningRequest = new(operationalKey, csrBase64),
            };

            await connectionClient
                .ProvisionAndConnectAsync(provisioningSettings, authentication, cancellationToken: TestContext.Current.CancellationToken)
                .WaitAsync(s_testTimeout, TestContext.Current.CancellationToken);

            // The callback runs when preparing credentials for the hub CONNECT.
            IReadOnlyList<string> handledChain = Assert.Single(callbackInvocations);
            Assert.Equal(issuedChain, handledChain);

            Assert.NotNull(hubConnect);
            Assert.Same(issuedCertificateWithKey, hubConnect.ClientCertificate);
        }

        [Fact]
        public async Task SymmetricKeyProvisioningWithACallbackConnectsToTheHubWithTheCertificateTheCallbackReturns()
        {
            using RSA operationalKey = RSA.Create(2048);
            string csrBase64 = CreateCertificateSigningRequest(operationalKey);
            using X509Certificate2 issuedCertificate = CreateIssuedCertificate(operationalKey);
            using X509Certificate2 issuedCertificateWithKey = issuedCertificate.CopyWithPrivateKey(operationalKey);
            string[] issuedChain = new[] { Convert.ToBase64String(issuedCertificate.RawData) };

            using MockConnectionMqttClient mockMqttClient = new();
            MockCertificateIssuingProvisioningService mockDps = new(mockMqttClient, issuedChain);
            Assert.NotNull(mockDps);

            MqttConnect? hubConnect = null;
            int connectCount = 0;
            mockMqttClient.OnConnect = connect =>
            {
                if (++connectCount == 2)
                {
                    hubConnect = connect;
                }

                return Task.FromResult(new MqttConnectAck() { ResultCode = MqttConnectReasonCode.Success });
            };

            using TestConnectionClient connectionClient = new(new() { MqttClient = mockMqttClient });

            int callbackInvocations = 0;
            SymmetricKeyWithIssuedCertificateAuthenticationProvider authentication = new(
                RegistrationId,
                IdScope,
                SymmetricKey,
                issuedCertificates =>
                {
                    callbackInvocations++;
                    return Task.FromResult(issuedCertificateWithKey);
                });

            ProvisioningSettings provisioningSettings = new(IdScope)
            {
                RegistrationId = RegistrationId,
                GlobalEndpointAddress = GlobalDeviceEndpoint,
                CertificateSigningRequest = new(operationalKey, csrBase64),
            };

            await connectionClient
                .ProvisionAndConnectAsync(provisioningSettings, authentication, cancellationToken: TestContext.Current.CancellationToken)
                .WaitAsync(s_testTimeout, TestContext.Current.CancellationToken);

            Assert.Equal(1, callbackInvocations);
            Assert.NotNull(hubConnect);
            Assert.Same(issuedCertificateWithKey, hubConnect.ClientCertificate);
            Assert.Empty(hubConnect.Password);
        }

        [Fact]
        public async Task ProvisioningFailsWhenTheCertificateSigningCompleteCallbackThrows()
        {
            using RSA operationalKey = RSA.Create(2048);
            string csrBase64 = CreateCertificateSigningRequest(operationalKey);
            using X509Certificate2 issuedCertificate = CreateIssuedCertificate(operationalKey);
            string[] issuedChain = new[] { Convert.ToBase64String(issuedCertificate.RawData) };

            using MockConnectionMqttClient mockMqttClient = new();
            MockCertificateIssuingProvisioningService mockDps = new(mockMqttClient, issuedChain);
            Assert.NotNull(mockDps);

            mockMqttClient.OnConnect = connect =>
                Task.FromResult(new MqttConnectAck() { ResultCode = MqttConnectReasonCode.Success });

            using TestConnectionClient connectionClient = new(new() { MqttClient = mockMqttClient });

            X509AuthenticationProvider authentication = new(
                CreateAuthenticationProvider().ClientCertificate,
                _ => throw new NotSupportedException("callback failed"));

            ProvisioningSettings provisioningSettings = new(IdScope)
            {
                RegistrationId = RegistrationId,
                GlobalEndpointAddress = GlobalDeviceEndpoint,
                CertificateSigningRequest = new(operationalKey, csrBase64),
            };

            DeviceException exception = await Assert.ThrowsAsync<DeviceException>(
                async () => await connectionClient
                    .ProvisionAndConnectAsync(provisioningSettings, authentication, cancellationToken: TestContext.Current.CancellationToken)
                    .WaitAsync(s_testTimeout, TestContext.Current.CancellationToken));
            NotSupportedException callbackException = Assert.IsType<NotSupportedException>(exception.InnerException);
            Assert.Equal("callback failed", callbackException.Message);
            Assert.Equal(1, mockMqttClient.ConnectAttemptCount);
        }

        [Theory]
        [InlineData(false)]
        [InlineData(true)]
        public async Task OperationalCredentialsAwaitTheCallbackAndReuseItsCertificate(bool useSymmetricKey)
        {
            using RSA operationalKey = RSA.Create(2048);
            using X509Certificate2 issuedCertificate = CreateIssuedCertificate(operationalKey);
            using X509Certificate2 issuedCertificateWithKey = issuedCertificate.CopyWithPrivateKey(operationalKey);
            string[] issuedChain = new[] { Convert.ToBase64String(issuedCertificate.RawData) };
            TaskCompletionSource<X509Certificate2> callbackResult = new(TaskCreationOptions.RunContinuationsAsynchronously);
            int callbackInvocations = 0;
            Func<IReadOnlyList<string>, Task<X509Certificate2>> callback = certificates =>
            {
                Assert.Same(issuedChain, certificates);
                callbackInvocations++;
                return callbackResult.Task;
            };
            AuthenticationProvider authentication = useSymmetricKey
                ? new SymmetricKeyWithIssuedCertificateAuthenticationProvider(RegistrationId, IdScope, SymmetricKey, callback)
                : new X509AuthenticationProvider(CreateAuthenticationProvider().ClientCertificate, callback);
            AuthenticationCredentials bootCredentials = authentication.GetBootAuthenticationCredentials();

            Task<AuthenticationCredentials> firstRequest = authentication.GetOperationalAuthenticationCredentialsAsync(AssignedHub, DeviceId, issuedChain);
            Task<AuthenticationCredentials> concurrentRequest = authentication.GetOperationalAuthenticationCredentialsAsync(AssignedHub, DeviceId, issuedChain);
            Assert.False(firstRequest.IsCompleted);
            Assert.False(concurrentRequest.IsCompleted);
            Assert.Equal(1, callbackInvocations);

            callbackResult.SetResult(issuedCertificateWithKey);
            AuthenticationCredentials[] credentials = await Task.WhenAll(firstRequest, concurrentRequest)
                .WaitAsync(s_testTimeout, TestContext.Current.CancellationToken);
            Assert.All(credentials, result => Assert.Same(issuedCertificateWithKey, result.ClientCertificate));
            Assert.Same(issuedCertificateWithKey, (await authentication.GetOperationalAuthenticationCredentialsAsync(AssignedHub, DeviceId, issuedChain)).ClientCertificate);
            Assert.Equal(1, callbackInvocations);
            Assert.Same(bootCredentials.ClientCertificate, authentication.GetBootAuthenticationCredentials().ClientCertificate);
        }

        [Theory]
        [InlineData(false)]
        [InlineData(true)]
        public async Task OperationalCredentialsPropagateCallbackFailuresAndRetryTheLatestChain(bool useSymmetricKey)
        {
            using RSA operationalKey = RSA.Create(2048);
            using X509Certificate2 issuedCertificate = CreateIssuedCertificate(operationalKey);
            using X509Certificate2 replacementCertificate = CreateIssuedCertificate(operationalKey);
            using X509Certificate2 issuedCertificateWithKey = issuedCertificate.CopyWithPrivateKey(operationalKey);
            using X509Certificate2 replacementCertificateWithKey = replacementCertificate.CopyWithPrivateKey(operationalKey);
            string[] issuedChain = new[] { Convert.ToBase64String(issuedCertificate.RawData) };
            string[] replacementChain = new[] { Convert.ToBase64String(replacementCertificate.RawData) };
            InvalidOperationException failure = new("callback failed");
            int replacementAttempts = 0;
            Func<IReadOnlyList<string>, Task<X509Certificate2>> callback = certificates =>
            {
                if (ReferenceEquals(certificates, issuedChain))
                {
                    return Task.FromResult(issuedCertificateWithKey);
                }

                Assert.Same(replacementChain, certificates);
                return ++replacementAttempts == 1
                    ? Task.FromException<X509Certificate2>(failure)
                    : Task.FromResult(replacementCertificateWithKey);
            };
            AuthenticationProvider authentication = useSymmetricKey
                ? new SymmetricKeyWithIssuedCertificateAuthenticationProvider(RegistrationId, IdScope, SymmetricKey, callback)
                : new X509AuthenticationProvider(CreateAuthenticationProvider().ClientCertificate, callback);

            Assert.Same(issuedCertificateWithKey, (await authentication.GetOperationalAuthenticationCredentialsAsync(AssignedHub, DeviceId, issuedChain)).ClientCertificate);
            Assert.Same(failure, await Assert.ThrowsAsync<InvalidOperationException>(
                () => authentication.GetOperationalAuthenticationCredentialsAsync(AssignedHub, DeviceId, replacementChain)));
            Assert.Same(replacementCertificateWithKey, (await authentication.GetOperationalAuthenticationCredentialsAsync(AssignedHub, DeviceId, replacementChain)).ClientCertificate);
            Assert.Equal(2, replacementAttempts);
        }

        [Fact]
        public async Task HubIssuedCertificatesArePreparedOnlyOnTheNextConnectionAttempt()
        {
            using RSA operationalKey = RSA.Create(2048);
            using X509Certificate2 issuedCertificate = CreateIssuedCertificate(operationalKey);
            using X509Certificate2 issuedCertificateWithKey = issuedCertificate.CopyWithPrivateKey(operationalKey);
            string[] issuedChain = new[] { Convert.ToBase64String(issuedCertificate.RawData) };
            int callbackInvocations = 0;
            X509AuthenticationProvider authentication = new(
                CreateAuthenticationProvider().ClientCertificate,
                async certificates =>
                {
                    await Task.Yield();
                    Assert.Equal(issuedChain, certificates);
                    callbackInvocations++;
                    return issuedCertificateWithKey;
                });
            ConnectionContext context = new()
            {
                DeviceId = DeviceId,
                IotHubHostName = AssignedHub,
                ConnectionProfile = IotHubConnectionType.Mqttv3,
                AuthenticationProvider = authentication,
            };
            using MockConnectionMqttClient mockMqttClient = new();
            List<X509Certificate2?> presentedCertificates = new();
            mockMqttClient.OnConnect = connect =>
            {
                presentedCertificates.Add(connect.ClientCertificate);
                return Task.FromResult(new MqttConnectAck() { ResultCode = MqttConnectReasonCode.Success });
            };
            using UnifiedConnectionClient connectionClient = new(new() { MqttClient = mockMqttClient });
            using SemaphoreSlim connectionReady = new(0);
            connectionClient.DevicePresenceFlowCompletedAsync += _ =>
            {
                connectionReady.Release();
                return Task.CompletedTask;
            };
            await connectionClient.ConnectToHubAsync(context, TestContext.Current.CancellationToken)
                .WaitAsync(s_testTimeout, TestContext.Current.CancellationToken);
            await connectionReady.WaitAsync(TestContext.Current.CancellationToken);

            IotHubCertificateSigningRequest request = new(DeviceId, CreateCertificateSigningRequest(operationalKey));
            CertificateSigningOperation operation = await connectionClient.SendCertificateSigningRequestAsync(request, TestContext.Current.CancellationToken);
            await mockMqttClient.SimulatePublishReceivedAsync(new()
            {
                Topic = "$iothub/credentials/res/200/?$rid=" + request.RequestId,
                Payload = JsonSerializer.SerializeToUtf8Bytes(new CertificateSigningResponse()
                {
                    Certificates = issuedChain,
                    CorrelationId = request.RequestId,
                }),
            });

            CertificateSigningResponse response = await operation.Completed.WaitAsync(s_testTimeout, TestContext.Current.CancellationToken);
            Assert.Equal(issuedChain, response.Certificates);
            Assert.Equal(issuedChain, context.IssuedClientCertificates);
            Assert.Equal(0, callbackInvocations);

            await mockMqttClient.SimulateServerDisconnectAsync(MqttDisconnectReason.NormalDisconnection)
                .WaitAsync(s_testTimeout, TestContext.Current.CancellationToken);
            await connectionReady.WaitAsync(TestContext.Current.CancellationToken)
                .WaitAsync(s_testTimeout, TestContext.Current.CancellationToken);
            await mockMqttClient.SimulateServerDisconnectAsync(MqttDisconnectReason.NormalDisconnection)
                .WaitAsync(s_testTimeout, TestContext.Current.CancellationToken);
            await connectionReady.WaitAsync(TestContext.Current.CancellationToken)
                .WaitAsync(s_testTimeout, TestContext.Current.CancellationToken);

            Assert.Equal(1, callbackInvocations);
            Assert.Equal(
                new[] { authentication.ClientCertificate, issuedCertificateWithKey, issuedCertificateWithKey },
                presentedCertificates);
        }

        private static X509Certificate2 CreateIssuedCertificate(RSA key)
        {
            var certificateRequest = new CertificateRequest($"CN={RegistrationId}", key, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);
            using X509Certificate2 withKey = certificateRequest.CreateSelfSigned(DateTimeOffset.UtcNow.AddDays(-1), DateTimeOffset.UtcNow.AddDays(1));

            // Issued certificates arrive without a private key.
            return X509CertificateLoader.LoadCertificate(withKey.RawData);
        }

        private static string CreateCertificateSigningRequest(RSA key)
        {
            var certificateRequest = new CertificateRequest($"CN={RegistrationId}", key, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);

            return Convert.ToBase64String(certificateRequest.CreateSigningRequest());
        }

        private static X509AuthenticationProvider CreateAuthenticationProvider(AsymmetricAlgorithm? issuedCertificatePrivateKey = null)
        {
            using RSA key = RSA.Create(2048);
            var certificateRequest = new CertificateRequest($"CN={RegistrationId}", key, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);
            X509Certificate2 certificate = certificateRequest.CreateSelfSigned(DateTimeOffset.UtcNow.AddDays(-1), DateTimeOffset.UtcNow.AddDays(1));

            return issuedCertificatePrivateKey == null
                ? new X509AuthenticationProvider(certificate)
                : new X509AuthenticationProvider(certificate, issuedCertificatePrivateKey);
        }

        /// <summary>
        /// The Device Provisioning Service half of the registration flow: it records the registration request it was
        /// sent and assigns the device, optionally reporting a certificate chain it issued for the request.
        /// </summary>
        private sealed class MockCertificateIssuingProvisioningService
        {
            private const string RegisterTopicPrefix = "$dps/registrations/PUT/";
            private const string GetOperationStatusTopicPrefix = "$dps/registrations/GET/";
            private const string OperationId = "someOperationId";

            private readonly MockConnectionMqttClient _mqttClient;
            private readonly IReadOnlyList<string>? _issuedCertificateChain;

            public MockCertificateIssuingProvisioningService(MockConnectionMqttClient mqttClient, IReadOnlyList<string>? issuedCertificateChain)
            {
                _mqttClient = mqttClient;
                _issuedCertificateChain = issuedCertificateChain;
                _mqttClient.OnPublish = HandlePublishAsync;
            }

            /// <summary>The registration request payload this service was sent, as it arrived on the wire.</summary>
            public RegistrationRequestPayload? RegistrationRequestPayload { get; private set; }

            private async Task<MqttPublishAck> HandlePublishAsync(MqttPublish publish)
            {
                if (publish.Topic.StartsWith(RegisterTopicPrefix))
                {
                    RegistrationRequestPayload = JsonSerializer.Deserialize<RegistrationRequestPayload>(
                        publish.Payload, JsonSerializationSettings.Options);

                    await RespondAsync(new RegistrationOperationStatus()
                    {
                        OperationId = OperationId,
                        Status = ProvisioningRegistrationStatus.Assigning,
                    });
                }
                else if (publish.Topic.StartsWith(GetOperationStatusTopicPrefix))
                {
                    await RespondAsync(new RegistrationOperationStatus()
                    {
                        OperationId = OperationId,
                        Status = ProvisioningRegistrationStatus.Assigned,
                        RegistrationState = new DeviceRegistrationResult()
                        {
                            RegistrationId = RegistrationId,
                            DeviceId = DeviceId,
                            AssignedHub = AssignedHub,
                            Status = ProvisioningRegistrationStatus.Assigned,
                            IssuedClientCertificateChain = _issuedCertificateChain,
                        },
                    });
                }

                return new MqttPublishAck() { ReasonCode = MqttPublishAckReasonCode.Success };
            }

            private Task RespondAsync(RegistrationOperationStatus status)
            {
                return _mqttClient.SimulatePublishReceivedAsync(new MqttPublish()
                {
                    Topic = "$dps/registrations/res/200/?$rid=1",
                    Payload = JsonSerializer.SerializeToUtf8Bytes(status, JsonSerializationSettings.Options),
                });
            }
        }

        /// <summary>
        /// A Device Provisioning Service that refuses the registration request, answering it the way the service
        /// answers an error: a status code in the response topic and a reason in the body, neither of which is a
        /// registration operation.
        /// </summary>
        private sealed class MockRefusingProvisioningService
        {
            private const string RegisterTopicPrefix = "$dps/registrations/PUT/";

            private readonly MockConnectionMqttClient _mqttClient;
            private readonly string _errorTopic;
            private readonly string _errorBody;

            public MockRefusingProvisioningService(MockConnectionMqttClient mqttClient, string errorTopic, string errorBody)
            {
                _mqttClient = mqttClient;
                _errorTopic = errorTopic;
                _errorBody = errorBody;
                _mqttClient.OnPublish = HandlePublishAsync;
            }

            private async Task<MqttPublishAck> HandlePublishAsync(MqttPublish publish)
            {
                if (publish.Topic.StartsWith(RegisterTopicPrefix))
                {
                    await _mqttClient.SimulatePublishReceivedAsync(new MqttPublish()
                    {
                        Topic = _errorTopic,
                        Payload = Encoding.UTF8.GetBytes(_errorBody),
                    });
                }

                return new MqttPublishAck() { ReasonCode = MqttPublishAckReasonCode.Success };
            }
        }
    }
}
