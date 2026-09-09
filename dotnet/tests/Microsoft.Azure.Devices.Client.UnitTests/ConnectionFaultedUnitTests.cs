// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

using Microsoft.Azure.Devices.Client.Exceptions;
using Microsoft.Azure.Devices.Client.Models;
using Microsoft.Azure.Devices.Client.Mqtt;
using Microsoft.Azure.Devices.Client.Provisioning.Models;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text.Json;
using Xunit;

namespace Microsoft.Azure.Devices.Client.UnitTests
{
    /// <summary>
    /// Tests for how <see cref="AbstractConnectionClient"/> reacts to the connection layer giving up on a connection.
    /// </summary>
    public class ConnectionFaultedUnitTests
    {
        private const string GlobalDeviceEndpoint = "global.azure-devices-provisioning.net";
        private const string IdScope = "0ne00000000";
        private const string RegistrationId = "someRegistrationId";
        private const string DeviceId = "someDeviceId";
        private const string FirstAssignedHub = "first-hub.azure-devices.net";
        private const string SecondAssignedHub = "second-hub.azure-devices.net";

        private static readonly TimeSpan s_testTimeout = TimeSpan.FromSeconds(30);

        // How long to wait before concluding that this client did not start doing something it should not do.
        private static readonly TimeSpan s_negativeTestTimeout = TimeSpan.FromSeconds(2);

        [Fact]
        public async Task IdentityFaultReprovisionsAndConnectsToNewlyAssignedHub()
        {
            using MockConnectionMqttClient mockMqttClient = new();
            MockDeviceProvisioningService mockDps = new(mockMqttClient, registrationCount => registrationCount == 1 ? FirstAssignedHub : SecondAssignedHub);

            TaskCompletionSource connectedToSecondHub = new(TaskCreationOptions.RunContinuationsAsynchronously);
            List<string> connectedHostNames = new();
            mockMqttClient.OnConnect = connect =>
            {
                lock (connectedHostNames)
                {
                    connectedHostNames.Add(connect.HostName);
                }

                if (connect.HostName == SecondAssignedHub)
                {
                    connectedToSecondHub.TrySetResult();
                }

                return Task.FromResult(new MqttConnectAck() { ResultCode = MqttConnectReasonCode.Success });
            };

            using TestConnectionClient connectionClient = new(new() { MqttClient = mockMqttClient });

            ConnectionContext connectionContext = await connectionClient.ProvisionAndConnectAsync(
                CreateProvisioningSettings(),
                CreateAuthenticationProvider(),
                TestContext.Current.CancellationToken);

            Assert.Equal(FirstAssignedHub, connectionContext.IotHubHostName);
            Assert.Equal(1, mockDps.RegistrationCount);

            // The hub rejects this device's identity mid-session, which the connection layer reports as a fault rather
            // than retrying it.
            await mockMqttClient.SimulateServerDisconnectAsync(MqttDisconnectReason.NotAuthorized);

            // This client should recover on its own by registering with DPS again and connecting to the hub it is assigned this time.
            await connectedToSecondHub.Task.WaitAsync(s_testTimeout, TestContext.Current.CancellationToken);

            Assert.Equal(2, mockDps.RegistrationCount);
            Assert.Equal(ConnectionEndpoint.IotHub, connectionClient.CurrentEndpoint);
            Assert.Equal(SecondAssignedHub, connectionClient.GetCurrentConnectionContext()!.IotHubHostName);

            lock (connectedHostNames)
            {
                Assert.Equal(
                    new[] { GlobalDeviceEndpoint, FirstAssignedHub, GlobalDeviceEndpoint, SecondAssignedHub },
                    connectedHostNames);
            }
        }

