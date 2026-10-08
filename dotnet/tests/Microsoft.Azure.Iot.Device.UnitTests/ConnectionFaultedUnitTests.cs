// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Exceptions;
using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Mqtt;
using Microsoft.Azure.Iot.Device.Provisioning.Models;
using Microsoft.Azure.Iot.Device.Retry;
using System.Net.Sockets;
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
                cancellationToken: TestContext.Current.CancellationToken);

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
        public async Task ConnectToHubAsyncDoesNotProvisionWhenAReprovisionIsStanding()
        {
            using MockConnectionMqttClient mockMqttClient = new();
            MockDeviceProvisioningService mockDps = new(mockMqttClient, _ => FirstAssignedHub);

            mockMqttClient.OnConnect = connect =>
            {
                if (connect.HostName == FirstAssignedHub)
                {
                    return Task.FromResult(new MqttConnectAck() { ResultCode = MqttConnectReasonCode.NotAuthorized });
                }

                return Task.FromResult(new MqttConnectAck() { ResultCode = MqttConnectReasonCode.Success });
            };

            using TestConnectionClient connectionClient = new(new() { MqttClient = mockMqttClient });

            await Assert.ThrowsAsync<DeviceException>(
                async () => await connectionClient.ProvisionAndConnectAsync(
                    CreateProvisioningSettings(),
                    CreateAuthenticationProvider(),
                    cancellationToken: TestContext.Current.CancellationToken));

            Assert.Equal(1, mockDps.RegistrationCount);

            // A direct hub connection does not provision: it surfaces the failure to the caller and leaves the
            // registration count untouched, even though the failed provisioning above left a re-provision standing.
            await Assert.ThrowsAsync<DeviceException>(
                async () => await connectionClient.ConnectToHubAsync(
                    connectionClient.GetCurrentConnectionContext()!,
                    TestContext.Current.CancellationToken));

            Assert.Equal(1, mockDps.RegistrationCount);
        }

        [Fact]
        public async Task ConnectToHubAsyncThrowsWhenRetryingIsAbandoned()
        {
            using MockConnectionMqttClient mockMqttClient = new();
            MockDeviceProvisioningService mockDps = new(mockMqttClient, _ => FirstAssignedHub);

            mockMqttClient.OnConnect = _ => throw new IOException("Mock connection failure.");

            using TestConnectionClient connectionClient = new(new()
            {
                MqttClient = mockMqttClient,
                ConnectionRetryPolicy = new AbandonRetryPolicy(),
            });

            await Assert.ThrowsAsync<DeviceException>(
                async () => await connectionClient.ConnectToHubAsync(
                    new ConnectionContext()
                    {
                        AuthenticationProvider = CreateAuthenticationProvider(),
                        DeviceId = DeviceId,
                        IotHubHostName = FirstAssignedHub,
                        ConnectionProfile = ConnectionProfile.Classic,
                    },
                    TestContext.Current.CancellationToken));

            Assert.Equal(0, mockDps.RegistrationCount);
        }

        [Fact]
        public async Task ConnectToHubAsyncDoesNotProvisionWhenTheCachedHubRejectsTheDevice()
        {
            using MockConnectionMqttClient mockMqttClient = new();
            MockDeviceProvisioningService mockDps = new(mockMqttClient, registrationCount => registrationCount == 1 ? FirstAssignedHub : SecondAssignedHub);

            int firstHubConnects = 0;
            mockMqttClient.OnConnect = connect =>
            {
                // The first assigned hub accepts this device's initial connection, but once this device is removed from
                // that hub it rejects the identity on every later connect. The second assigned hub accepts the device.
                if (connect.HostName == FirstAssignedHub)
                {
                    return Interlocked.Increment(ref firstHubConnects) == 1
                        ? Task.FromResult(new MqttConnectAck() { ResultCode = MqttConnectReasonCode.Success })
                        : Task.FromResult(new MqttConnectAck() { ResultCode = MqttConnectReasonCode.NotAuthorized });
                }

                return Task.FromResult(new MqttConnectAck() { ResultCode = MqttConnectReasonCode.Success });
            };

            using TestConnectionClient connectionClient = new(new() { MqttClient = mockMqttClient });

            // The device provisions and connects to the first assigned hub.
            ConnectionContext connectionContext = await connectionClient.ProvisionAndConnectAsync(
                CreateProvisioningSettings(),
                CreateAuthenticationProvider(),
                cancellationToken: TestContext.Current.CancellationToken);

            Assert.Equal(FirstAssignedHub, connectionContext.IotHubHostName);
            Assert.Equal(1, mockDps.RegistrationCount);

            // The application disconnects the device deliberately and then connects it again by hand -- without
            // provisioning -- holding the connection context that provisioning returned.
            await connectionClient.DisconnectAsync(TestContext.Current.CancellationToken);

            await Assert.ThrowsAsync<DeviceException>(
                async () => await connectionClient.ConnectToHubAsync(connectionContext, TestContext.Current.CancellationToken));

            Assert.Equal(1, mockDps.RegistrationCount);
            Assert.Equal(ConnectionEndpoint.IotHub, connectionClient.CurrentEndpoint);
            Assert.Equal(FirstAssignedHub, connectionClient.GetCurrentConnectionContext()!.IotHubHostName);
        }

        [Fact]
        public async Task ReprovisioningRetriesAfterAFailedAttemptUntilItSucceeds()
        {
            using MockConnectionMqttClient mockMqttClient = new();
            MockDeviceProvisioningService mockDps = new(mockMqttClient, registrationCount => registrationCount == 1 ? FirstAssignedHub : SecondAssignedHub);

            int dpsConnectAttempts = 0;
            TaskCompletionSource connectedToSecondHub = new(TaskCreationOptions.RunContinuationsAsynchronously);
            mockMqttClient.OnConnect = connect =>
            {
                if (connect.HostName == GlobalDeviceEndpoint)
                {
                    // Reject the first re-provisioning attempt's connection to DPS with an identity fault so that
                    // recovery only completes if that failed attempt is retried rather than abandoned. The first DPS
                    // connect is the initial provisioning and must succeed; the second is the first re-provisioning
                    // attempt, which fails; later attempts succeed.
                    if (Interlocked.Increment(ref dpsConnectAttempts) == 2)
                    {
                        return Task.FromResult(new MqttConnectAck() { ResultCode = MqttConnectReasonCode.NotAuthorized });
                    }
                }
                else if (connect.HostName == SecondAssignedHub)
                {
                    connectedToSecondHub.TrySetResult();
                }

                return Task.FromResult(new MqttConnectAck() { ResultCode = MqttConnectReasonCode.Success });
            };

            using TestConnectionClient connectionClient = new(new()
            {
                MqttClient = mockMqttClient,
                ConnectionRetryPolicy = new ImmediateRetryPolicy(),
            });

            ConnectionContext connectionContext = await connectionClient.ProvisionAndConnectAsync(
                CreateProvisioningSettings(),
                CreateAuthenticationProvider(),
                cancellationToken: TestContext.Current.CancellationToken);

            Assert.Equal(FirstAssignedHub, connectionContext.IotHubHostName);
            Assert.Equal(1, mockDps.RegistrationCount);

            // The hub rejects this device's identity, so this client re-provisions. Its first re-provisioning attempt
            // fails, so recovery depends on that attempt being retried rather than the client giving up.
            await mockMqttClient.SimulateServerDisconnectAsync(MqttDisconnectReason.NotAuthorized);

            // This client should recover on its own by registering with DPS again -- retrying past the failed attempt --
            // and connecting to the hub it is assigned.
            await connectedToSecondHub.Task.WaitAsync(s_testTimeout, TestContext.Current.CancellationToken);

            Assert.Equal(ConnectionEndpoint.IotHub, connectionClient.CurrentEndpoint);
            Assert.Equal(SecondAssignedHub, connectionClient.GetCurrentConnectionContext()!.IotHubHostName);

            // The initial provisioning, the re-provisioning attempt that failed, and the one that finally succeeded.
            Assert.Equal(3, Volatile.Read(ref dpsConnectAttempts));

            // Only the initial provisioning and the successful re-provisioning actually registered; the failed attempt
            // never got past the refused CONNACK.
            Assert.Equal(2, mockDps.RegistrationCount);
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
                cancellationToken: TestContext.Current.CancellationToken);

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
                cancellationToken: TestContext.Current.CancellationToken);

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
        public async Task UnrecoverableFaultReleasesAPendingOperationAndRaisesConnectionFaultedAsync()
        {
            using MockConnectionMqttClient mockMqttClient = new();
            MockDeviceProvisioningService mockDps = new(mockMqttClient, _ => FirstAssignedHub);

            // A feature client's publish finds the connection gone, which parks it inside the client waiting for the
            // connection to come back -- this is the internal waiter that the unrecoverable fault must release.
            Func<MqttPublish, Task<MqttPublishAck>> handleProvisioningPublishAsync = mockMqttClient.OnPublish!;
            TaskCompletionSource publishAttempted = new(TaskCreationOptions.RunContinuationsAsynchronously);
            mockMqttClient.OnPublish = publish =>
            {
                if (publish.Topic.StartsWith(ProvisioningTopicPrefix))
                {
                    return handleProvisioningPublishAsync.Invoke(publish);
                }

                publishAttempted.TrySetResult();
                throw new MqttClientNotConnectedException("mock client not connected exception");
            };

            using TestConnectionClient connectionClient = new(new() { MqttClient = mockMqttClient });

            await connectionClient.ProvisionAndConnectAsync(
                CreateProvisioningSettings(),
                CreateAuthenticationProvider(),
                cancellationToken: TestContext.Current.CancellationToken);

            TaskCompletionSource<ConnectionFaultedEventArgs> connectionFaulted = new(TaskCreationOptions.RunContinuationsAsynchronously);
            connectionClient.ConnectionFaultedAsync += args =>
            {
                connectionFaulted.TrySetResult(args);
                return Task.CompletedTask;
            };

            // A feature client publishes while the connection is gone, so this operation is left waiting for the client
            // to make the device present again. It runs on its own thread because that wait blocks the thread it is on.
            Task pendingPublish = Task.Run(
                () => connectionClient.PublishAsync(
                    new MqttPublish() { Topic = $"devices/{DeviceId}/messages/events/" },
                    TestContext.Current.CancellationToken),
                TestContext.Current.CancellationToken);

            await publishAttempted.Task.WaitAsync(s_testTimeout, TestContext.Current.CancellationToken);

            // This reason is terminal and says nothing about this device's identity, so nothing on this client will
            // bring the connection back. The single fault must both release the parked publish (an internal waiter) and
            // tell the application the connection is gone for good -- neither may suppress the other.
            await mockMqttClient.SimulateServerDisconnectAsync(MqttDisconnectReason.ServerMoved);

            await Assert.ThrowsAsync<OperationCanceledException>(() => pendingPublish);

            ConnectionFaultedEventArgs raisedArgs = await connectionFaulted.Task.WaitAsync(s_testTimeout, TestContext.Current.CancellationToken);
            Assert.Equal(ErrorRetryability.Terminal, raisedArgs.Exception.Retryability);
        }

        [Fact]
        public async Task ProvisioningThatDoesNotAssignAHubThrowsARetryableFault()
        {
            using MockConnectionMqttClient mockMqttClient = new();
            MockDeviceProvisioningService mockDps = new(mockMqttClient, _ => FirstAssignedHub)
            {
                // DPS completes the registration with a terminal "failed" status and no hub assignment, as it does
                // when the enrollment does not exist yet. There is no hub for this device to connect to.
                TerminalRegistrationResult = new DeviceRegistrationResult()
                {
                    RegistrationId = RegistrationId,
                    Status = ProvisioningRegistrationStatus.Failed,
                    ErrorCode = 401002,
                    ErrorMessage = "Invalid certificate.",
                },
            };

            using TestConnectionClient connectionClient = new(new() { MqttClient = mockMqttClient });

            // The device was not assigned to a hub, so provisioning must surface as a classified, retryable fault
            // rather than a NullReferenceException from dereferencing the absent assignment. Mirrors the C connection
            // client, which treats a registration that failed or produced no assignment as a transient failure to be
            // retried under the reconnection policy.
            DeviceException exception = await Assert.ThrowsAsync<DeviceException>(
                async () => await connectionClient.ProvisionAndConnectAsync(
                    CreateProvisioningSettings(),
                    CreateAuthenticationProvider(),
                    cancellationToken: TestContext.Current.CancellationToken));

            Assert.Equal(ErrorRetryability.Retryable, exception.Retryability);
            Assert.Equal(1, mockDps.RegistrationCount);
        }

        [Fact]
        public async Task ProvisioningThatAssignsWithoutAHubThrowsARetryableFault()
        {
            using MockConnectionMqttClient mockMqttClient = new();
            MockDeviceProvisioningService mockDps = new(mockMqttClient, _ => FirstAssignedHub)
            {
                // DPS reports the registration as "assigned" but omits the hub hostname, so there is nothing for this
                // device to connect to even though the status looks successful.
                TerminalRegistrationResult = new DeviceRegistrationResult()
                {
                    RegistrationId = RegistrationId,
                    DeviceId = DeviceId,
                    AssignedHub = null,
                    Status = ProvisioningRegistrationStatus.Assigned,
                },
            };

            using TestConnectionClient connectionClient = new(new() { MqttClient = mockMqttClient });

            // An "assigned" result with no hub is unusable, so provisioning must refuse to adopt it and surface a
            // classified, retryable fault rather than connecting with a null hostname. Mirrors the C connection
            // client's reject_assignment, which re-provisions instead of using an assignment that lacks a hub.
            DeviceException exception = await Assert.ThrowsAsync<DeviceException>(
                async () => await connectionClient.ProvisionAndConnectAsync(
                    CreateProvisioningSettings(),
                    CreateAuthenticationProvider(),
                    cancellationToken: TestContext.Current.CancellationToken));

            Assert.Equal(ErrorRetryability.Retryable, exception.Retryability);
            Assert.Equal(1, mockDps.RegistrationCount);
        }

        [Fact]
        public async Task ProvisioningThatAssignsWithoutADeviceIdThrowsARetryableFault()
        {
            using MockConnectionMqttClient mockMqttClient = new();
            MockDeviceProvisioningService mockDps = new(mockMqttClient, _ => FirstAssignedHub)
            {
                // DPS reports the registration as "assigned" with a hub but no device id, so this device still cannot
                // build a usable identity to connect with.
                TerminalRegistrationResult = new DeviceRegistrationResult()
                {
                    RegistrationId = RegistrationId,
                    DeviceId = null,
                    AssignedHub = FirstAssignedHub,
                    Status = ProvisioningRegistrationStatus.Assigned,
                },
            };

            using TestConnectionClient connectionClient = new(new() { MqttClient = mockMqttClient });

            // An "assigned" result with no device id is unusable, so provisioning must refuse to adopt it and surface a
            // classified, retryable fault rather than connecting with a null device id. Mirrors the C connection
            // client's reject_assignment.
            DeviceException exception = await Assert.ThrowsAsync<DeviceException>(
                async () => await connectionClient.ProvisionAndConnectAsync(
                    CreateProvisioningSettings(),
                    CreateAuthenticationProvider(),
                    cancellationToken: TestContext.Current.CancellationToken));

            Assert.Equal(ErrorRetryability.Retryable, exception.Retryability);
            Assert.Equal(1, mockDps.RegistrationCount);
        }

        [Fact]
        public async Task ProvisioningThatAssignsANonClassicConnectionProfileThrowsARetryableFault()
        {
            using MockConnectionMqttClient mockMqttClient = new();
            MockDeviceProvisioningService mockDps = new(mockMqttClient, _ => FirstAssignedHub)
            {
                // DPS reports the registration as "assigned" with a hub and device id, but names a connection profile
                // other than "classic", which this SDK does not support.
                TerminalRegistrationResult = new DeviceRegistrationResult()
                {
                    RegistrationId = RegistrationId,
                    DeviceId = DeviceId,
                    AssignedHub = FirstAssignedHub,
                    ConnectionProfile = ConnectionProfile.MqttV5,
                    Status = ProvisioningRegistrationStatus.Assigned,
                },
            };

            using TestConnectionClient connectionClient = new(new() { MqttClient = mockMqttClient });

            // An "assigned" result that names an unsupported (non-"classic") connection profile is unusable, so
            // provisioning must refuse to adopt it and surface a classified, retryable fault rather than connecting
            // with a profile this SDK cannot speak. Mirrors the C connection client's reject_assignment, which
            // re-provisions instead of using an assignment with an unsupported connection profile.
            DeviceException exception = await Assert.ThrowsAsync<DeviceException>(
                async () => await connectionClient.ProvisionAndConnectAsync(
                    CreateProvisioningSettings(),
                    CreateAuthenticationProvider(),
                    cancellationToken: TestContext.Current.CancellationToken));

            Assert.Equal(ErrorRetryability.Retryable, exception.Retryability);
            Assert.Equal(1, mockDps.RegistrationCount);
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
                cancellationToken: TestContext.Current.CancellationToken);

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

            // Connect directly to a hub without provisioning, so this client holds no provisioning inputs it could
            // renew on its own.
            await connectionClient.ConnectToHubAsync(
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
                    cancellationToken: TestContext.Current.CancellationToken));

            Assert.Equal(ErrorRetryability.IdentityTerminal, exception.Retryability);

            await Task.Delay(s_negativeTestTimeout, TestContext.Current.CancellationToken);

            // The caller was told about the fault and decides what to do, so this client must not also be recovering from it.
            Assert.Equal(1, mockDps.RegistrationCount);
        }

        [Fact]
        public Task PendingPublishIsSentAfterReprovisioningRetriesPastATerminalFault()
        {
            return AssertPendingOperationIsSentAfterReprovisioningRetriesPastATerminalFaultAsync(
                (mockMqttClient, operationAttempted) =>
                {
                    // The registration flow's publishes must keep working. A feature client's first publish finds the
                    // connection gone, which is what makes it wait for this client to re-establish the connection, and
                    // the publish after that is sent normally once it has.
                    Func<MqttPublish, Task<MqttPublishAck>> handleProvisioningPublishAsync = mockMqttClient.OnPublish!;
                    int featurePublishAttempts = 0;
                    mockMqttClient.OnPublish = publish =>
                    {
                        if (publish.Topic.StartsWith(ProvisioningTopicPrefix))
                        {
                            return handleProvisioningPublishAsync.Invoke(publish);
                        }

                        if (Interlocked.Increment(ref featurePublishAttempts) == 1)
                        {
                            operationAttempted.TrySetResult();
                            throw new MqttClientNotConnectedException("mock client not connected exception");
                        }

                        return Task.FromResult(new MqttPublishAck() { ReasonCode = MqttPublishAckReasonCode.Success });
                    };
                },
                (connectionClient, cancellationToken) => connectionClient.PublishAsync(
                    new MqttPublish() { Topic = $"devices/{DeviceId}/messages/events/" },
                    cancellationToken));
        }

        [Fact]
        public Task PendingSubscribeIsSentAfterReprovisioningRetriesPastATerminalFault()
        {
            return AssertPendingOperationIsSentAfterReprovisioningRetriesPastATerminalFaultAsync(
                (mockMqttClient, operationAttempted) =>
                {
                    // The registration flow's subscribes must keep working. A feature client's first subscribe finds
                    // the connection gone, which is what makes it wait for this client to re-establish the connection,
                    // and the subscribe after that is sent normally once it has.
                    int featureSubscribeAttempts = 0;
                    mockMqttClient.OnSubscribe = subscribe =>
                    {
                        if (subscribe.TopicFilters.Any(topicFilter => topicFilter.Topic.StartsWith(ProvisioningTopicPrefix)))
                        {
                            return Task.FromResult(MqttObjectHelpers.CreateSuccessfulSuback(subscribe));
                        }

                        if (Interlocked.Increment(ref featureSubscribeAttempts) == 1)
                        {
                            operationAttempted.TrySetResult();
                            throw new MqttClientNotConnectedException("mock client not connected exception");
                        }

                        return Task.FromResult(MqttObjectHelpers.CreateSuccessfulSuback(subscribe));
                    };
                },
                (connectionClient, cancellationToken) => connectionClient.SubscribeAsync(
                    new MqttSubscribe($"devices/{DeviceId}/messages/devicebound/#", MqttQualityOfServiceLevel.AtLeastOnce),
                    cancellationToken));
        }

        [Fact]
        public Task PendingUnsubscribeIsSentAfterReprovisioningRetriesPastATerminalFault()
        {
            return AssertPendingOperationIsSentAfterReprovisioningRetriesPastATerminalFaultAsync(
                (mockMqttClient, operationAttempted) =>
                {
                    // The first unsubscribe finds the connection gone, which is what makes it wait for this client to
                    // re-establish the connection, and the unsubscribe after that is sent normally once it has.
                    int featureUnsubscribeAttempts = 0;
                    mockMqttClient.OnUnsubscribe = unsubscribe =>
                    {
                        if (Interlocked.Increment(ref featureUnsubscribeAttempts) == 1)
                        {
                            operationAttempted.TrySetResult();
                            throw new MqttClientNotConnectedException("mock client not connected exception");
                        }

                        return Task.FromResult(MqttObjectHelpers.CreateSuccessfulUnsuback(unsubscribe));
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
                cancellationToken: TestContext.Current.CancellationToken);

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
        /// A feature client operation issued before this client has ever connected -- so nothing is maintaining or
        /// recovering a connection -- must fail fast with the underlying not-connected error rather than block forever
        /// waiting for a reconnection that is never coming.
        /// </summary>
        [Fact]
        public async Task FeatureOperationBeforeConnectingFailsFastInsteadOfHanging()
        {
            using MockConnectionMqttClient mockMqttClient = new();

            // Nothing has connected this client, so the underlying client is not connected and a publish fails this way.
            mockMqttClient.OnPublish = _ => throw new MqttClientNotConnectedException("mock client not connected exception");

            using TestConnectionClient connectionClient = new(new() { MqttClient = mockMqttClient });

            // Run the publish on its own thread: if the fail-fast were missing, the call would block that thread waiting
            // for a reconnection, and the WaitAsync below would time out (failing this test) rather than hang the runner.
            var publish = new MqttPublish() { Topic = $"devices/{DeviceId}/messages/events/" };
            Task<MqttPublishAck> publishTask = Task.Run(
                () => connectionClient.PublishAsync(publish, TestContext.Current.CancellationToken),
                TestContext.Current.CancellationToken);

            await Assert.ThrowsAsync<MqttClientNotConnectedException>(
                async () => await publishTask.WaitAsync(s_negativeTestTimeout, TestContext.Current.CancellationToken));
        }

        /// <summary>
        /// Provision and connect a device, start an operation that finds the connection gone and waits for it to come
        /// back, then fault that connection on this device's identity and refuse the first re-provisioning attempt for a
        /// terminal reason. Re-provisioning is persistent (matching the C client's needs_reprovision loop), so it must
        /// retry past that terminal fault rather than give up, and the waiting operation is sent once a later attempt
        /// re-establishes the connection.
        /// </summary>
        /// <param name="failOperationWhileDisconnectedThenSucceed">
        /// Makes the mock report the operation under test as attempted (and failed) while this client is not connected,
        /// then let it succeed after the connection is re-established. It is given the source to signal once that first
        /// attempt has been made.
        /// </param>
        /// <param name="startPendingOperation">Starts the operation under test on the connection client.</param>
        private static async Task AssertPendingOperationIsSentAfterReprovisioningRetriesPastATerminalFaultAsync(
            Action<MockConnectionMqttClient, TaskCompletionSource> failOperationWhileDisconnectedThenSucceed,
            Func<TestConnectionClient, CancellationToken, Task> startPendingOperation)
        {
            using MockConnectionMqttClient mockMqttClient = new();
            MockDeviceProvisioningService mockDps = new(mockMqttClient, registrationCount => registrationCount == 1 ? FirstAssignedHub : SecondAssignedHub);

            int dpsConnectAttempts = 0;
            mockMqttClient.OnConnect = connect =>
            {
                if (connect.HostName != GlobalDeviceEndpoint)
                {
                    return Task.FromResult(new MqttConnectAck() { ResultCode = MqttConnectReasonCode.Success });
                }

                // The first registration succeeds so that this device gets connected to a hub. The re-provisioning that
                // the identity fault starts is refused once for a terminal reason, which must be retried rather than
                // given up on, and the attempt after that succeeds and assigns the device to a new hub.
                int attempt = Interlocked.Increment(ref dpsConnectAttempts);
                return Task.FromResult(new MqttConnectAck()
                {
                    ResultCode = attempt == 2 ? MqttConnectReasonCode.Banned : MqttConnectReasonCode.Success,
                });
            };

            TaskCompletionSource operationAttempted = new(TaskCreationOptions.RunContinuationsAsynchronously);
            failOperationWhileDisconnectedThenSucceed(mockMqttClient, operationAttempted);

            using TestConnectionClient connectionClient = new(new()
            {
                MqttClient = mockMqttClient,
                ConnectionRetryPolicy = new ImmediateRetryPolicy(),
            });

            await connectionClient.ProvisionAndConnectAsync(
                CreateProvisioningSettings(),
                CreateAuthenticationProvider(),
                cancellationToken: TestContext.Current.CancellationToken);

            // A feature client, such as the telemetry client, sends this while the connection is gone, so the operation
            // is left waiting for this client to make the device present again. It runs on its own thread because that
            // wait blocks whichever thread the operation was started on.
            Task pendingOperation = Task.Run(
                () => startPendingOperation(connectionClient, TestContext.Current.CancellationToken),
                TestContext.Current.CancellationToken);

            await operationAttempted.Task.WaitAsync(s_testTimeout, TestContext.Current.CancellationToken);

            // The hub rejects this device's identity, so this client starts re-provisioning while that operation waits.
            await mockMqttClient.SimulateServerDisconnectAsync(MqttDisconnectReason.NotAuthorized);

            // The first re-provisioning attempt hit a terminal fault, but re-provisioning is persistent, so a later
            // attempt re-established the connection and the waiting operation was sent rather than canceled.
            await pendingOperation.WaitAsync(s_testTimeout, TestContext.Current.CancellationToken);

            Assert.Equal(3, Volatile.Read(ref dpsConnectAttempts));
            Assert.Equal(2, mockDps.RegistrationCount);
            Assert.Equal(SecondAssignedHub, connectionClient.GetCurrentConnectionContext()!.IotHubHostName);
        }

        [Fact]
        public async Task ReprovisioningRetriesPastATerminalFaultWithoutRaisingConnectionFaulted()
        {
            using MockConnectionMqttClient mockMqttClient = new();
            MockDeviceProvisioningService mockDps = new(mockMqttClient, registrationCount => registrationCount == 1 ? FirstAssignedHub : SecondAssignedHub);

            int dpsConnectAttempts = 0;
            TaskCompletionSource connectedToSecondHub = new(TaskCreationOptions.RunContinuationsAsynchronously);
            mockMqttClient.OnConnect = connect =>
            {
                if (connect.HostName == SecondAssignedHub)
                {
                    connectedToSecondHub.TrySetResult();
                }

                if (connect.HostName != GlobalDeviceEndpoint)
                {
                    return Task.FromResult(new MqttConnectAck() { ResultCode = MqttConnectReasonCode.Success });
                }

                // The first registration succeeds so that this device gets connected to a hub. The re-provisioning that
                // the identity fault starts is refused once for a terminal reason, which must be retried rather than
                // surfaced to the application, and the attempt after that succeeds and assigns the device to a new hub.
                int attempt = Interlocked.Increment(ref dpsConnectAttempts);
                return Task.FromResult(new MqttConnectAck()
                {
                    ResultCode = attempt == 2 ? MqttConnectReasonCode.Banned : MqttConnectReasonCode.Success,
                });
            };

            using TestConnectionClient connectionClient = new(new()
            {
                MqttClient = mockMqttClient,
                ConnectionRetryPolicy = new ImmediateRetryPolicy(),
            });

            bool connectionFaultedRaised = false;
            connectionClient.ConnectionFaultedAsync += args =>
            {
                connectionFaultedRaised = true;
                return Task.CompletedTask;
            };

            await connectionClient.ProvisionAndConnectAsync(
                CreateProvisioningSettings(),
                CreateAuthenticationProvider(),
                cancellationToken: TestContext.Current.CancellationToken);

            // The hub rejects this device's identity, so this client starts re-provisioning. The first re-provisioning
            // attempt hits a terminal error of its own, but a terminal fault during re-provisioning must not be surfaced
            // to the application: re-provisioning keeps retrying until it recovers (just like the C client).
            await mockMqttClient.SimulateServerDisconnectAsync(MqttDisconnectReason.NotAuthorized);

            await connectedToSecondHub.Task.WaitAsync(s_testTimeout, TestContext.Current.CancellationToken);

            // Give any erroneous connection-faulted notification a chance to surface before asserting it never does.
            await Task.Delay(s_negativeTestTimeout, TestContext.Current.CancellationToken);

            Assert.False(connectionFaultedRaised, "A terminal fault during re-provisioning must not be surfaced while re-provisioning keeps retrying.");
            Assert.Equal(3, Volatile.Read(ref dpsConnectAttempts));
            Assert.Equal(2, mockDps.RegistrationCount);
            Assert.Equal(SecondAssignedHub, connectionClient.GetCurrentConnectionContext()!.IotHubHostName);
        }

        [Fact]
        public async Task ReprovisioningBackoffHonorsTheServiceRetryAfterAsAFloor()
        {
            using MockConnectionMqttClient mockMqttClient = new();
            MockDeviceProvisioningService mockDps = new(mockMqttClient, _ => FirstAssignedHub)
            {
                // The first registration succeeds so that this device gets connected to a hub. Every re-provisioning
                // attempt after it (registration 2 and on) completes with a terminal "failed" status, so recovery never
                // succeeds and the delay between consecutive attempts stays observable.
                TerminalRegistrationResultSelector = registrationCount => registrationCount == 1
                    ? null
                    : new DeviceRegistrationResult()
                    {
                        RegistrationId = RegistrationId,
                        Status = ProvisioningRegistrationStatus.Failed,
                    },

                // The service asks to be left alone for a second on each terminal response, which must floor the
                // re-provisioning backoff.
                TerminalRetryAfterSeconds = 1,
            };

            // Record when each re-provisioning registration (registration 2 and on) arrives so that the delay between
            // two consecutive attempts can be measured.
            List<DateTime> reprovisioningRegistrationTimes = new();
            TaskCompletionSource twoReprovisioningAttemptsSeen = new(TaskCreationOptions.RunContinuationsAsynchronously);
            mockDps.RegistrationRequested += registrationCount =>
            {
                if (registrationCount >= 2)
                {
                    lock (reprovisioningRegistrationTimes)
                    {
                        reprovisioningRegistrationTimes.Add(DateTime.UtcNow);
                        if (reprovisioningRegistrationTimes.Count == 2)
                        {
                            twoReprovisioningAttemptsSeen.TrySetResult();
                        }
                    }
                }

                return Task.CompletedTask;
            };

            // A policy that retries with no backoff of its own, so any delay between attempts can only come from the
            // service's Retry-After acting as a floor.
            using TestConnectionClient connectionClient = new(new()
            {
                MqttClient = mockMqttClient,
                ConnectionRetryPolicy = new ImmediateRetryPolicy(),
            });

            await connectionClient.ProvisionAndConnectAsync(
                CreateProvisioningSettings(),
                CreateAuthenticationProvider(),
                cancellationToken: TestContext.Current.CancellationToken);

            Assert.Equal(1, mockDps.RegistrationCount);

            // The hub rejects this device's identity, so this client starts re-provisioning. Every attempt fails, so the
            // loop keeps retrying; the service asked for a one-second Retry-After, which must floor the policy's zero
            // backoff -- mirroring the C client, where the service's retry-after wins when it is longer than the policy's.
            await mockMqttClient.SimulateServerDisconnectAsync(MqttDisconnectReason.NotAuthorized);

            await twoReprovisioningAttemptsSeen.Task.WaitAsync(s_testTimeout, TestContext.Current.CancellationToken);

            TimeSpan gapBetweenAttempts;
            lock (reprovisioningRegistrationTimes)
            {
                gapBetweenAttempts = reprovisioningRegistrationTimes[1] - reprovisioningRegistrationTimes[0];
            }

            // Without the floor these attempts would fire back-to-back under the zero-backoff policy; the service's
            // one-second Retry-After forces them at least that far apart (allowing for scheduling slack).
            Assert.True(
                gapBetweenAttempts >= TimeSpan.FromMilliseconds(800),
                $"Expected consecutive re-provisioning attempts to be at least ~1s apart because of the service's Retry-After, but they were {gapBetweenAttempts.TotalMilliseconds:F0}ms apart.");
        }

        [Fact]
        public async Task HubUnreachableForTheConfiguredAttemptsReprovisionsAndConnectsToNewlyAssignedHub()
        {
            const uint threshold = 3;

            using MockConnectionMqttClient mockMqttClient = new();
            MockDeviceProvisioningService mockDps = new(mockMqttClient, registrationCount => registrationCount == 1 ? FirstAssignedHub : SecondAssignedHub);

            TaskCompletionSource connectedToSecondHub = new(TaskCreationOptions.RunContinuationsAsynchronously);
            int firstHubConnects = 0;
            mockMqttClient.OnConnect = connect =>
            {
                if (connect.HostName == SecondAssignedHub)
                {
                    connectedToSecondHub.TrySetResult();
                    return Task.FromResult(new MqttConnectAck() { ResultCode = MqttConnectReasonCode.Success });
                }

                // The first hub accepts the initial connect so this device is connected, then stops answering -- it
                // was vacated service-side, so every reconnect fails with a retryable transport error rather than a
                // rejection of this device's identity. DPS keeps answering throughout.
                if (connect.HostName == FirstAssignedHub && Interlocked.Increment(ref firstHubConnects) > 1)
                {
                    throw new Exception("simulated unreachable hub");
                }

                return Task.FromResult(new MqttConnectAck() { ResultCode = MqttConnectReasonCode.Success });
            };

            using TestConnectionClient connectionClient = new(new()
            {
                MqttClient = mockMqttClient,
                ConnectionRetryPolicy = new ImmediateRetryPolicy(maxHubConnectAttemptsBeforeReprovision: threshold),
            });

            await connectionClient.ProvisionAndConnectAsync(
                CreateProvisioningSettings(),
                CreateAuthenticationProvider(),
                cancellationToken: TestContext.Current.CancellationToken);

            // The hub stops answering mid-session for a benign, retryable reason, so the connection layer reconnects
            // rather than faulting. Every reconnect fails, and once the configured number of attempts has been spent,
            // this client re-provisions instead of retrying the unreachable hub forever.
            await mockMqttClient.SimulateServerDisconnectAsync(MqttDisconnectReason.ServerBusy);

            await connectedToSecondHub.Task.WaitAsync(s_testTimeout, TestContext.Current.CancellationToken);

            Assert.Equal(2, mockDps.RegistrationCount);
            Assert.Equal(ConnectionEndpoint.IotHub, connectionClient.CurrentEndpoint);
            Assert.Equal(SecondAssignedHub, connectionClient.GetCurrentConnectionContext()!.IotHubHostName);
        }

        [Fact]
        public async Task HubUnreachableDoesNotReprovisionWhenTheThresholdIsDisabled()
        {
            using MockConnectionMqttClient mockMqttClient = new();
            MockDeviceProvisioningService mockDps = new(mockMqttClient, _ => FirstAssignedHub);

            int firstHubConnects = 0;
            mockMqttClient.OnConnect = connect =>
            {
                if (connect.HostName == FirstAssignedHub && Interlocked.Increment(ref firstHubConnects) > 1)
                {
                    throw new Exception("simulated unreachable hub");
                }

                return Task.FromResult(new MqttConnectAck() { ResultCode = MqttConnectReasonCode.Success });
            };

            using TestConnectionClient connectionClient = new(new()
            {
                MqttClient = mockMqttClient,
                // Zero (the ImmediateRetryPolicy default) disables the fallback, so the hub is retried indefinitely and
                // this device never re-provisions.
                ConnectionRetryPolicy = new ImmediateRetryPolicy(TimeSpan.FromMilliseconds(20)),
            });

            await connectionClient.ProvisionAndConnectAsync(
                CreateProvisioningSettings(),
                CreateAuthenticationProvider(),
                cancellationToken: TestContext.Current.CancellationToken);

            await mockMqttClient.SimulateServerDisconnectAsync(MqttDisconnectReason.ServerBusy);
            await Task.Delay(s_negativeTestTimeout, TestContext.Current.CancellationToken);

            // The hub has been retried many times over, but with the fallback disabled this device never crosses over
            // to Device Provisioning Service.
            Assert.Equal(1, mockDps.RegistrationCount);
            Assert.True(firstHubConnects > 1, "The connection layer should have kept retrying the unreachable hub.");
        }

        [Fact]
        public async Task HubReachableAgainBeforeTheThresholdDoesNotReprovision()
        {
            const uint threshold = 5;

            using MockConnectionMqttClient mockMqttClient = new();
            MockDeviceProvisioningService mockDps = new(mockMqttClient, _ => FirstAssignedHub);

            TaskCompletionSource reconnectedToFirstHub = new(TaskCreationOptions.RunContinuationsAsynchronously);
            int firstHubConnects = 0;
            mockMqttClient.OnConnect = connect =>
            {
                if (connect.HostName != FirstAssignedHub)
                {
                    return Task.FromResult(new MqttConnectAck() { ResultCode = MqttConnectReasonCode.Success });
                }

                // Initial connect succeeds (1), the next two reconnects fail, then the hub answers again (4). Two
                // failures is short of the threshold, so the counter must reset on that success rather than carry over.
                int attempt = Interlocked.Increment(ref firstHubConnects);
                if (attempt == 2 || attempt == 3)
                {
                    throw new Exception("temporarily unreachable hub");
                }

                if (attempt == 4)
                {
                    reconnectedToFirstHub.TrySetResult();
                }

                return Task.FromResult(new MqttConnectAck() { ResultCode = MqttConnectReasonCode.Success });
            };

            using TestConnectionClient connectionClient = new(new()
            {
                MqttClient = mockMqttClient,
                ConnectionRetryPolicy = new ImmediateRetryPolicy(maxHubConnectAttemptsBeforeReprovision: threshold),
            });

            await connectionClient.ProvisionAndConnectAsync(
                CreateProvisioningSettings(),
                CreateAuthenticationProvider(),
                cancellationToken: TestContext.Current.CancellationToken);

            await mockMqttClient.SimulateServerDisconnectAsync(MqttDisconnectReason.ServerBusy);

            await reconnectedToFirstHub.Task.WaitAsync(s_testTimeout, TestContext.Current.CancellationToken);
            await Task.Delay(s_negativeTestTimeout, TestContext.Current.CancellationToken);

            // The hub came back within the threshold, so this device reconnected to the same hub without re-provisioning.
            Assert.Equal(1, mockDps.RegistrationCount);
            Assert.Equal(ConnectionEndpoint.IotHub, connectionClient.CurrentEndpoint);
            Assert.Equal(FirstAssignedHub, connectionClient.GetCurrentConnectionContext()!.IotHubHostName);
        }

        [Fact]
        public async Task InitialConnectConsultsTheRetryPolicyOncePerAttemptWhenFailedAttemptsRaiseDisconnected()
        {
            const int failedAttempts = 3;

            using MockConnectionMqttClient mockMqttClient = new();
            FailFirstHubConnectAttempts(mockMqttClient, failedAttempts);

            RecordingRetryPolicy retryPolicy = new(TimeSpan.FromMilliseconds(100));
            using TestConnectionClient connectionClient = new(new()
            {
                MqttClient = mockMqttClient,
                ConnectionRetryPolicy = retryPolicy,
            });

            await connectionClient.ConnectToHubAsync(CreateHubConnectionContext(), TestContext.Current.CancellationToken)
                .WaitAsync(s_testTimeout, TestContext.Current.CancellationToken);

            // Give any competing reconnection loop started by a "Disconnected" callback time to show itself.
            await Task.Delay(s_negativeTestTimeout, TestContext.Current.CancellationToken);

            // Exactly one retry loop drove the initial connect: it consulted the policy once for each retry and sent one
            // CONNECT per attempt.
            Assert.Equal(new uint[] { 2, 3, 4 }, retryPolicy.GetConsultedAttempts());
            Assert.Equal(failedAttempts + 1, mockMqttClient.ConnectAttemptCount);
            Assert.True(mockMqttClient.IsConnected());
        }

        [Fact]
        public async Task InitialConnectAbandonedByTheRetryPolicyDoesNotLeaveASecondRetryLoopRunning()
        {
            using MockConnectionMqttClient mockMqttClient = new();
            FailFirstHubConnectAttempts(mockMqttClient, int.MaxValue);

            RecordingRetryPolicy retryPolicy = new(TimeSpan.FromMilliseconds(100), maxRetries: 3);
            using TestConnectionClient connectionClient = new(new()
            {
                MqttClient = mockMqttClient,
                ConnectionRetryPolicy = retryPolicy,
            });

            await Assert.ThrowsAsync<DeviceException>(
                async () => await connectionClient.ConnectToHubAsync(CreateHubConnectionContext(), TestContext.Current.CancellationToken)
                    .WaitAsync(s_testTimeout, TestContext.Current.CancellationToken));

            int connectAttemptsWhenAbandoned = mockMqttClient.ConnectAttemptCount;
            await Task.Delay(s_negativeTestTimeout, TestContext.Current.CancellationToken);

            // The policy was asked about attempts 2, 3 and 4 once each, abandoning at 4, and nothing kept connecting
            // after the initial connect gave up.
            Assert.Equal(new uint[] { 2, 3, 4 }, retryPolicy.GetConsultedAttempts());
            Assert.Equal(3, connectAttemptsWhenAbandoned);
            Assert.Equal(connectAttemptsWhenAbandoned, mockMqttClient.ConnectAttemptCount);
        }

        [Fact]
        public async Task ProvisionAndConnectUsesSeededConnectionContextAndSkipsProvisioning()
        {
            using MockConnectionMqttClient mockMqttClient = new();
            MockDeviceProvisioningService mockDps = new(mockMqttClient, _ => SecondAssignedHub);

            mockMqttClient.OnConnect = _ => Task.FromResult(new MqttConnectAck() { ResultCode = MqttConnectReasonCode.Success });

            ConnectionContext seededConnectionContext = new()
            {
                AuthenticationProvider = CreateAuthenticationProvider(),
                DeviceId = DeviceId,
                IotHubHostName = FirstAssignedHub,
                ConnectionProfile = ConnectionProfile.Classic,
            };

            using TestConnectionClient connectionClient = new(
                new() { MqttClient = mockMqttClient },
                seededConnectionContext);

            ConnectionContext connectionContext = await connectionClient.ProvisionAndConnectAsync(
                CreateProvisioningSettings(),
                CreateAuthenticationProvider(),
                cancellationToken: TestContext.Current.CancellationToken);

            // The seeded assignment connected, so this device connected directly without provisioning.
            Assert.Same(seededConnectionContext, connectionContext);
            Assert.Equal(FirstAssignedHub, connectionContext.IotHubHostName);
            Assert.Equal(0, mockDps.RegistrationCount);
            Assert.Equal(ConnectionEndpoint.IotHub, connectionClient.CurrentEndpoint);
        }

        [Fact]
        public async Task ProvisionAndConnectFallsBackToProvisioningWhenSeededContextCannotConnect()
        {
            using MockConnectionMqttClient mockMqttClient = new();
            MockDeviceProvisioningService mockDps = new(mockMqttClient, _ => SecondAssignedHub);

            // The seeded assignment rejects this device's identity; Device Provisioning Service and the newly assigned
            // hub accept it.
            mockMqttClient.OnConnect = connect => Task.FromResult(new MqttConnectAck()
            {
                ResultCode = connect.HostName == FirstAssignedHub
                    ? MqttConnectReasonCode.NotAuthorized
                    : MqttConnectReasonCode.Success,
            });

            ConnectionContext seededConnectionContext = new()
            {
                AuthenticationProvider = CreateAuthenticationProvider(),
                DeviceId = DeviceId,
                IotHubHostName = FirstAssignedHub,
                ConnectionProfile = ConnectionProfile.Classic,
            };

            using TestConnectionClient connectionClient = new(
                new() { MqttClient = mockMqttClient },
                seededConnectionContext);

            ConnectionContext connectionContext = await connectionClient.ProvisionAndConnectAsync(
                CreateProvisioningSettings(),
                CreateAuthenticationProvider(),
                cancellationToken: TestContext.Current.CancellationToken);

            // The seeded assignment could not connect, so this device provisioned for a fresh one and connected to it.
            Assert.Equal(1, mockDps.RegistrationCount);
            Assert.Equal(SecondAssignedHub, connectionContext.IotHubHostName);
            Assert.Equal(ConnectionEndpoint.IotHub, connectionClient.CurrentEndpoint);
        }

        [Fact]
        public async Task ProvisionAndConnectWithForceProvisioningSkipsSeededContext()
        {
            using MockConnectionMqttClient mockMqttClient = new();
            MockDeviceProvisioningService mockDps = new(mockMqttClient, _ => SecondAssignedHub);

            // Every hub, including the seeded assignment, would accept this device; forcing provisioning must still skip
            // the seeded assignment and go straight to Device Provisioning Service.
            mockMqttClient.OnConnect = _ => Task.FromResult(new MqttConnectAck() { ResultCode = MqttConnectReasonCode.Success });

            ConnectionContext seededConnectionContext = new()
            {
                AuthenticationProvider = CreateAuthenticationProvider(),
                DeviceId = DeviceId,
                IotHubHostName = FirstAssignedHub,
                ConnectionProfile = ConnectionProfile.Classic,
            };

            using TestConnectionClient connectionClient = new(
                new() { MqttClient = mockMqttClient },
                seededConnectionContext);

            ConnectionContext connectionContext = await connectionClient.ProvisionAndConnectAsync(
                CreateProvisioningSettings(),
                CreateAuthenticationProvider(),
                forceProvisioning: true,
                cancellationToken: TestContext.Current.CancellationToken);

            // Provisioning was forced, so this device provisioned and connected to the assigned hub rather than the
            // seeded one, even though the seeded assignment would have connected.
            Assert.Equal(1, mockDps.RegistrationCount);
            Assert.Equal(SecondAssignedHub, connectionContext.IotHubHostName);
            Assert.Equal(ConnectionEndpoint.IotHub, connectionClient.CurrentEndpoint);
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

        private static ConnectionContext CreateHubConnectionContext()
        {
            return new ConnectionContext()
            {
                AuthenticationProvider = CreateAuthenticationProvider(),
                DeviceId = DeviceId,
                IotHubHostName = FirstAssignedHub,
                ConnectionProfile = ConnectionProfile.Classic,
            };
        }

        /// <summary>
        /// Make the first <paramref name="failedAttempts"/> CONNECTs to <see cref="FirstAssignedHub"/> fail the way an
        /// unresolvable host does with MQTTnet: the connect throws a retryable network error, and the client also raises
        /// its "Disconnected" callback for that failed attempt from a background task.
        /// </summary>
        private static void FailFirstHubConnectAttempts(MockConnectionMqttClient mockMqttClient, int failedAttempts)
        {
            int hubConnects = 0;
            mockMqttClient.OnConnect = connect =>
            {
                if (connect.HostName == FirstAssignedHub && Interlocked.Increment(ref hubConnects) <= failedAttempts)
                {
                    _ = Task.Run(() => mockMqttClient.SimulateSpuriousDisconnectCallbackAsync(MqttDisconnectReason.UnspecifiedError));
                    throw new SocketException((int)SocketError.HostNotFound);
                }

                return Task.FromResult(new MqttConnectAck() { ResultCode = MqttConnectReasonCode.Success });
            };
        }

        /// <summary>
        /// A retry policy that records every attempt number it is consulted about, retries after a fixed delay, and
        /// optionally abandons retrying once more than <c>maxRetries</c> retries have been requested.
        /// </summary>
        private sealed class RecordingRetryPolicy : IRetryPolicy
        {
            private readonly TimeSpan _delay;
            private readonly uint _maxRetries;
            private readonly List<uint> _consultedAttempts = new();

            public RecordingRetryPolicy(TimeSpan delay, uint maxRetries = uint.MaxValue)
            {
                _delay = delay;
                _maxRetries = maxRetries;
            }

            public uint[] GetConsultedAttempts()
            {
                lock (_consultedAttempts)
                {
                    return _consultedAttempts.ToArray();
                }
            }

            public RetryGuidance GetRetryGuidance(uint currentRetryCount, Exception? lastException, ConnectionEndpoint connectionEndpoint, out TimeSpan retryDelay)
            {
                lock (_consultedAttempts)
                {
                    _consultedAttempts.Add(currentRetryCount);
                }

                retryDelay = _delay;
                return currentRetryCount > _maxRetries ? RetryGuidance.AbandonRetry : RetryGuidance.Retry;
            }
        }

        /// <summary>
        /// A retry policy that retries forever after a fixed (by default zero) delay, so that a test can drive many
        /// reconnection attempts quickly without waiting out the production exponential backoff.
        /// </summary>
        private sealed class ImmediateRetryPolicy : IRetryPolicy
        {
            private readonly TimeSpan _delay;
            private readonly uint _maxHubConnectAttemptsBeforeReprovision;

            public ImmediateRetryPolicy(TimeSpan? delay = null, uint maxHubConnectAttemptsBeforeReprovision = 0)
            {
                _delay = delay ?? TimeSpan.Zero;
                _maxHubConnectAttemptsBeforeReprovision = maxHubConnectAttemptsBeforeReprovision;
            }

            public ImmediateRetryPolicy(uint maxHubConnectAttemptsBeforeReprovision)
                : this(null, maxHubConnectAttemptsBeforeReprovision)
            {
            }

            public RetryGuidance GetRetryGuidance(uint currentRetryCount, Exception? lastException, ConnectionEndpoint connectionEndpoint, out TimeSpan retryDelay)
            {
                retryDelay = _delay;

                if (connectionEndpoint == ConnectionEndpoint.IotHub
                    && _maxHubConnectAttemptsBeforeReprovision > 0
                    && currentRetryCount > _maxHubConnectAttemptsBeforeReprovision)
                {
                    return RetryGuidance.Reprovision;
                }

                return RetryGuidance.Retry;
            }
        }

        /// <summary>
        /// A retry policy that abandons retrying on the very first failure, so that a test can drive a recovery loop to
        /// give up immediately rather than retrying forever under the default indefinite policy.
        /// </summary>
        private sealed class AbandonRetryPolicy : IRetryPolicy
        {
            public RetryGuidance GetRetryGuidance(uint currentRetryCount, Exception? lastException, ConnectionEndpoint connectionEndpoint, out TimeSpan retryDelay)
            {
                retryDelay = TimeSpan.Zero;
                return RetryGuidance.AbandonRetry;
            }
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
            /// When set, the registration poll completes with this terminal result (for example a "failed" status with
            /// no hub assignment) instead of assigning the device to a hub. Lets a test drive the non-"assigned"
            /// provisioning path.
            /// </summary>
            public DeviceRegistrationResult? TerminalRegistrationResult { get; set; }

            /// <summary>
            /// When set, chooses the terminal registration result for each registration (given the running registration
            /// count), overriding <see cref="TerminalRegistrationResult"/>. Returning null falls back to the default
            /// "assigned" result, so a test can let the first registration succeed and later ones fail.
            /// </summary>
            public Func<int, DeviceRegistrationResult?>? TerminalRegistrationResultSelector { get; set; }

            /// <summary>
            /// When set, the terminal status poll's response topic carries this many seconds as its Retry-After, letting
            /// a test drive the service's retry-after guidance.
            /// </summary>
            public int? TerminalRetryAfterSeconds { get; set; }

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
                    DeviceRegistrationResult registrationState =
                        TerminalRegistrationResultSelector?.Invoke(RegistrationCount)
                        ?? TerminalRegistrationResult
                        ?? new DeviceRegistrationResult()
                        {
                            RegistrationId = RegistrationId,
                            DeviceId = DeviceId,
                            AssignedHub = _assignedHubSelector(RegistrationCount),
                            Status = ProvisioningRegistrationStatus.Assigned,
                        };

                    await RespondAsync(
                        new RegistrationOperationStatus()
                        {
                            OperationId = OperationId,
                            Status = registrationState.Status,
                            RegistrationState = registrationState,
                        },
                        TerminalRetryAfterSeconds);
                }

                return new MqttPublishAck() { ReasonCode = MqttPublishAckReasonCode.Success };
            }

            private Task RespondAsync(RegistrationOperationStatus status, int? retryAfterSeconds = null)
            {
                string topic = retryAfterSeconds is { } seconds
                    ? $"$dps/registrations/res/200/?$rid=1&retry-after={seconds}"
                    : "$dps/registrations/res/200/?$rid=1";

                return _mqttClient.SimulatePublishReceivedAsync(new MqttPublish()
                {
                    Topic = topic,
                    Payload = JsonSerializer.SerializeToUtf8Bytes(status, JsonSerializationSettings.Options),
                });
            }
        }
    }
}
