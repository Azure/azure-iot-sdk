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
using System.Security.Authentication;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using System.Text.Json;
using Xunit;
using ConnectionClient = Microsoft.Azure.Iot.Device.Unified.Connection.ConnectionClient;

namespace Microsoft.Azure.Iot.Device.UnitTests
{
    /// <summary>
    /// Tests that a certificate signing request supplied to
    /// <see cref="AbstractConnectionClient.ProvisionAndConnectAsync(ProvisioningSettings, AuthenticationProvider, ProvisioningCertificateSigningRequest, bool, CancellationToken)"/>
    /// reaches Device Provisioning Service, and that the certificate chain it issues reaches the caller. Dropping the
    /// request is invisible from the outside -- registration still succeeds, it just returns no issued certificate -- so
    /// it is covered here rather than only by an e2e test.
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
        public void ConnectionApiTakesASingleAuthenticationProvider()
        {
            var methods = typeof(AbstractConnectionClient).GetMethods()
                .Where(m => m.Name == nameof(AbstractConnectionClient.ProvisionAndConnectAsync))
                .ToList();
            Assert.Equal(2, methods.Count);
            Assert.All(methods, method => Assert.Equal(typeof(AuthenticationProvider), method.GetParameters()[1].ParameterType));
            var property = typeof(ConnectionContext).GetProperty(nameof(ConnectionContext.AuthenticationProvider));
            Assert.NotNull(property);
            Assert.Equal(typeof(AuthenticationProvider), property.PropertyType);
            Assert.True(typeof(AuthenticationProvider).IsAbstract);
        }

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

            X509AuthenticationProvider authentication = CreateX509AuthenticationProvider(operationalKey);

            ProvisioningSettings provisioningSettings = new(IdScope)
            {
                RegistrationId = RegistrationId,
                GlobalEndpointAddress = GlobalDeviceEndpoint,
            };

            ConnectionContext connectionContext = await connectionClient
                .ProvisionAndConnectAsync(provisioningSettings, authentication, new ProvisioningCertificateSigningRequest(csrBase64), cancellationToken: TestContext.Current.CancellationToken)
                .WaitAsync(s_testTimeout, TestContext.Current.CancellationToken);

            RegistrationRequestPayload? sentPayload = mockDps.RegistrationRequestPayload;
            Assert.NotNull(sentPayload);
            Assert.Equal(csrBase64, sentPayload.ClientCertificateSigningRequest);

            Assert.NotNull(dpsConnect);
            Assert.Contains("api-version=2026-11-02-preview", dpsConnect.Username);
            Assert.Equal(authentication.ClientCertificate.Thumbprint, dpsConnect.ClientCertificate?.Thumbprint);

            // The hub connection presents the DPS-issued certificate rather than the provider's own certificate.
            Assert.NotNull(hubConnect);
            Assert.NotNull(hubConnect.ClientCertificate);
            Assert.Equal(issuedCertificate.Thumbprint, hubConnect.ClientCertificate.Thumbprint);
            Assert.True(hubConnect.ClientCertificate.HasPrivateKey);

