using Microsoft.Azure.Devices.Client.Models;
using Microsoft.Azure.Devices.Client.Models.DirectMethods;
using Microsoft.Azure.Devices.Client.Models.Telemetry;
using Microsoft.Azure.Devices.Client.Models.Twin;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using System.Text.Json.Nodes;
using Xunit;
using Gen2ConnectionClient = Microsoft.Azure.Devices.Client.Gen2.Connection.ConnectionClient;
using Gen2DirectMethodProbeAck = Microsoft.Azure.Devices.Client.Gen2.DirectMethods.DirectMethodProbeAck;
using Gen2ReportedPatchRequest = Microsoft.Azure.Devices.Client.Gen2.Twin.ReportedPatchRequest;
using TwinResult = Microsoft.Azure.Devices.Client.Models.Twin.Result;
using Gen2DirectMethodClient = Microsoft.Azure.Devices.Client.Gen2.DirectMethods.DirectMethodClient;
using Gen2TelemetryClient = Microsoft.Azure.Devices.Client.Gen2.Telemetry.TelemetryClient;
using Gen2TwinClient = Microsoft.Azure.Devices.Client.Gen2.Twin.TwinClient;
using UnifiedConnectionClient = Microsoft.Azure.Devices.Client.Unified.Connection.ConnectionClient;
using UnifiedDirectMethodClient = Microsoft.Azure.Devices.Client.Unified.DirectMethods.DirectMethodClient;
using UnifiedTelemetryClient = Microsoft.Azure.Devices.Client.Unified.Telemetry.TelemetryClient;
using UnifiedTwinClient = Microsoft.Azure.Devices.Client.Unified.Twin.TwinClient;

namespace Microsoft.Azure.Devices.Client.IntegrationTests.StubService
{
    /// <summary>
    /// End to end tests that run a real device client against the <see cref="StubIotHubService"/> over an
    /// <see cref="InProcessMqttBroker"/>. No cloud resources are involved.
    /// </summary>
    /// <remarks>
    /// These tests double as executable documentation of how to use the stub, and as the verification that the stub
    /// actually speaks the same MQTT protocol that the device client expects from a real IoT hub.
    /// </remarks>
    public class StubIotHubServiceTests
    {
        private const string StubHostName = "stub.azure-devices.net";
        private const string IdScope = "0ne00000000";

        [Fact(Timeout = Setup.TestTimeoutMilliseconds)]
        public async Task Gen2_DeviceConnect_CompletesThePresenceHandshake()
        {
            await using var harness = await StubServiceTestHarness.StartAsync(IotHubGeneration.Gen2);
            await harness.ConnectGen2DeviceAsync();

            StubDeviceBirthEventArgs birth = await harness.Stub.WaitForDeviceBirthAsync(harness.DeviceId, TestContext.Current.CancellationToken);

            Assert.Equal(harness.DeviceId, birth.DeviceId);
            Assert.NotEqual(Guid.Empty, birth.ConnectionNonce);
            Assert.True(birth.PushDesired);
            Assert.True(birth.PushReported);
        }

        [Fact(Timeout = Setup.TestTimeoutMilliseconds)]
        public async Task Gen2_GetTwin_ReturnsTheStubsAuthoritativeState()
        {
            await using var harness = await StubServiceTestHarness.StartAsync(IotHubGeneration.Gen2);

            // Seed the stub's twin before the device connects so the response is deterministic.
            harness.Stub.GetDeviceState(harness.DeviceId).ReplaceDesiredProperties(new JsonObject { ["targetTemperature"] = 72 });

            Gen2ConnectionClient connection = await harness.ConnectGen2DeviceAsync();
            using var twinClient = new Gen2TwinClient(connection);

            DeviceTwin twin = await twinClient.GetTwinAsync(cancellationToken: TestContext.Current.CancellationToken);

            Assert.NotNull(twin.Desired);
            Assert.Equal(72, (int)twin.Desired["targetTemperature"]!);
            Assert.Equal(2ul, twin.DesiredVersion);
            Assert.NotNull(twin.Reported);
            Assert.Empty(twin.Reported);
            Assert.Equal(1ul, twin.ReportedVersion);
        }

