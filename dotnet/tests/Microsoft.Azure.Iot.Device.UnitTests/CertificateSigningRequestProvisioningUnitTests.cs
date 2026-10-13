// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Authentication;
using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Mqtt;
using Microsoft.Azure.Iot.Device.Provisioning;
using Microsoft.Azure.Iot.Device.Provisioning.Models;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using System.Text.Json;
using Xunit;

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

            await authentication.HandleClientCertificatesIssuedAsync(issuedChain);

            AuthenticationCredentials hubCredentials = authentication.GetIotHubAuthentication(AssignedHub, DeviceId, issuedChain);
            Assert.NotNull(hubCredentials.ClientCertificate);
            Assert.Equal(issuedCertificate.Thumbprint, hubCredentials.ClientCertificate.Thumbprint);
            Assert.True(hubCredentials.ClientCertificate.HasPrivateKey);

            // Handling the same chain again reuses the same certificate rather than rebuilding it on every connect.
            await authentication.HandleClientCertificatesIssuedAsync(issuedChain);
            Assert.Same(hubCredentials.ClientCertificate, authentication.GetIotHubAuthentication(AssignedHub, DeviceId, issuedChain).ClientCertificate);

            // DPS is always authenticated with the original client certificate.
            Assert.Same(authentication.ClientCertificate, authentication.GetDeviceProvisioningServiceAuthentication().ClientCertificate);
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
            await authentication.HandleClientCertificatesIssuedAsync(issuedChain);

            Assert.Same(authentication.ClientCertificate, authentication.GetIotHubAuthentication(AssignedHub, DeviceId, issuedChain).ClientCertificate);

            authentication = CreateAuthenticationProvider(operationalKey);
            Assert.Same(authentication.ClientCertificate, authentication.GetIotHubAuthentication(AssignedHub, DeviceId, null).ClientCertificate);
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
        public void SymmetricKeyWithIssuedCertificateAuthenticatesTheHubWithASasTokenUntilACertificateIsIssued()
        {
            using RSA operationalKey = RSA.Create(2048);
            SymmetricKeyWithIssuedCertificateAuthenticationProvider authentication = new(RegistrationId, IdScope, SymmetricKey, operationalKey);

            AuthenticationCredentials hubCredentials = authentication.GetIotHubAuthentication(AssignedHub, DeviceId, null);

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
                _ => throw new InvalidOperationException("callback failed"));

            ProvisioningSettings provisioningSettings = new(IdScope)
            {
                RegistrationId = RegistrationId,
                GlobalEndpointAddress = GlobalDeviceEndpoint,
                CertificateSigningRequest = new(operationalKey, csrBase64),
            };

            InvalidOperationException exception = await Assert.ThrowsAsync<InvalidOperationException>(
                async () => await connectionClient
                    .ProvisionAndConnectAsync(provisioningSettings, authentication, cancellationToken: TestContext.Current.CancellationToken)
                    .WaitAsync(s_testTimeout, TestContext.Current.CancellationToken));
            Assert.Equal("callback failed", exception.Message);
        }

        [Fact]
        public async Task CallbackProvidersFallBackToTheirOriginalCredentialsUntilTheCallbackHasRun()
        {
            using RSA operationalKey = RSA.Create(2048);
            using X509Certificate2 issuedCertificate = CreateIssuedCertificate(operationalKey);
            using X509Certificate2 issuedCertificateWithKey = issuedCertificate.CopyWithPrivateKey(operationalKey);
            string[] issuedChain = new[] { Convert.ToBase64String(issuedCertificate.RawData) };
            Func<IReadOnlyList<string>, Task<X509Certificate2>> callback = _ => Task.FromResult(issuedCertificateWithKey);

            X509AuthenticationProvider x509 = new(CreateAuthenticationProvider().ClientCertificate, callback);
            Assert.Same(x509.ClientCertificate, x509.GetIotHubAuthentication(AssignedHub, DeviceId, issuedChain).ClientCertificate);

            await x509.HandleClientCertificatesIssuedAsync(issuedChain);
            Assert.Same(issuedCertificateWithKey, x509.GetIotHubAuthentication(AssignedHub, DeviceId, issuedChain).ClientCertificate);
            Assert.Same(x509.ClientCertificate, x509.GetDeviceProvisioningServiceAuthentication().ClientCertificate);

            SymmetricKeyWithIssuedCertificateAuthenticationProvider symmetricKey = new(RegistrationId, IdScope, SymmetricKey, callback);
            AuthenticationCredentials beforeCallback = symmetricKey.GetIotHubAuthentication(AssignedHub, DeviceId, issuedChain);
            Assert.Null(beforeCallback.ClientCertificate);
            Assert.StartsWith("SharedAccessSignature ", Encoding.UTF8.GetString(beforeCallback.Password));

            await symmetricKey.HandleClientCertificatesIssuedAsync(issuedChain);
            Assert.Same(issuedCertificateWithKey, symmetricKey.GetIotHubAuthentication(AssignedHub, DeviceId, issuedChain).ClientCertificate);
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

            return new X509AuthenticationProvider(certificate, issuedCertificatePrivateKey: issuedCertificatePrivateKey);
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