            Assert.NotNull(connectionContext.IssuedClientCertificates);
            Assert.Equal(issuedChain, connectionContext.IssuedClientCertificates);
            Assert.Same(authentication, connectionContext.AuthenticationProvider);
            Assert.Same(authentication.ClientCertificate, (await authentication.GetProvisioningCredentialsAsync(TestContext.Current.CancellationToken)).ClientCertificate);
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
            };

            Exception exception = await Assert.ThrowsAnyAsync<Exception>(
                async () => await connectionClient
                    .ProvisionAndConnectAsync(provisioningSettings, CreateX509AuthenticationProvider(operationalKey), new ProvisioningCertificateSigningRequest(csrBase64), cancellationToken: TestContext.Current.CancellationToken)
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
                .ProvisionAndConnectAsync(provisioningSettings, CreateX509AuthenticationProvider(), cancellationToken: TestContext.Current.CancellationToken)
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

            X509AuthenticationProvider authentication = CreateX509AuthenticationProvider(operationalKey);
            Assert.True(authentication.HasCertificateSigningHandler);

            AuthenticationCredentials hubCredentials = await authentication.GetIotHubCredentialsAsync(AssignedHub, DeviceId, issuedChain, TestContext.Current.CancellationToken);
            Assert.NotNull(hubCredentials.ClientCertificate);
            Assert.Equal(issuedCertificate.Thumbprint, hubCredentials.ClientCertificate.Thumbprint);
            Assert.True(hubCredentials.ClientCertificate.HasPrivateKey);

            // Getting credentials for the same chain again reuses the same certificate rather than rebuilding it on every connect.
            Assert.Same(hubCredentials.ClientCertificate, (await authentication.GetIotHubCredentialsAsync(AssignedHub, DeviceId, issuedChain, TestContext.Current.CancellationToken)).ClientCertificate);
            Assert.Same(hubCredentials.ClientCertificate, (await authentication.GetIotHubCredentialsAsync(AssignedHub, DeviceId, (string[])issuedChain.Clone(), TestContext.Current.CancellationToken)).ClientCertificate);
        }

        [Fact]
        public async Task IotHubAuthenticationPresentsTheOriginalCertificateUntilACertificateIsIssued()
        {
            using RSA operationalKey = RSA.Create(2048);
            using X509Certificate2 issuedCertificate = CreateIssuedCertificate(operationalKey);
            string[] issuedChain = new[] { Convert.ToBase64String(issuedCertificate.RawData) };

            // A provider that cannot present issued certificates keeps presenting the original certificate.
            X509AuthenticationProvider authentication = CreateX509AuthenticationProvider();
            Assert.False(authentication.HasCertificateSigningHandler);
            Assert.Same(authentication.ClientCertificate, (await authentication.GetIotHubCredentialsAsync(AssignedHub, DeviceId, issuedChain, TestContext.Current.CancellationToken)).ClientCertificate);

            X509AuthenticationProvider certificateSigningAuthentication = CreateX509AuthenticationProvider(operationalKey);
            Assert.Same(certificateSigningAuthentication.ClientCertificate, (await certificateSigningAuthentication.GetIotHubCredentialsAsync(AssignedHub, DeviceId, null, TestContext.Current.CancellationToken)).ClientCertificate);
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

            SymmetricKeyAuthenticationProvider authentication = new(RegistrationId, IdScope, SymmetricKey, operationalKey);

            ProvisioningSettings provisioningSettings = new(IdScope)
            {
                RegistrationId = RegistrationId,
                GlobalEndpointAddress = GlobalDeviceEndpoint,
            };

            await connectionClient
                .ProvisionAndConnectAsync(provisioningSettings, authentication, new ProvisioningCertificateSigningRequest(csrBase64), cancellationToken: TestContext.Current.CancellationToken)
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
        public async Task RestoredIssuedCertificatesArePreparedByTheSuppliedProvider()
        {
            using RSA operationalKey = RSA.Create(2048);
            using X509Certificate2 issuedCertificate = CreateIssuedCertificate(operationalKey);
            string[] issuedChain = new[] { Convert.ToBase64String(issuedCertificate.RawData) };
            ConnectionContext restoredContext = new()
            {
                DeviceId = DeviceId,
                IotHubHostName = AssignedHub,
                ConnectionProfile = IotHubConnectionType.Mqttv3,
                IssuedClientCertificates = issuedChain,
                AuthenticationProvider = CreateX509AuthenticationProvider(),
            };
            X509AuthenticationProvider authentication = CreateX509AuthenticationProvider(operationalKey);
            using MockConnectionMqttClient mockMqttClient = new();
            MqttConnect? hubConnect = null;
            mockMqttClient.OnConnect = connect =>
            {
                hubConnect = connect;
                return Task.FromResult(new MqttConnectAck() { ResultCode = MqttConnectReasonCode.Success });
            };
            using TestConnectionClient connectionClient = new(new() { MqttClient = mockMqttClient }, restoredContext);

            ConnectionContext context = await connectionClient.ProvisionAndConnectAsync(
                new(IdScope), authentication,
                cancellationToken: TestContext.Current.CancellationToken);

            Assert.Same(restoredContext, context);
            Assert.Same(authentication, context.AuthenticationProvider);
            Assert.Equal(1, mockMqttClient.ConnectAttemptCount);
            Assert.NotNull(hubConnect);
            Assert.Equal(AssignedHub, hubConnect.HostName);
            Assert.Equal(issuedCertificate.Thumbprint, hubConnect.ClientCertificate?.Thumbprint);
            Assert.True(hubConnect.ClientCertificate?.HasPrivateKey);
        }

        [Fact]
        public async Task HubIssuedCertificatesAreHandledByTheProviderAndPresentedOnReconnect()
        {
            using RSA operationalKey = RSA.Create(2048);
            using X509Certificate2 issuedCertificate = CreateIssuedCertificate(operationalKey);
            string[] issuedChain = new[] { Convert.ToBase64String(issuedCertificate.RawData) };
            int callbackCount = 0;
            X509AuthenticationProvider authentication = new(
                CreateClientCertificate(),
                certificates =>
                {
                    callbackCount++;
                    return Task.FromResult(issuedCertificate.CopyWithPrivateKey(operationalKey));
                });

            using MockConnectionMqttClient mockMqttClient = new();
            MockCertificateIssuingProvisioningService mockDps = new(mockMqttClient, issuedCertificateChain: null);
            Assert.NotNull(mockDps);
            MqttConnect? latestHubConnect = null;
            TaskCompletionSource reconnectedWithIssuedCertificate = new(TaskCreationOptions.RunContinuationsAsynchronously);
            mockMqttClient.OnConnect = connect =>
            {
                if (connect.HostName == AssignedHub)
                {
                    latestHubConnect = connect;
                    if (connect.ClientCertificate?.Thumbprint == issuedCertificate.Thumbprint)
                    {
                        reconnectedWithIssuedCertificate.TrySetResult();
                    }
                }
                return Task.FromResult(new MqttConnectAck() { ResultCode = MqttConnectReasonCode.Success });
            };
            using ConnectionClient connectionClient = new(new() { MqttClient = mockMqttClient });
            ConnectionContext context = await connectionClient.ProvisionAndConnectAsync(
                new(IdScope), authentication,
                cancellationToken: TestContext.Current.CancellationToken);
            IotHubCertificateSigningRequest request = new(RegistrationId, CreateCertificateSigningRequest(operationalKey), null, "*");
            CertificateSigningOperation operation = await connectionClient.SendCertificateSigningRequestAsync(
                request, TestContext.Current.CancellationToken);

            await mockMqttClient.SimulatePublishReceivedAsync(new()
            {
                Topic = $"$iothub/credentials/res/200/?$rid={request.RequestId}",
                Payload = JsonSerializer.SerializeToUtf8Bytes(new CertificateSigningResponse()
                {
                    CorrelationId = request.RequestId,
                    Certificates = issuedChain,
                }),
            });
            await operation.Completed.WaitAsync(s_testTimeout, TestContext.Current.CancellationToken);

            Assert.Equal(1, callbackCount);
            Assert.Equal(issuedChain, context.IssuedClientCertificates);
            await mockMqttClient.SimulateServerDisconnectAsync(MqttDisconnectReason.ServerBusy);
            await reconnectedWithIssuedCertificate.Task.WaitAsync(s_testTimeout, TestContext.Current.CancellationToken);
            Assert.Equal(1, callbackCount);
            Assert.NotNull(latestHubConnect);
            Assert.Equal(issuedCertificate.Thumbprint, latestHubConnect.ClientCertificate?.Thumbprint);
            Assert.True(latestHubConnect.ClientCertificate?.HasPrivateKey);
            Assert.Same(authentication.ClientCertificate, (await authentication.GetProvisioningCredentialsAsync(TestContext.Current.CancellationToken)).ClientCertificate);
        }

        [Fact]
        public async Task HubCertificateSigningRequestRequiresACertificateSigningHandler()
        {
            using RSA operationalKey = RSA.Create(2048);
            using MockConnectionMqttClient mockMqttClient = new();
            MockCertificateIssuingProvisioningService mockDps = new(mockMqttClient, issuedCertificateChain: null);
            Assert.NotNull(mockDps);
            mockMqttClient.OnConnect = connect => Task.FromResult(new MqttConnectAck() { ResultCode = MqttConnectReasonCode.Success });
            using ConnectionClient connectionClient = new(new() { MqttClient = mockMqttClient });
            await connectionClient.ProvisionAndConnectAsync(
                new(IdScope), CreateX509AuthenticationProvider(),
                cancellationToken: TestContext.Current.CancellationToken);

            IotHubCertificateSigningRequest request = new(RegistrationId, CreateCertificateSigningRequest(operationalKey), null, "*");
            await Assert.ThrowsAsync<InvalidOperationException>(() =>
                connectionClient.SendCertificateSigningRequestAsync(request, TestContext.Current.CancellationToken));
        }

        [Fact]
        public async Task ProvisionAndConnectRequiresAnAuthenticationProvider()
        {
            using MockConnectionMqttClient mockMqttClient = new();
            using TestConnectionClient connectionClient = new(new() { MqttClient = mockMqttClient });
            ArgumentNullException exception = await Assert.ThrowsAsync<ArgumentNullException>(() =>
                connectionClient.ProvisionAndConnectAsync(
                    new(IdScope), null!,
                    cancellationToken: TestContext.Current.CancellationToken));
            Assert.Equal("authenticationProvider", exception.ParamName);
            Assert.Equal(0, mockMqttClient.ConnectAttemptCount);
        }

        [Fact]
        public async Task ACertificateSigningRequestRequiresAProviderWithACertificateSigningHandler()
        {
            Assert.Null(typeof(ProvisioningSettings).GetProperty("CertificateSigningRequest"));

            using RSA operationalKey = RSA.Create(2048);
            using MockConnectionMqttClient mockMqttClient = new();
            using TestConnectionClient connectionClient = new(new() { MqttClient = mockMqttClient });
            ProvisioningCertificateSigningRequest certificateSigningRequest = new(CreateCertificateSigningRequest(operationalKey));

            AuthenticationProvider[] providersWithoutAHandler =
            {
                CreateX509AuthenticationProvider(),
                new SymmetricKeyAuthenticationProvider(RegistrationId, IdScope, SymmetricKey),
            };

            foreach (AuthenticationProvider provider in providersWithoutAHandler)
            {
                Assert.False(provider.HasCertificateSigningHandler);
                ArgumentException exception = await Assert.ThrowsAsync<ArgumentException>(() =>
                    connectionClient.ProvisionAndConnectAsync(
                        new(IdScope), provider, certificateSigningRequest,
                        cancellationToken: TestContext.Current.CancellationToken));
                Assert.Equal("authenticationProvider", exception.ParamName);
            }

            Assert.Equal(0, mockMqttClient.ConnectAttemptCount);
        }

        [Fact]
        public async Task ProvisionAndConnectWithACertificateSigningProviderRequiresTheCertificateSigningRequest()
        {
            using RSA operationalKey = RSA.Create(2048);
            using MockConnectionMqttClient mockMqttClient = new();
            using TestConnectionClient connectionClient = new(new() { MqttClient = mockMqttClient });

            ArgumentNullException exception = await Assert.ThrowsAsync<ArgumentNullException>(() =>
                connectionClient.ProvisionAndConnectAsync(
                    new(IdScope), CreateX509AuthenticationProvider(operationalKey), null!,
                    cancellationToken: TestContext.Current.CancellationToken));
            Assert.Equal("certificateSigningRequest", exception.ParamName);
            Assert.Equal(0, mockMqttClient.ConnectAttemptCount);
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
                CreateClientCertificate(),
                issuedCertificates =>
                {
                    callbackInvocations.Add(issuedCertificates);
                    return Task.FromResult(issuedCertificateWithKey);
                });

            ProvisioningSettings provisioningSettings = new(IdScope)
            {
                RegistrationId = RegistrationId,
                GlobalEndpointAddress = GlobalDeviceEndpoint,
            };

            await connectionClient
                .ProvisionAndConnectAsync(provisioningSettings, authentication, new ProvisioningCertificateSigningRequest(csrBase64), cancellationToken: TestContext.Current.CancellationToken)
                .WaitAsync(s_testTimeout, TestContext.Current.CancellationToken);

            // The callback runs once for the issued chain, even though the chain is also seen on the hub CONNECT.
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
            SymmetricKeyAuthenticationProvider authentication = new(
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
            };

            await connectionClient
                .ProvisionAndConnectAsync(provisioningSettings, authentication, new ProvisioningCertificateSigningRequest(csrBase64), cancellationToken: TestContext.Current.CancellationToken)
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
                CreateClientCertificate(),
                _ => throw new InvalidOperationException("callback failed"));

            ProvisioningSettings provisioningSettings = new(IdScope)
            {
                RegistrationId = RegistrationId,
                GlobalEndpointAddress = GlobalDeviceEndpoint,
            };

            InvalidOperationException exception = await Assert.ThrowsAsync<InvalidOperationException>(
                async () => await connectionClient
                    .ProvisionAndConnectAsync(provisioningSettings, authentication, new ProvisioningCertificateSigningRequest(csrBase64), cancellationToken: TestContext.Current.CancellationToken)
                    .WaitAsync(s_testTimeout, TestContext.Current.CancellationToken));
            Assert.Equal("callback failed", exception.Message);
        }

        [Fact]
        public async Task CallbackProvidersInvokeTheCallbackFromGetIotHubCredentialsAsyncOncePerIssuedChain()
        {
            using RSA operationalKey = RSA.Create(2048);
            using X509Certificate2 issuedCertificate = CreateIssuedCertificate(operationalKey);
            using X509Certificate2 issuedCertificateWithKey = issuedCertificate.CopyWithPrivateKey(operationalKey);
            using X509Certificate2 secondIssuedCertificate = CreateIssuedCertificate(operationalKey);
            using X509Certificate2 secondIssuedCertificateWithKey = secondIssuedCertificate.CopyWithPrivateKey(operationalKey);
            string[] issuedChain = new[] { Convert.ToBase64String(issuedCertificate.RawData) };
            string[] secondIssuedChain = new[] { Convert.ToBase64String(secondIssuedCertificate.RawData) };
            int callbackCount = 0;
            Func<IReadOnlyList<string>, Task<X509Certificate2>> callback = chain =>
            {
                callbackCount++;
                return Task.FromResult(chain[0] == issuedChain[0] ? issuedCertificateWithKey : secondIssuedCertificateWithKey);
            };

            X509AuthenticationProvider x509 = new(CreateClientCertificate(), callback);
            Assert.Same(x509.ClientCertificate, (await x509.GetIotHubCredentialsAsync(AssignedHub, DeviceId, null, TestContext.Current.CancellationToken)).ClientCertificate);
            Assert.Equal(0, callbackCount);
            Assert.Same(issuedCertificateWithKey, (await x509.GetIotHubCredentialsAsync(AssignedHub, DeviceId, issuedChain, TestContext.Current.CancellationToken)).ClientCertificate);
            Assert.Same(issuedCertificateWithKey, (await x509.GetIotHubCredentialsAsync(AssignedHub, DeviceId, issuedChain, TestContext.Current.CancellationToken)).ClientCertificate);
            Assert.Equal(1, callbackCount);

            // A newly issued chain is handed to the callback the next time credentials are requested.
            Assert.Same(secondIssuedCertificateWithKey, (await x509.GetIotHubCredentialsAsync(AssignedHub, DeviceId, secondIssuedChain, TestContext.Current.CancellationToken)).ClientCertificate);
            Assert.Equal(2, callbackCount);

            // A symmetric key provider uses a SAS token until a certificate is issued, then hands the issued chain to its callback.
            SymmetricKeyAuthenticationProvider symmetricKey = new(RegistrationId, IdScope, SymmetricKey, callback);
            Assert.Null((await symmetricKey.GetIotHubCredentialsAsync(AssignedHub, DeviceId, null, TestContext.Current.CancellationToken)).ClientCertificate);
            Assert.Same(issuedCertificateWithKey, (await symmetricKey.GetIotHubCredentialsAsync(AssignedHub, DeviceId, issuedChain, TestContext.Current.CancellationToken)).ClientCertificate);

            X509AuthenticationProvider failing = new(CreateClientCertificate(), _ => throw new InvalidOperationException("callback failed"));
            await Assert.ThrowsAsync<InvalidOperationException>(() => failing.GetIotHubCredentialsAsync(AssignedHub, DeviceId, issuedChain, TestContext.Current.CancellationToken));
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

        private static X509AuthenticationProvider CreateX509AuthenticationProvider()
        {
            return new X509AuthenticationProvider(CreateClientCertificate());
        }

        private static X509AuthenticationProvider CreateX509AuthenticationProvider(AsymmetricAlgorithm issuedCertificatePrivateKey)
        {
            return new X509AuthenticationProvider(CreateClientCertificate(), issuedCertificatePrivateKey);
        }

        private static X509Certificate2 CreateClientCertificate()
        {
            using RSA key = RSA.Create(2048);
            var certificateRequest = new CertificateRequest($"CN={RegistrationId}", key, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);
            X509Certificate2 certificate = certificateRequest.CreateSelfSigned(DateTimeOffset.UtcNow.AddDays(-1), DateTimeOffset.UtcNow.AddDays(1));

            return certificate;
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