        [Fact(Timeout = Setup.TestTimeoutMilliseconds)]
        public async Task Gen2_UpdateReportedProperties_IsAppliedByTheStub()
        {
            await using var harness = await StubServiceTestHarness.StartAsync(IotHubGeneration.Gen2);
            Gen2ConnectionClient connection = await harness.ConnectGen2DeviceAsync();
            using var twinClient = new Gen2TwinClient(connection);

            var patch = new JsonObject { ["firmwareVersion"] = "1.2.3" };
            ReportedPatchResponse response = await twinClient.UpdateReportedPropertiesAsync(
                new Gen2ReportedPatchRequest { ReportedProperties = patch, IfMatch = 1 },
                TestContext.Current.CancellationToken);

            Assert.Equal(TwinResult.Ok, response.Result);
            Assert.Equal(2ul, response.Version);

            StubTwinSnapshot twin = harness.Stub.GetDeviceState(harness.DeviceId).GetTwinSnapshot();
            Assert.Equal("1.2.3", (string)twin.Reported["firmwareVersion"]!);
            Assert.Equal(2ul, twin.ReportedVersion);
        }

        [Fact(Timeout = Setup.TestTimeoutMilliseconds)]
        public async Task Gen2_UpdateReportedProperties_WithStaleIfMatch_IsRejectedByTheStub()
        {
            await using var harness = await StubServiceTestHarness.StartAsync(IotHubGeneration.Gen2);
            Gen2ConnectionClient connection = await harness.ConnectGen2DeviceAsync();
            using var twinClient = new Gen2TwinClient(connection);

            ReportedPatchResponse response = await twinClient.UpdateReportedPropertiesAsync(
                new Gen2ReportedPatchRequest { ReportedProperties = new JsonObject { ["a"] = 1 }, IfMatch = 99 },
                TestContext.Current.CancellationToken);

            Assert.Equal(TwinResult.VersionMismatch, response.Result);
            Assert.Equal(1ul, response.Version);
            Assert.Empty(harness.Stub.GetDeviceState(harness.DeviceId).GetTwinSnapshot().Reported);
        }

        [Fact(Timeout = Setup.TestTimeoutMilliseconds)]
        public async Task Gen2_DesiredPatch_IsDeliveredToTheDevice()
        {
            await using var harness = await StubServiceTestHarness.StartAsync(IotHubGeneration.Gen2);
            Gen2ConnectionClient connection = await harness.ConnectGen2DeviceAsync();
            using var twinClient = new Gen2TwinClient(connection);

            TaskCompletionSource<DesiredPatchReceivedEventArgs> received = new();
            twinClient.DesiredPatchReceived += args => received.TrySetResult(args);

            await harness.Stub.WaitForDeviceBirthAsync(harness.DeviceId, TestContext.Current.CancellationToken);

            ulong newVersion = await harness.Stub.UpdateDesiredPropertiesAsync(
                harness.DeviceId,
                new JsonObject { ["fanSpeed"] = "high" },
                TestContext.Current.CancellationToken);

            DesiredPatchReceivedEventArgs patch = await received.Task.WaitAsync(TestContext.Current.CancellationToken);

            Assert.Equal(newVersion, patch.DesiredPropertiesVersion);
            Assert.Equal("high", (string)patch.DesiredProperties["fanSpeed"]!);
        }

        [Fact(Timeout = Setup.TestTimeoutMilliseconds)]
        public async Task Gen2_DirectMethod_RoundTripsThroughProbeAndExec()
        {
            await using var harness = await StubServiceTestHarness.StartAsync(IotHubGeneration.Gen2);
            Gen2ConnectionClient connection = await harness.ConnectGen2DeviceAsync();
            using var directMethodClient = new Gen2DirectMethodClient(connection);

            directMethodClient.DirectMethodProbeReceivedAsync += _ => Task.FromResult(Gen2DirectMethodProbeAck.Accepted());
            directMethodClient.DirectMethodInvokedAsync += args =>
            {
                Assert.Equal("reboot", args.MethodName);
                Assert.Equal("now", Encoding.UTF8.GetString(args.Payload!));

                return Task.FromResult(new DirectMethodResponse
                {
                    Status = 200,
                    Payload = Encoding.UTF8.GetBytes("rebooting"),
                });
            };

            await harness.Stub.WaitForDeviceBirthAsync(harness.DeviceId, TestContext.Current.CancellationToken);

            StubDirectMethodResult result = await harness.Stub.InvokeDirectMethodAsync(
                harness.DeviceId,
                "reboot",
                Encoding.UTF8.GetBytes("now"),
                cancellationToken: TestContext.Current.CancellationToken);

            Assert.Equal(StubDirectMethodOutcome.Completed, result.Outcome);
            Assert.Equal(200, result.Status);
            Assert.Equal("rebooting", Encoding.UTF8.GetString(result.Payload));
        }

