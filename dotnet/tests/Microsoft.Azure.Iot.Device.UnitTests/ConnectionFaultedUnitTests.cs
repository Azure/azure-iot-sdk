// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Exceptions;
using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Mqtt;
using Microsoft.Azure.Iot.Device.Provisioning.Models;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text.Json;
using Xunit;

namespace Microsoft.Azure.Iot.Device.UnitTests
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

        // The prefix of every topic that the Device Provisioning Service registration flow uses.
        private const string ProvisioningTopicPrefix = "$dps/";

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
        public async Task TerminalFaultThatIsNotAboutIdentityRaisesConnectionFaultedAsync()
        {
            using MockConnectionMqttClient mockMqttClient = new();
            MockDeviceProvisioningService mockDps = new(mockMqttClient, _ => FirstAssignedHub);

            using TestConnectionClient connectionClient = new(new() { MqttClient = mockMqttClient });

            await connectionClient.ProvisionAndConnectAsync(
                CreateProvisioningSettings(),
                CreateAuthenticationProvider(),
                TestContext.Current.CancellationToken);

            TaskCompletionSource<ConnectionFaultedEventArgs> connectionFaulted = new(TaskCreationOptions.RunContinuationsAsynchronously);
            connectionClient.ConnectionFaultedAsync += args =>
            {
                connectionFaulted.TrySetResult(args);
                return Task.CompletedTask;
            };

            // This reason is terminal, but it says nothing about this device's identity, so nothing on this client
            // will bring the connection back: the application must be told so that it can connect again itself.
            await mockMqttClient.SimulateServerDisconnectAsync(MqttDisconnectReason.ServerMoved);

            ConnectionFaultedEventArgs raisedArgs = await connectionFaulted.Task.WaitAsync(s_testTimeout, TestContext.Current.CancellationToken);

            Assert.Equal(ErrorRetryability.Terminal, raisedArgs.Exception.Retryability);
        }

        [Fact]
        public async Task ConnectionFaultedAsyncIsNotRaisedWhenAnIdentityFaultRecoversOnItsOwn()
        {
            using MockConnectionMqttClient mockMqttClient = new();
            MockDeviceProvisioningService mockDps = new(mockMqttClient, registrationCount => registrationCount == 1 ? FirstAssignedHub : SecondAssignedHub);

            TaskCompletionSource connectedToSecondHub = new(TaskCreationOptions.RunContinuationsAsynchronously);
            mockMqttClient.OnConnect = connect =>
            {
                if (connect.HostName == SecondAssignedHub)
                {
                    connectedToSecondHub.TrySetResult();
                }

                return Task.FromResult(new MqttConnectAck() { ResultCode = MqttConnectReasonCode.Success });
            };

            using TestConnectionClient connectionClient = new(new() { MqttClient = mockMqttClient });

            bool connectionFaultedAsyncRaised = false;
            connectionClient.ConnectionFaultedAsync += _ =>
            {
                connectionFaultedAsyncRaised = true;
                return Task.CompletedTask;
            };

            await connectionClient.ProvisionAndConnectAsync(
                CreateProvisioningSettings(),
                CreateAuthenticationProvider(),
                TestContext.Current.CancellationToken);

            // The hub rejects this device's identity mid-session, but this client recovers on its own by
            // re-provisioning and connecting to the hub it is assigned this time, so the application is never told
            // that the connection is gone for good.
            await mockMqttClient.SimulateServerDisconnectAsync(MqttDisconnectReason.NotAuthorized);
            await connectedToSecondHub.Task.WaitAsync(s_testTimeout, TestContext.Current.CancellationToken);

            await Task.Delay(s_negativeTestTimeout, TestContext.Current.CancellationToken);

            Assert.False(connectionFaultedAsyncRaised);
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
                    ConnectionProfile = ConnectionProfile.Classic,
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
        public async Task IdentityFaultReportedToTheCallerDoesNotReprovision() //TODO huh?
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

        [Fact]
        public Task TerminalFaultWhileReprovisioningCancelsPendingPublish()
        {
            return AssertPendingOperationIsCanceledByTerminalReprovisioningFaultAsync(
                (mockMqttClient, operationAttempted) =>
                {
                    // The registration flow's publishes must keep working, but a feature client's publish finds the
                    // connection gone, which is what makes it wait for this client to re-establish the connection.
                    Func<MqttPublish, Task<MqttPublishAck>> handleProvisioningPublishAsync = mockMqttClient.OnPublish!;
                    mockMqttClient.OnPublish = publish =>
                    {
                        if (publish.Topic.StartsWith(ProvisioningTopicPrefix))
                        {
                            return handleProvisioningPublishAsync.Invoke(publish);
                        }

                        operationAttempted.TrySetResult();
                        throw new MqttClientNotConnectedException("mock client not connected exception");
                    };
                },
                (connectionClient, cancellationToken) => connectionClient.PublishAsync(
                    new MqttPublish() { Topic = $"devices/{DeviceId}/messages/events/" },
                    cancellationToken));
        }

        [Fact]
        public Task TerminalFaultWhileReprovisioningCancelsPendingSubscribe()
        {
            return AssertPendingOperationIsCanceledByTerminalReprovisioningFaultAsync(
                (mockMqttClient, operationAttempted) =>
                {
                    // The registration flow's subscribe must keep working, but a feature client's subscribe finds the
                    // connection gone, which is what makes it wait for this client to re-establish the connection.
                    mockMqttClient.OnSubscribe = subscribe =>
                    {
                        if (subscribe.TopicFilters.Any(topicFilter => topicFilter.Topic.StartsWith(ProvisioningTopicPrefix)))
                        {
                            return Task.FromResult(MqttObjectHelpers.CreateSuccessfulSuback(subscribe));
                        }

                        operationAttempted.TrySetResult();
                        throw new MqttClientNotConnectedException("mock client not connected exception");
                    };
                },
                (connectionClient, cancellationToken) => connectionClient.SubscribeAsync(
                    new MqttSubscribe($"devices/{DeviceId}/messages/devicebound/#", MqttQualityOfServiceLevel.AtLeastOnce),
                    cancellationToken));
        }

        [Fact]
        public Task TerminalFaultWhileReprovisioningCancelsPendingUnsubscribe()
        {
            return AssertPendingOperationIsCanceledByTerminalReprovisioningFaultAsync(
                (mockMqttClient, operationAttempted) =>
                {
                    // Nothing but the operation under test unsubscribes, so every unsubscribe finds the connection gone,
                    // which is what makes it wait for this client to re-establish the connection.
                    mockMqttClient.OnUnsubscribe = unsubscribe =>
                    {
                        operationAttempted.TrySetResult();
                        throw new MqttClientNotConnectedException("mock client not connected exception");
                    };
                },
                (connectionClient, cancellationToken) => connectionClient.UnsubscribeAsync(
                    new MqttUnsubscribe($"devices/{DeviceId}/messages/devicebound/#"),
                    cancellationToken));
        }

        [Fact]
        public async Task IdentityFaultSendsAPendingPublishOnceTheDeviceIsProvisionedAndConnectedAgain()
        {
            using MockConnectionMqttClient mockMqttClient = new();
            MockDeviceProvisioningService mockDps = new(mockMqttClient, registrationCount => registrationCount == 1 ? FirstAssignedHub : SecondAssignedHub);

            string? currentHubHostName = null;
            mockMqttClient.OnConnect = connect =>
            {
                currentHubHostName = connect.HostName;

                return Task.FromResult(new MqttConnectAck() { ResultCode = MqttConnectReasonCode.Success });
            };

            // The registration flow's publishes must keep working. A feature client's first publish finds the connection
            // gone, which is what makes it wait for this client to re-establish the connection, and every publish after
            // that is sent normally.
            Func<MqttPublish, Task<MqttPublishAck>> handleProvisioningPublishAsync = mockMqttClient.OnPublish!;
            TaskCompletionSource publishAttempted = new(TaskCreationOptions.RunContinuationsAsynchronously);
            int publishAttempts = 0;
            string? hubThePublishWasSentTo = null;
            MqttPublish? sentPublish = null;

            // How many device presence flows had completed by the time the publish was actually sent. This is captured
            // where the publish reaches the wire rather than where the publish call returns, so that it cannot observe a
            // presence flow that completed after this publish was already on its way.
            int devicePresenceFlowsCompleted = 0;
            int devicePresenceFlowsCompletedWhenPublishWasSent = -1;
            mockMqttClient.OnPublish = publish =>
            {
                if (publish.Topic.StartsWith(ProvisioningTopicPrefix))
                {
                    return handleProvisioningPublishAsync.Invoke(publish);
                }

                if (Interlocked.Increment(ref publishAttempts) == 1)
                {
                    publishAttempted.TrySetResult();
                    throw new MqttClientNotConnectedException("mock client not connected exception");
                }

                hubThePublishWasSentTo = currentHubHostName;
                sentPublish = publish;
                devicePresenceFlowsCompletedWhenPublishWasSent = Volatile.Read(ref devicePresenceFlowsCompleted);

                return Task.FromResult(new MqttPublishAck() { ReasonCode = MqttPublishAckReasonCode.Success });
            };

            using TestConnectionClient connectionClient = new(new() { MqttClient = mockMqttClient });

            await connectionClient.ProvisionAndConnectAsync(
                CreateProvisioningSettings(),
                CreateAuthenticationProvider(),
                TestContext.Current.CancellationToken);

            // Watch the device presence flow that the reconnection runs. This is attached before the publish is started,
            // so it is invoked before the handler that releases the waiting publish is.
            connectionClient.DevicePresenceFlowCompletedAsync += args =>
            {
                Interlocked.Increment(ref devicePresenceFlowsCompleted);
                return Task.CompletedTask;
            };

            // A feature client, such as the telemetry client, publishes while the connection is gone, so this operation
            // is left waiting for this client to make the device present again. It runs on its own thread because that
            // wait blocks whichever thread the publish was started on.
            var publish = new MqttPublish() { Topic = $"devices/{DeviceId}/messages/events/" };
            Task<MqttPublishAck> pendingPublish = Task.Run(
                () => connectionClient.PublishAsync(publish, TestContext.Current.CancellationToken),
                TestContext.Current.CancellationToken);

            await publishAttempted.Task.WaitAsync(s_testTimeout, TestContext.Current.CancellationToken);

            // The hub rejects this device's identity, so this client registers again and connects to the hub it is
            // assigned this time, all while that publish waits.
            await mockMqttClient.SimulateServerDisconnectAsync(MqttDisconnectReason.NotAuthorized);

            // The publish was only waiting for this device to be present again, so it must be sent now that it is.
            MqttPublishAck puback = await pendingPublish.WaitAsync(s_testTimeout, TestContext.Current.CancellationToken);

            Assert.Equal(MqttPublishAckReasonCode.Success, puback.ReasonCode);
            Assert.Equal(2, publishAttempts);
            Assert.Same(publish, sentPublish);

            // The publish was only released by the device presence flow that the reconnection ran, so that flow must
            // have completed before this publish call finished.
            Assert.Equal(1, Volatile.Read(ref devicePresenceFlowsCompleted));
            Assert.Equal(1, devicePresenceFlowsCompletedWhenPublishWasSent);

            // The one attempt that was actually sent went out over the connection to the newly assigned hub.
            Assert.Equal(SecondAssignedHub, hubThePublishWasSentTo);
            Assert.Equal(2, mockDps.RegistrationCount);
            Assert.Equal(SecondAssignedHub, connectionClient.GetCurrentConnectionContext()!.IotHubHostName);
        }

        /// <summary>
        /// Provision and connect a device, start an operation that finds the connection gone and waits for it to come
        /// back, then fault that connection on this device's identity and refuse the re-provisioning that follows for a
        /// terminal reason. The waiting operation must be canceled with that fault rather than wait forever.
        /// </summary>
        /// <param name="failOperationWhileDisconnected">
        /// Makes the mock report the operation under test as attempted while this client is not connected. It is given
        /// the source to signal once that attempt has been made.
        /// </param>
        /// <param name="startPendingOperation">Starts the operation under test on the connection client.</param>
        private static async Task AssertPendingOperationIsCanceledByTerminalReprovisioningFaultAsync(
            Action<MockConnectionMqttClient, TaskCompletionSource> failOperationWhileDisconnected,
            Func<TestConnectionClient, CancellationToken, Task> startPendingOperation)
        {
            using MockConnectionMqttClient mockMqttClient = new();
            MockDeviceProvisioningService mockDps = new(mockMqttClient, _ => FirstAssignedHub);

            int dpsConnectAttempts = 0;
            mockMqttClient.OnConnect = connect =>
            {
                if (connect.HostName != GlobalDeviceEndpoint)
                {
                    return Task.FromResult(new MqttConnectAck() { ResultCode = MqttConnectReasonCode.Success });
                }

                // The first registration succeeds so that this device gets connected to a hub. The re-provisioning that
                // the identity fault starts is refused for a reason that no amount of retrying or re-provisioning fixes.
                return Task.FromResult(new MqttConnectAck()
                {
                    ResultCode = Interlocked.Increment(ref dpsConnectAttempts) == 1
                        ? MqttConnectReasonCode.Success
                        : MqttConnectReasonCode.Banned,
                });
            };

            TaskCompletionSource operationAttempted = new(TaskCreationOptions.RunContinuationsAsynchronously);
            failOperationWhileDisconnected(mockMqttClient, operationAttempted);

            using TestConnectionClient connectionClient = new(new() { MqttClient = mockMqttClient });

            await connectionClient.ProvisionAndConnectAsync(
                CreateProvisioningSettings(),
                CreateAuthenticationProvider(),
                TestContext.Current.CancellationToken);

            // A feature client, such as the telemetry client, sends this while the connection is gone, so the operation
            // is left waiting for this client to make the device present again. It runs on its own thread because that
            // wait blocks whichever thread the operation was started on.
            Task pendingOperation = Task.Run(
                () => startPendingOperation(connectionClient, TestContext.Current.CancellationToken),
                TestContext.Current.CancellationToken);

            await operationAttempted.Task.WaitAsync(s_testTimeout, TestContext.Current.CancellationToken);

            // The hub rejects this device's identity, so this client starts re-provisioning while that operation waits.
            await mockMqttClient.SimulateServerDisconnectAsync(MqttDisconnectReason.NotAuthorized);

            // That re-provisioning attempt is the only thing that could have re-established the connection, and it hit a
            // terminal error, so the waiting operation must be canceled rather than left waiting forever.
            OperationCanceledException exception = await Assert.ThrowsAsync<OperationCanceledException>(
                () => pendingOperation.WaitAsync(s_testTimeout, TestContext.Current.CancellationToken));

            DeviceException fault = Assert.IsType<DeviceException>(exception.InnerException);
            Assert.Equal(ErrorRetryability.Terminal, fault.Retryability);

            Assert.Equal(2, dpsConnectAttempts);
            Assert.Equal(1, mockDps.RegistrationCount);
        }

        [Fact]
        public async Task ConnectionFaultedAsyncIsRaisedWhenReprovisioningFailsTerminally()
        {
            using MockConnectionMqttClient mockMqttClient = new();
            MockDeviceProvisioningService mockDps = new(mockMqttClient, _ => FirstAssignedHub);

            int dpsConnectAttempts = 0;
            mockMqttClient.OnConnect = connect =>
            {
                if (connect.HostName != GlobalDeviceEndpoint)
                {
                    return Task.FromResult(new MqttConnectAck() { ResultCode = MqttConnectReasonCode.Success });
                }

                // The first registration succeeds so that this device gets connected to a hub. The re-provisioning that
                // the identity fault starts is refused for a reason that no amount of retrying or re-provisioning fixes.
                return Task.FromResult(new MqttConnectAck()
                {
                    ResultCode = Interlocked.Increment(ref dpsConnectAttempts) == 1
                        ? MqttConnectReasonCode.Success
                        : MqttConnectReasonCode.Banned,
                });
            };

            using TestConnectionClient connectionClient = new(new() { MqttClient = mockMqttClient });

            TaskCompletionSource<ConnectionFaultedEventArgs> connectionFaulted = new(TaskCreationOptions.RunContinuationsAsynchronously);
            connectionClient.ConnectionFaultedAsync += args =>
            {
                connectionFaulted.TrySetResult(args);
                return Task.CompletedTask;
            };

            await connectionClient.ProvisionAndConnectAsync(
                CreateProvisioningSettings(),
                CreateAuthenticationProvider(),
                TestContext.Current.CancellationToken);

            // The hub rejects this device's identity, so this client starts re-provisioning, but that re-provisioning
            // attempt hits a terminal error of its own, so the application must be told that the connection is gone
            // for good rather than being left with no indication that anything went wrong.
            await mockMqttClient.SimulateServerDisconnectAsync(MqttDisconnectReason.NotAuthorized);

            ConnectionFaultedEventArgs raisedArgs = await connectionFaulted.Task.WaitAsync(s_testTimeout, TestContext.Current.CancellationToken);

            Assert.Equal(ErrorRetryability.Terminal, raisedArgs.Exception.Retryability);
            Assert.Equal(2, dpsConnectAttempts);
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