        [Fact]
        public async Task IdentityFaultOnlyStartsOneProvisioningAttemptAtATime()
        {
            using MockConnectionMqttClient mockMqttClient = new();

            // Hold the second registration open so that a second fault arrives while this client is still re-provisioning.
            TaskCompletionSource releaseSecondRegistration = new(TaskCreationOptions.RunContinuationsAsynchronously);
            TaskCompletionSource secondRegistrationStarted = new(TaskCreationOptions.RunContinuationsAsynchronously);
            MockDeviceProvisioningService mockDps = new(mockMqttClient, registrationCount => registrationCount == 1 ? FirstAssignedHub : SecondAssignedHub);
            mockDps.RegistrationRequested += async registrationCount =>
            {
                if (registrationCount == 2)
                {
                    secondRegistrationStarted.TrySetResult();
                    await releaseSecondRegistration.Task;
                }
            };

            using TestConnectionClient connectionClient = new(new() { MqttClient = mockMqttClient });

            await connectionClient.ProvisionAndConnectAsync(
                CreateProvisioningSettings(),
                CreateAuthenticationProvider(),
                TestContext.Current.CancellationToken);

            await mockMqttClient.SimulateServerDisconnectAsync(MqttDisconnectReason.NotAuthorized);
            await secondRegistrationStarted.Task.WaitAsync(s_testTimeout, TestContext.Current.CancellationToken);

            // A second identity fault while the first recovery is still running must not start a competing one.
            await mockMqttClient.SimulateServerDisconnectAsync(MqttDisconnectReason.NotAuthorized);
            await Task.Delay(s_negativeTestTimeout, TestContext.Current.CancellationToken);

            Assert.Equal(2, mockDps.RegistrationCount);

            releaseSecondRegistration.TrySetResult();
        }

        [Fact]
        public async Task TerminalFaultThatIsNotAboutIdentityDoesNotReprovision()
        {
            using MockConnectionMqttClient mockMqttClient = new();
            MockDeviceProvisioningService mockDps = new(mockMqttClient, _ => FirstAssignedHub);

            using TestConnectionClient connectionClient = new(new() { MqttClient = mockMqttClient });

            await connectionClient.ProvisionAndConnectAsync(
                CreateProvisioningSettings(),
                CreateAuthenticationProvider(),
                TestContext.Current.CancellationToken);

            int connectAttemptsBeforeFault = mockMqttClient.ConnectAttemptCount;

            // This reason is terminal, but it says nothing about this device's identity, so re-provisioning would not fix it.
            await mockMqttClient.SimulateServerDisconnectAsync(MqttDisconnectReason.ServerMoved);
            await Task.Delay(s_negativeTestTimeout, TestContext.Current.CancellationToken);

            Assert.Equal(1, mockDps.RegistrationCount);
            Assert.Equal(connectAttemptsBeforeFault, mockMqttClient.ConnectAttemptCount);
        }

        [Fact]
        public async Task IdentityFaultDoesNotReprovisionADeviceThatWasNeverProvisioned()
        {
            using MockConnectionMqttClient mockMqttClient = new();
            MockDeviceProvisioningService mockDps = new(mockMqttClient, _ => FirstAssignedHub);

            using TestConnectionClient connectionClient = new(new() { MqttClient = mockMqttClient });

            await connectionClient.ConnectAsync(
                new ConnectionContext()
                {
                    AuthenticationProvider = CreateAuthenticationProvider(),
                    DeviceId = DeviceId,
                    IotHubHostName = FirstAssignedHub,
                    IsGen2Hub = false,
                },
                cancellationToken: TestContext.Current.CancellationToken);

            int connectAttemptsBeforeFault = mockMqttClient.ConnectAttemptCount;

            await mockMqttClient.SimulateServerDisconnectAsync(MqttDisconnectReason.NotAuthorized);
            await Task.Delay(s_negativeTestTimeout, TestContext.Current.CancellationToken);

            // This device was connected with credentials the application supplied directly, so there is no registration
            // for this client to renew on its own.
            Assert.Equal(0, mockDps.RegistrationCount);
            Assert.Equal(connectAttemptsBeforeFault, mockMqttClient.ConnectAttemptCount);
        }