        [Fact(Timeout = Setup.TestTimeoutMilliseconds)]
        public async Task Gen2_DirectMethod_RejectedProbe_IsSurfacedToTheCaller()
        {
            await using var harness = await StubServiceTestHarness.StartAsync(IotHubGeneration.Gen2);
            Gen2ConnectionClient connection = await harness.ConnectGen2DeviceAsync();
            using var directMethodClient = new Gen2DirectMethodClient(connection);

            directMethodClient.DirectMethodProbeReceivedAsync += _ =>
                Task.FromResult(Gen2DirectMethodProbeAck.Rejected(RejectedReason.MethodNotFound));
            directMethodClient.DirectMethodInvokedAsync += _ =>
                Task.FromResult(new DirectMethodResponse { Status = 200 });

            await harness.Stub.WaitForDeviceBirthAsync(harness.DeviceId, TestContext.Current.CancellationToken);

            StubDirectMethodResult result = await harness.Stub.InvokeDirectMethodAsync(
                harness.DeviceId,
                "notARealMethod",
                cancellationToken: TestContext.Current.CancellationToken);

            Assert.Equal(StubDirectMethodOutcome.Rejected, result.Outcome);
            Assert.Equal(RejectedReason.MethodNotFound, result.RejectedReason);
        }

        [Fact(Timeout = Setup.TestTimeoutMilliseconds)]
        public async Task Gen2_Telemetry_ReachesTheStub()
        {
            await using var harness = await StubServiceTestHarness.StartAsync(IotHubGeneration.Gen2);
            Gen2ConnectionClient connection = await harness.ConnectGen2DeviceAsync();
            using var telemetryClient = new Gen2TelemetryClient(connection);

            TaskCompletionSource<StubTelemetryReceivedEventArgs> received = new();
            harness.Stub.TelemetryReceived += (_, args) => received.TrySetResult(args);

            await telemetryClient.SendTelemetryAsync(
                new DeviceToCloudTelemetry
                {
                    Payload = Encoding.UTF8.GetBytes("{\"temperature\":21}"),
                    MessageId = "message-1",
                    ContentType = "application/json",
                    ContentEncoding = "utf-8",
                },
                TestContext.Current.CancellationToken);

            StubTelemetryReceivedEventArgs telemetry = await received.Task.WaitAsync(TestContext.Current.CancellationToken);

            Assert.Equal(harness.DeviceId, telemetry.DeviceId);
            Assert.Equal("{\"temperature\":21}", Encoding.UTF8.GetString(telemetry.Payload));
            Assert.Equal("message-1", telemetry.MessageId);
            Assert.Equal("application/json", telemetry.ContentType);
        }

        [Fact(Timeout = Setup.TestTimeoutMilliseconds)]
        public async Task Gen1_GetTwin_ReturnsTheStubsAuthoritativeState()
        {
            await using var harness = await StubServiceTestHarness.StartAsync(IotHubGeneration.Gen1);

            harness.Stub.GetDeviceState(harness.DeviceId).ReplaceDesiredProperties(new JsonObject { ["targetTemperature"] = 72 });

            UnifiedConnectionClient connection = await harness.ConnectGen1DeviceAsync();
            using var twinClient = new UnifiedTwinClient(connection);

            DeviceTwin twin = await twinClient.GetTwinAsync(TestContext.Current.CancellationToken);

            Assert.NotNull(twin.Desired);
            Assert.Equal(72, (int)twin.Desired["targetTemperature"]!);
            Assert.Equal(2ul, twin.DesiredVersion);
            Assert.NotNull(twin.Reported);
            Assert.Empty(twin.Reported);
            Assert.Equal(1ul, twin.ReportedVersion);
        }

        [Fact(Timeout = Setup.TestTimeoutMilliseconds)]
        public async Task Gen1_UpdateReportedProperties_IsAppliedByTheStub()
        {
            await using var harness = await StubServiceTestHarness.StartAsync(IotHubGeneration.Gen1);
            UnifiedConnectionClient connection = await harness.ConnectGen1DeviceAsync();
            using var twinClient = new UnifiedTwinClient(connection);

            ReportedPatchResponse response = await twinClient.UpdateReportedPropertiesAsync(
                new JsonObject { ["firmwareVersion"] = "1.2.3" },
                TestContext.Current.CancellationToken);

            Assert.Equal(TwinResult.Ok, response.Result);
            Assert.Equal(2ul, response.Version);

            StubTwinSnapshot twin = harness.Stub.GetDeviceState(harness.DeviceId).GetTwinSnapshot();
            Assert.Equal("1.2.3", (string)twin.Reported["firmwareVersion"]!);
        }

        [Fact(Timeout = Setup.TestTimeoutMilliseconds)]
        public async Task Gen1_DesiredPatch_IsDeliveredToTheDevice()
        {
            await using var harness = await StubServiceTestHarness.StartAsync(IotHubGeneration.Gen1);
            UnifiedConnectionClient connection = await harness.ConnectGen1DeviceAsync();
            using var twinClient = new UnifiedTwinClient(connection);

            TaskCompletionSource<DesiredPatchReceivedEventArgs> received = new();
            twinClient.DesiredPatchReceived += args => received.TrySetResult(args);

            ulong newVersion = await harness.Stub.UpdateDesiredPropertiesAsync(
                harness.DeviceId,
                new JsonObject { ["fanSpeed"] = "high" },
                TestContext.Current.CancellationToken);

            DesiredPatchReceivedEventArgs patch = await received.Task.WaitAsync(TestContext.Current.CancellationToken);

            Assert.Equal(newVersion, patch.DesiredPropertiesVersion);
            Assert.Equal("high", (string)patch.DesiredProperties["fanSpeed"]!);
        }

        [Fact(Timeout = Setup.TestTimeoutMilliseconds)]
        public async Task Gen1_DirectMethod_RoundTrips()
        {
            await using var harness = await StubServiceTestHarness.StartAsync(IotHubGeneration.Gen1);
            UnifiedConnectionClient connection = await harness.ConnectGen1DeviceAsync();
            using var directMethodClient = new UnifiedDirectMethodClient(connection);

            directMethodClient.DirectMethodInvokedAsync += args =>
            {
                Assert.Equal("reboot", args.MethodName);
                Assert.Equal("now", Encoding.UTF8.GetString(args.Payload!));

                return Task.FromResult(new DirectMethodResponse
                {
                    Status = 200,
                    Payload = Encoding.UTF8.GetBytes("rebooting"),
                });
            };

            StubDirectMethodResult result = await harness.Stub.InvokeDirectMethodAsync(
                harness.DeviceId,
                "reboot",
                Encoding.UTF8.GetBytes("now"),
                cancellationToken: TestContext.Current.CancellationToken);

            Assert.Equal(StubDirectMethodOutcome.Completed, result.Outcome);
            Assert.Equal(200, result.Status);
            Assert.Equal("rebooting", Encoding.UTF8.GetString(result.Payload));
        }

        [Fact(Timeout = Setup.TestTimeoutMilliseconds)]
        public async Task Gen1_Telemetry_ReachesTheStub()
        {
            await using var harness = await StubServiceTestHarness.StartAsync(IotHubGeneration.Gen1);
            UnifiedConnectionClient connection = await harness.ConnectGen1DeviceAsync();
            using var telemetryClient = new UnifiedTelemetryClient(connection);

            TaskCompletionSource<StubTelemetryReceivedEventArgs> received = new();
            harness.Stub.TelemetryReceived += (_, args) => received.TrySetResult(args);

            await telemetryClient.SendTelemetryAsync(
                new DeviceToCloudTelemetry
                {
                    Payload = Encoding.UTF8.GetBytes("{\"temperature\":21}"),
                    MessageId = "message-1",
                    ContentType = "application/json",
                    UserProperties = { ["site"] = "redmond" },
                },
                TestContext.Current.CancellationToken);

            StubTelemetryReceivedEventArgs telemetry = await received.Task.WaitAsync(TestContext.Current.CancellationToken);

            Assert.Equal(harness.DeviceId, telemetry.DeviceId);
            Assert.Equal("{\"temperature\":21}", Encoding.UTF8.GetString(telemetry.Payload));
            Assert.Equal("message-1", telemetry.MessageId);
            Assert.Equal("application/json", telemetry.ContentType);
            Assert.Equal("redmond", telemetry.UserProperties["site"]);
        }