        [Fact]
        public async Task IdentityFaultReportedToTheCallerDoesNotReprovision()
        {
            using MockConnectionMqttClient mockMqttClient = new();
            MockDeviceProvisioningService mockDps = new(mockMqttClient, _ => FirstAssignedHub);

            // Provisioning succeeds, but the hub it assigns refuses this device's identity on the very first connect.
            mockMqttClient.OnConnect = connect => Task.FromResult(new MqttConnectAck()
            {
                ResultCode = connect.HostName == GlobalDeviceEndpoint
                    ? MqttConnectReasonCode.Success
                    : MqttConnectReasonCode.NotAuthorized,
            });

            using TestConnectionClient connectionClient = new(new() { MqttClient = mockMqttClient });

            DeviceException exception = await Assert.ThrowsAsync<DeviceException>(
                () => connectionClient.ProvisionAndConnectAsync(
                    CreateProvisioningSettings(),
                    CreateAuthenticationProvider(),
                    TestContext.Current.CancellationToken));

            Assert.Equal(ErrorRetryability.IdentityTerminal, exception.Retryability);

            await Task.Delay(s_negativeTestTimeout, TestContext.Current.CancellationToken);

            // The caller was told about the fault and decides what to do, so this client must not also be recovering from it.
            Assert.Equal(1, mockDps.RegistrationCount);
        }

        private static ProvisioningSettings CreateProvisioningSettings()
        {
            return new ProvisioningSettings(IdScope)
            {
                RegistrationId = RegistrationId,
                GlobalEndpointAddress = GlobalDeviceEndpoint,
            };
        }

        private static X509AuthenticationProvider CreateAuthenticationProvider()
        {
            using RSA key = RSA.Create(2048);
            var certificateRequest = new CertificateRequest($"CN={RegistrationId}", key, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);
            X509Certificate2 certificate = certificateRequest.CreateSelfSigned(DateTimeOffset.UtcNow.AddDays(-1), DateTimeOffset.UtcNow.AddDays(1));

            return new X509AuthenticationProvider(certificate);
        }

        /// <summary>
        /// The Device Provisioning Service half of the MQTT registration flow: it answers each registration request with
        /// an "assigning" status and each status poll with the hub that the device was assigned.
        /// </summary>
        private sealed class MockDeviceProvisioningService
        {
            private const string RegisterTopicPrefix = "$dps/registrations/PUT/";
            private const string GetOperationStatusTopicPrefix = "$dps/registrations/GET/";
            private const string OperationId = "someOperationId";

            private readonly MockConnectionMqttClient _mqttClient;
            private readonly Func<int, string> _assignedHubSelector;

            private int _registrationCount;

            public MockDeviceProvisioningService(MockConnectionMqttClient mqttClient, Func<int, string> assignedHubSelector)
            {
                _mqttClient = mqttClient;
                _assignedHubSelector = assignedHubSelector;
                _mqttClient.OnPublish = HandlePublishAsync;
            }

            /// <summary>How many registrations this service has been asked to start.</summary>
            public int RegistrationCount => Volatile.Read(ref _registrationCount);

            /// <summary>
            /// Raised with the running registration count each time a registration request arrives, before it is
            /// answered, so that a test can hold the flow open.
            /// </summary>
            public event Func<int, Task>? RegistrationRequested;

            private async Task<MqttPublishAck> HandlePublishAsync(MqttPublish publish)
            {
                if (publish.Topic.StartsWith(RegisterTopicPrefix))
                {
                    int registrationCount = Interlocked.Increment(ref _registrationCount);

                    if (RegistrationRequested != null)
                    {
                        await RegistrationRequested.Invoke(registrationCount);
                    }

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
                            AssignedHub = _assignedHubSelector(RegistrationCount),
                            Status = ProvisioningRegistrationStatus.Assigned,
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
    }
}