        [Fact(Timeout = Setup.TestTimeoutMilliseconds)]
        public async Task Gen1_CloudToDeviceMessage_ReachesTheDevice()
        {
            await using var harness = await StubServiceTestHarness.StartAsync(IotHubGeneration.Gen1);
            UnifiedConnectionClient connection = await harness.ConnectGen1DeviceAsync();
            using var telemetryClient = new UnifiedTelemetryClient(connection);

            TaskCompletionSource<CloudToDeviceTelemetry> received = new();
            telemetryClient.CloudToDeviceTelemetryReceivedAsync += message =>
            {
                received.TrySetResult(message);
                return Task.CompletedTask;
            };

            await harness.Stub.SendCloudToDeviceMessageAsync(
                harness.DeviceId,
                new StubCloudToDeviceMessage
                {
                    Payload = Encoding.UTF8.GetBytes("hello device"),
                    MessageId = "c2d-1",
                },
                TestContext.Current.CancellationToken);

            CloudToDeviceTelemetry message = await received.Task.WaitAsync(TestContext.Current.CancellationToken);

            Assert.Equal("hello device", Encoding.UTF8.GetString(message.Payload));
            Assert.Equal("c2d-1", message.MessageId);
        }

        /// <summary>
        /// Wires an <see cref="InProcessMqttBroker"/>, a <see cref="StubIotHubService"/>, and a device client together.
        /// </summary>
        [Fact(Timeout = Setup.TestTimeoutMilliseconds)]
        public async Task Provisioning_AssignsTheDeviceToTheGen2Hub()
        {
            await using var harness = await StubServiceTestHarness.StartAsync(IotHubGeneration.Gen2, withProvisioningService: true);

            (UnifiedConnectionClient client, ConnectionContext context) = await harness.ProvisionAndConnectDeviceAsync();

            Assert.Equal(harness.DeviceId, context.DeviceId);
            Assert.Equal(harness.Stub.HubHostName, context.IotHubHostName);
            Assert.True(context.IsGen2Hub);

            // The device is not merely told it is on a gen2 hub - it completed the gen2 presence handshake, which
            // only exists on the Event Grid based hub, to get here.
            StubDeviceBirthEventArgs birth = await harness.Stub.WaitForDeviceBirthAsync(harness.DeviceId, TestContext.Current.CancellationToken);
            Assert.Equal(harness.DeviceId, birth.DeviceId);

            // And telemetry proves the device is publishing on the gen2 "ih/{deviceId}/dev/..." topics, not the
            // classic "devices/{deviceId}/messages/events/" ones.
            using var telemetryClient = new UnifiedTelemetryClient(client);

            TaskCompletionSource<StubTelemetryReceivedEventArgs> received = new();
            harness.Stub.TelemetryReceived += (_, args) => received.TrySetResult(args);

            await telemetryClient.SendTelemetryAsync(
                new DeviceToCloudTelemetry { Payload = Encoding.UTF8.GetBytes("{\"temperature\":21}") },
                TestContext.Current.CancellationToken);

            StubTelemetryReceivedEventArgs telemetry = await received.Task.WaitAsync(TestContext.Current.CancellationToken);

            Assert.Equal(harness.DeviceId, telemetry.DeviceId);
            Assert.Equal("{\"temperature\":21}", Encoding.UTF8.GetString(telemetry.Payload));
        }

        [Fact(Timeout = Setup.TestTimeoutMilliseconds)]
        public async Task Provisioning_AssignsTheDeviceToTheGen1Hub()
        {
            await using var harness = await StubServiceTestHarness.StartAsync(IotHubGeneration.Gen1, withProvisioningService: true);

            (UnifiedConnectionClient client, ConnectionContext context) = await harness.ProvisionAndConnectDeviceAsync();

            Assert.Equal(harness.DeviceId, context.DeviceId);
            Assert.Equal(harness.Stub.HubHostName, context.IotHubHostName);
            Assert.False(context.IsGen2Hub);

            // A twin round trip proves the device is speaking the classic JSON protocol on the $iothub/... topics.
            harness.Stub.GetDeviceState(harness.DeviceId).ReplaceDesiredProperties(new JsonObject { ["targetTemperature"] = 72 });

            using var twinClient = new UnifiedTwinClient(client);
            DeviceTwin twin = await twinClient.GetTwinAsync(TestContext.Current.CancellationToken);

            Assert.Equal(72, (int)twin.Desired!["targetTemperature"]!);
            Assert.Equal(2ul, twin.DesiredVersion);
        }

        [Fact(Timeout = Setup.TestTimeoutMilliseconds)]
        public async Task Provisioning_RaisesDeviceProvisioned()
        {
            await using var harness = await StubServiceTestHarness.StartAsync(IotHubGeneration.Gen2, withProvisioningService: true);

            TaskCompletionSource<StubDeviceProvisionedEventArgs> provisioned = new();
            harness.Dps.DeviceProvisioned += (_, args) => provisioned.TrySetResult(args);

            await harness.ProvisionAndConnectDeviceAsync();

            StubDeviceProvisionedEventArgs args = await provisioned.Task.WaitAsync(TestContext.Current.CancellationToken);

            Assert.Equal(harness.DeviceId, args.DeviceId);
            Assert.Equal(harness.DeviceId, args.RegistrationId);
            Assert.Equal(harness.Stub.HubHostName, args.AssignedHubHostName);
            Assert.Equal(IotHubGeneration.Gen2, args.Generation);
        }

        [Fact(Timeout = Setup.TestTimeoutMilliseconds)]
        public async Task Provisioning_PollsUntilTheRegistrationIsAssigned()
        {
            await using var harness = await StubServiceTestHarness.StartAsync(
                IotHubGeneration.Gen2,
                configureProvisioningService: options => options.AssigningPollResponses = 1);

            (_, ConnectionContext context) = await harness.ProvisionAndConnectDeviceAsync();

            Assert.Equal(harness.DeviceId, context.DeviceId);
            Assert.True(context.IsGen2Hub);
        }

        [Fact]
        public async Task Provisioning_ForHub_InheritsTheHubsGenerationAndEndpoint()
        {
            await using var hub = new StubIotHubService(new StubIotHubServiceOptions
            {
                Generation = IotHubGeneration.Gen1,
                HubHostName = "some-hub.azure-devices.net",
                BrokerHostName = "broker.example",
                BrokerPort = 18830,
            });

            StubDeviceProvisioningServiceOptions? captured = null;

            await using StubDeviceProvisioningService dps = StubDeviceProvisioningService.ForHub(hub, options =>
            {
                captured = options;
                options.DeviceId = "device-1";
            });

            Assert.NotNull(captured);
            Assert.Equal(IotHubGeneration.Gen1, dps.Generation);
            Assert.Equal("broker.example", captured.BrokerHostName);
            Assert.Equal(18830, captured.BrokerPort);
            Assert.Equal("some-hub.azure-devices.net", captured.AssignedHubHostName);
        }

        /// <summary>
        /// Wires an <see cref="InProcessMqttBroker"/>, a <see cref="StubIotHubService"/>, an optional
        /// <see cref="StubDeviceProvisioningService"/>, and a device client together.
        /// </summary>
        private sealed class StubServiceTestHarness : IAsyncDisposable
        {
            private readonly List<IDisposable> _disposables = new();
            private InProcessMqttBroker _broker = null!;

            public string DeviceId { get; } = "stub-device-" + Guid.NewGuid().ToString("N")[..8];

            public StubIotHubService Stub { get; private set; } = null!;

            /// <summary>
            /// The stub DPS, if this harness was started with one.
            /// </summary>
            public StubDeviceProvisioningService Dps { get; private set; } = null!;

            public static async Task<StubServiceTestHarness> StartAsync(
                IotHubGeneration generation,
                bool withProvisioningService = false,
                Action<StubDeviceProvisioningServiceOptions>? configureProvisioningService = null)
            {
                var harness = new StubServiceTestHarness();

                harness._broker = await InProcessMqttBroker.StartAsync(TestContext.Current.CancellationToken);

                harness.Stub = new StubIotHubService(new StubIotHubServiceOptions
                {
                    Generation = generation,
                    HubHostName = StubHostName,
                    BrokerHostName = harness._broker.HostName,
                    BrokerPort = harness._broker.Port,

                    // Classic hub topics carry no device id, so the stub has to be told which device it is serving.
                    DeviceIdFilter = generation == IotHubGeneration.Gen1 ? harness.DeviceId : null,
                });

                await harness.Stub.StartAsync(TestContext.Current.CancellationToken);

                if (withProvisioningService || configureProvisioningService != null)
                {
                    harness.Dps = StubDeviceProvisioningService.ForHub(harness.Stub, options =>
                    {
                        options.DeviceId = harness.DeviceId;
                        configureProvisioningService?.Invoke(options);
                    });

                    await harness.Dps.StartAsync(TestContext.Current.CancellationToken);
                }

                return harness;
            }

            public async Task<Gen2ConnectionClient> ConnectGen2DeviceAsync()
            {
                var connectionClient = new Gen2ConnectionClient(BuildClientOptions());
                _disposables.Add(connectionClient);

                await connectionClient.ConnectAsync(BuildConnectionContext(isGen2Hub: true), null, TestContext.Current.CancellationToken);

                return connectionClient;
            }

            public async Task<UnifiedConnectionClient> ConnectGen1DeviceAsync()
            {
                var connectionClient = new UnifiedConnectionClient(BuildClientOptions());
                _disposables.Add(connectionClient);

                await connectionClient.ConnectAsync(BuildConnectionContext(isGen2Hub: false), TestContext.Current.CancellationToken);

                return connectionClient;
            }

            /// <summary>
            /// Runs the SDK's full provisioning flow: register with the stub DPS, then connect to whichever hub it assigned.
            /// </summary>
            public async Task<(UnifiedConnectionClient Client, ConnectionContext Context)> ProvisionAndConnectDeviceAsync()
            {
                var connectionClient = new UnifiedConnectionClient(BuildClientOptions());
                _disposables.Add(connectionClient);

                ConnectionContext context = await connectionClient.ProvisionAndConnectAsync(
                    new ProvisioningSettings(IdScope),
                    new X509AuthenticationProvider(CreateSelfSignedCertificate(DeviceId)),
                    TestContext.Current.CancellationToken);

                return (connectionClient, context);
            }

            public async ValueTask DisposeAsync()
            {
                foreach (IDisposable disposable in _disposables)
                {
                    try
                    {
                        disposable.Dispose();
                    }
                    catch (Exception)
                    {
                        // Teardown of a test harness should never mask the test's own failure.
                    }
                }

                if (Stub != null)
                {
                    await Stub.DisposeAsync();
                }

                if (Dps != null)
                {
                    await Dps.DisposeAsync();
                }

                if (_broker != null)
                {
                    await _broker.DisposeAsync();
                }
            }

            private ConnectionClientOptions BuildClientOptions()
            {
                return new ConnectionClientOptions
                {
                    MqttClient = new LocalBrokerMqttClient(_broker.HostName, _broker.Port),

                    // Without this, a broker that drops the connection mid-test would keep the test hanging in retries.
                    ConnectionRetryPolicy = new Retry.NoRetry(),
                };
            }

            private ConnectionContext BuildConnectionContext(bool isGen2Hub)
            {
                return new ConnectionContext
                {
                    DeviceId = DeviceId,
                    IotHubHostName = StubHostName,
                    IsGen2Hub = isGen2Hub,
                    AuthenticationProvider = new X509AuthenticationProvider(CreateSelfSignedCertificate(DeviceId)),
                };
            }

            /// <summary>
            /// The SDK requires a client certificate to build its CONNECT packet, but
            /// <see cref="LocalBrokerMqttClient"/> strips it before connecting to the plaintext local broker.
            /// </summary>
            private static X509Certificate2 CreateSelfSignedCertificate(string deviceId)
            {
                using RSA key = RSA.Create(2048);

                var request = new CertificateRequest($"CN={deviceId}", key, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);

                return request.CreateSelfSigned(DateTimeOffset.UtcNow.AddDays(-1), DateTimeOffset.UtcNow.AddDays(1));
            }
        }
    }
}
