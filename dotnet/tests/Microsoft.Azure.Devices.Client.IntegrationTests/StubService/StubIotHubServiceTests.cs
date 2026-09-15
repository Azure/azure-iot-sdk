using Microsoft.Azure.Devices.Client.Provisioning.Models;
using Microsoft.Azure.Devices.Client.Models;
using Microsoft.Azure.Devices.Client.Models.DirectMethods;
using Microsoft.Azure.Devices.Client.Models.Telemetry;
using Microsoft.Azure.Devices.Client.Models.Twin;
using MQTTnet.Protocol;
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
            await using var harness = await StubServiceTestHarness.StartAsync(IotHubGeneration.Gen2);

            (UnifiedConnectionClient client, ConnectionContext context) = await harness.ProvisionAndConnectDeviceAsync();

            Assert.Equal(harness.DeviceId, context.DeviceId);
            Assert.Equal(harness.Stub.HubHostName, context.IotHubHostName);
            Assert.Equal(ConnectionProfile.MqttV5, context.ConnectionProfile);

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
            await using var harness = await StubServiceTestHarness.StartAsync(IotHubGeneration.Gen1);

            (UnifiedConnectionClient client, ConnectionContext context) = await harness.ProvisionAndConnectDeviceAsync();

            Assert.Equal(harness.DeviceId, context.DeviceId);
            Assert.Equal(harness.Stub.HubHostName, context.IotHubHostName);
            Assert.Equal(ConnectionProfile.Classic, context.ConnectionProfile);

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
            await using var harness = await StubServiceTestHarness.StartAsync(IotHubGeneration.Gen2);

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
            Assert.Equal(ConnectionProfile.MqttV5, context.ConnectionProfile);
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

        public static TheoryData<MqttDisconnectReasonCode> AllDisconnectReasonCodes => [.. StubDisconnectReasonCodes.All];

        [Theory]
        [MemberData(nameof(AllDisconnectReasonCodes))]
        public async Task DropDeviceConnection_SendsTheRequestedReasonCode(MqttDisconnectReasonCode reasonCode)
        {
            const string deviceId = "drop-test-device";

            var dropper = new RecordingConnectionDropper(deviceId);
            await using var hub = new StubIotHubService(new StubIotHubServiceOptions { ConnectionDropper = dropper });
            hub.GetDeviceState(deviceId);

            StubDeviceConnectionDroppedEventArgs? observed = null;
            hub.DeviceConnectionDropped += (_, args) => observed = args;

            bool wasDropped = await hub.DropDeviceConnectionAsync(
                deviceId,
                reasonCode,
                "the stub says so",
                TestContext.Current.CancellationToken);

            Assert.True(wasDropped);
            Assert.Equal((deviceId, reasonCode, "the stub says so"), Assert.Single(dropper.Drops));

            Assert.NotNull(observed);
            Assert.Equal(deviceId, observed.DeviceId);
            Assert.Equal(reasonCode, observed.ReasonCode);
            Assert.False(observed.WasRandom);

            StubDeviceConnectionDroppedEventArgs recorded = Assert.Single(hub.ConnectionDropHistory);
            Assert.Equal(reasonCode, recorded.ReasonCode);
        }

        [Fact]
        public async Task DropDeviceConnection_WithRandomReasonCodes_EventuallyUsesEveryConfiguredCode()
        {
            const string deviceId = "drop-test-device";

            var dropper = new RecordingConnectionDropper(deviceId);
            await using var hub = new StubIotHubService(new StubIotHubServiceOptions
            {
                ConnectionDropper = dropper,
                RandomConnectionDrops = new StubConnectionDropOptions
                {
                    ReasonCodes = StubDisconnectReasonCodes.All,

                    // A fixed seed keeps a randomized assertion from being able to fail only on some runs.
                    RandomSeed = 20260914,
                },
            });
            hub.GetDeviceState(deviceId);

            for (int i = 0; i < 2000; i++)
            {
                StubDeviceConnectionDroppedEventArgs? dropped = await hub.DropRandomDeviceConnectionAsync(TestContext.Current.CancellationToken);
                Assert.NotNull(dropped);
                Assert.Equal(deviceId, dropped.DeviceId);
                Assert.True(dropped.WasRandom);
            }

            IEnumerable<MqttDisconnectReasonCode> observed = hub.ConnectionDropHistory
                .Select(drop => drop.ReasonCode)
                .Distinct()
                .Order();

            Assert.Equal(StubDisconnectReasonCodes.All.Order(), observed);
        }

        [Fact]
        public async Task DropDeviceConnection_WithTheSameSeed_ProducesTheSameReasonCodes()
        {
            static async Task<List<MqttDisconnectReasonCode>> DropTwentyAsync()
            {
                const string deviceId = "drop-test-device";

                var dropper = new RecordingConnectionDropper(deviceId);
                await using var hub = new StubIotHubService(new StubIotHubServiceOptions
                {
                    ConnectionDropper = dropper,
                    RandomConnectionDrops = new StubConnectionDropOptions { RandomSeed = 7 },
                });
                hub.GetDeviceState(deviceId);

                for (int i = 0; i < 20; i++)
                {
                    await hub.DropRandomDeviceConnectionAsync(TestContext.Current.CancellationToken);
                }

                return [.. hub.ConnectionDropHistory.Select(drop => drop.ReasonCode)];
            }

            Assert.Equal(await DropTwentyAsync(), await DropTwentyAsync());
        }

        [Fact]
        public async Task DropDeviceConnection_DefaultReasonCodes_ExcludeTheClientOnlyCode()
        {
            // MQTT 5 section 3.14.2.1 only allows a client to send DisconnectWithWillMessage, so the stub does not pick it
            // unless a test opts in with StubDisconnectReasonCodes.All.
            Assert.DoesNotContain(MqttDisconnectReasonCode.DisconnectWithWillMessage, StubDisconnectReasonCodes.ServerInitiated);
            Assert.Contains(MqttDisconnectReasonCode.DisconnectWithWillMessage, StubDisconnectReasonCodes.All);
            Assert.DoesNotContain(MqttDisconnectReasonCode.NormalDisconnection, StubDisconnectReasonCodes.ServerInitiatedErrors);

            const string deviceId = "drop-test-device";

            var dropper = new RecordingConnectionDropper(deviceId);
            await using var hub = new StubIotHubService(new StubIotHubServiceOptions { ConnectionDropper = dropper });
            hub.GetDeviceState(deviceId);

            for (int i = 0; i < 500; i++)
            {
                await hub.DropRandomDeviceConnectionAsync(TestContext.Current.CancellationToken);
            }

            Assert.DoesNotContain(
                MqttDisconnectReasonCode.DisconnectWithWillMessage,
                hub.ConnectionDropHistory.Select(drop => drop.ReasonCode));
        }

        [Fact]
        public async Task DropDeviceConnection_WithoutAConnectionDropper_Throws()
        {
            await using var hub = new StubIotHubService(new StubIotHubServiceOptions());
            hub.GetDeviceState("drop-test-device");

            Assert.False(hub.CanDropConnections);
            await Assert.ThrowsAsync<InvalidOperationException>(() => hub.DropDeviceConnectionAsync(
                "drop-test-device",
                MqttDisconnectReasonCode.ServerBusy,
                cancellationToken: TestContext.Current.CancellationToken));
        }

        [Fact]
        public async Task DropDeviceConnection_ForADeviceThatIsNotConnected_ReportsNoDrop()
        {
            await using var harness = await StubServiceTestHarness.StartAsync(IotHubGeneration.Gen2);
            harness.Stub.GetDeviceState(harness.DeviceId);

            bool wasDropped = await harness.Stub.DropDeviceConnectionAsync(
                harness.DeviceId,
                MqttDisconnectReasonCode.ServerBusy,
                cancellationToken: TestContext.Current.CancellationToken);

            Assert.False(wasDropped);
            Assert.Empty(harness.Stub.ConnectionDropHistory);
        }

        [Theory]
        [InlineData(MqttDisconnectReasonCode.ServerBusy)]
        [InlineData(MqttDisconnectReasonCode.SessionTakenOver)]
        [InlineData(MqttDisconnectReasonCode.NotAuthorized)]
        [InlineData(MqttDisconnectReasonCode.AdministrativeAction)]
        public async Task Gen2_DropDeviceConnection_DisconnectsTheDeviceUnderTest(MqttDisconnectReasonCode reasonCode)
        {
            await using var harness = await StubServiceTestHarness.StartAsync(IotHubGeneration.Gen2);
            await harness.ConnectGen2DeviceAsync();

            await harness.Stub.WaitForDeviceBirthAsync(harness.DeviceId, TestContext.Current.CancellationToken);

            // The device is on MQTT 5, so the reason code the stub chose reaches it in the DISCONNECT packet.
            TaskCompletionSource<Mqtt.MqttClientDisconnectedEventArgs> disconnected = new(TaskCreationOptions.RunContinuationsAsynchronously);
            harness.DeviceMqttClient.DisconnectedAsync += args =>
            {
                disconnected.TrySetResult(args);
                return Task.CompletedTask;
            };

            bool wasDropped = await harness.Stub.DropDeviceConnectionAsync(
                harness.DeviceId,
                reasonCode,
                "the stub says so",
                TestContext.Current.CancellationToken);

            Assert.True(wasDropped);

            Mqtt.MqttClientDisconnectedEventArgs args = await disconnected.Task.WaitAsync(
                TimeSpan.FromSeconds(30),
                TestContext.Current.CancellationToken);

            Assert.Equal((int)reasonCode, (int)args.Reason);

            StubDeviceConnectionDroppedEventArgs recorded = Assert.Single(harness.Stub.ConnectionDropHistory);
            Assert.Equal(harness.DeviceId, recorded.DeviceId);
            Assert.Equal(reasonCode, recorded.ReasonCode);
            Assert.Equal("the stub says so", recorded.ReasonString);

            // The device has to redo the presence handshake before the stub may dispatch to it again.
            Assert.False(harness.Stub.GetDeviceState(harness.DeviceId).IsDispatchReady);

            Assert.DoesNotContain(
                harness.DeviceId,
                await harness.Broker.GetConnectedClientIdsAsync(TestContext.Current.CancellationToken));
        }

        [Fact(Timeout = Setup.TestTimeoutMilliseconds)]
        public async Task Gen2_RandomConnectionDrops_DisconnectTheDeviceUnderTest()
        {
            await using var harness = await StubServiceTestHarness.StartAsync(
                IotHubGeneration.Gen2,
                configureHub: options => options.RandomConnectionDrops = new StubConnectionDropOptions
                {
                    Enabled = true,

                    // Long enough that the device is provisioned and connected before the first drop attempt lands.
                    MinInterval = TimeSpan.FromSeconds(3),
                    MaxInterval = TimeSpan.FromSeconds(4),
                    ReasonCodes = StubDisconnectReasonCodes.All,
                    MaxDrops = 1,
                    RandomSeed = 20260914,
                });

            TaskCompletionSource<StubDeviceConnectionDroppedEventArgs> dropped = new(TaskCreationOptions.RunContinuationsAsynchronously);
            harness.Stub.DeviceConnectionDropped += (_, args) => dropped.TrySetResult(args);

            await harness.ConnectGen2DeviceAsync();

            TaskCompletionSource<Mqtt.MqttClientDisconnectedEventArgs> disconnected = new(TaskCreationOptions.RunContinuationsAsynchronously);
            harness.DeviceMqttClient.DisconnectedAsync += args =>
            {
                disconnected.TrySetResult(args);
                return Task.CompletedTask;
            };

            StubDeviceConnectionDroppedEventArgs drop = await dropped.Task.WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);

            Assert.Equal(harness.DeviceId, drop.DeviceId);
            Assert.True(drop.WasRandom);
            Assert.Contains(drop.ReasonCode, StubDisconnectReasonCodes.All);

            Mqtt.MqttClientDisconnectedEventArgs args = await disconnected.Task.WaitAsync(
                TimeSpan.FromSeconds(30),
                TestContext.Current.CancellationToken);

            Assert.Equal((int)drop.ReasonCode, (int)args.Reason);
        }

        [Fact]
        public async Task RandomConnectionDrops_WithoutAConnectionDropper_FailOnStart()
        {
            await using var broker = await InProcessMqttBroker.StartAsync(TestContext.Current.CancellationToken);
            await using var hub = new StubIotHubService(new StubIotHubServiceOptions
            {
                BrokerHostName = broker.HostName,
                BrokerPort = broker.Port,
                RandomConnectionDrops = new StubConnectionDropOptions { Enabled = true },
            });

            await Assert.ThrowsAsync<InvalidOperationException>(() => hub.StartAsync(TestContext.Current.CancellationToken));
        }

        [Fact]
        public void RandomConnectionDropOptions_RejectAnEmptyReasonCodeList()
        {
            Assert.Throws<ArgumentException>(() => new StubConnectionDropOptions { ReasonCodes = [] });
        }

        [Theory]
        [MemberData(nameof(AllDisconnectReasonCodes))]
        public async Task Provisioning_DropDeviceConnection_SendsTheRequestedReasonCode(MqttDisconnectReasonCode reasonCode)
        {
            const string deviceId = "drop-test-device";

            var dropper = new RecordingConnectionDropper(deviceId);
            await using var dps = new StubDeviceProvisioningService(new StubDeviceProvisioningServiceOptions
            {
                DeviceId = deviceId,
                ConnectionDropper = dropper,
            });

            StubDeviceConnectionDroppedEventArgs? observed = null;
            dps.DeviceConnectionDropped += (_, args) => observed = args;

            bool wasDropped = await dps.DropDeviceConnectionAsync(
                deviceId,
                reasonCode,
                "the stub says so",
                TestContext.Current.CancellationToken);

            Assert.True(wasDropped);
            Assert.Equal((deviceId, reasonCode, "the stub says so"), Assert.Single(dropper.Drops));

            Assert.NotNull(observed);
            Assert.Equal(reasonCode, observed.ReasonCode);
            Assert.False(observed.WasRandom);

            StubDeviceConnectionDroppedEventArgs recorded = Assert.Single(dps.ConnectionDropHistory);
            Assert.Equal(reasonCode, recorded.ReasonCode);
        }

        [Fact]
        public async Task Provisioning_DropDeviceConnection_WithRandomReasonCodes_EventuallyUsesEveryConfiguredCode()
        {
            const string deviceId = "drop-test-device";

            var dropper = new RecordingConnectionDropper(deviceId);
            await using var dps = new StubDeviceProvisioningService(new StubDeviceProvisioningServiceOptions
            {
                DeviceId = deviceId,
                ConnectionDropper = dropper,
                RandomConnectionDrops = new StubConnectionDropOptions
                {
                    ReasonCodes = StubDisconnectReasonCodes.All,
                    RandomSeed = 20260914,
                },
            });

            for (int i = 0; i < 2000; i++)
            {
                StubDeviceConnectionDroppedEventArgs? dropped = await dps.DropRandomDeviceConnectionAsync(TestContext.Current.CancellationToken);
                Assert.NotNull(dropped);
                Assert.Equal(deviceId, dropped.DeviceId);
                Assert.True(dropped.WasRandom);
            }

            IEnumerable<MqttDisconnectReasonCode> observed = dps.ConnectionDropHistory
                .Select(drop => drop.ReasonCode)
                .Distinct()
                .Order();

            Assert.Equal(StubDisconnectReasonCodes.All.Order(), observed);
        }

        [Fact]
        public async Task Provisioning_DropDeviceConnection_OnlyTargetsTheDeviceItRegisters()
        {
            const string deviceId = "drop-test-device";

            // The broker also holds the stubs' own connections, and DPS must never pick one of those.
            var dropper = new RecordingConnectionDropper(deviceId, "some-other-client");
            await using var dps = new StubDeviceProvisioningService(new StubDeviceProvisioningServiceOptions
            {
                DeviceId = deviceId,
                ConnectionDropper = dropper,
            });

            await Assert.ThrowsAsync<InvalidOperationException>(() => dps.DropDeviceConnectionAsync(
                "some-other-client",
                MqttDisconnectReasonCode.ServerBusy,
                cancellationToken: TestContext.Current.CancellationToken));

            for (int i = 0; i < 50; i++)
            {
                StubDeviceConnectionDroppedEventArgs? dropped = await dps.DropRandomDeviceConnectionAsync(TestContext.Current.CancellationToken);
                Assert.Equal(deviceId, dropped?.DeviceId);
            }
        }

        [Fact]
        public async Task Provisioning_DropDeviceConnection_WithoutAConnectionDropper_Throws()
        {
            await using var dps = new StubDeviceProvisioningService(new StubDeviceProvisioningServiceOptions
            {
                DeviceId = "drop-test-device",
            });

            Assert.False(dps.CanDropConnections);
            await Assert.ThrowsAsync<InvalidOperationException>(() => dps.DropDeviceConnectionAsync(
                "drop-test-device",
                MqttDisconnectReasonCode.ServerBusy,
                cancellationToken: TestContext.Current.CancellationToken));
        }

        [Fact]
        public async Task Provisioning_RandomConnectionDrops_WithoutAConnectionDropper_FailOnStart()
        {
            await using var broker = await InProcessMqttBroker.StartAsync(TestContext.Current.CancellationToken);
            await using var dps = new StubDeviceProvisioningService(new StubDeviceProvisioningServiceOptions
            {
                DeviceId = "drop-test-device",
                BrokerHostName = broker.HostName,
                BrokerPort = broker.Port,
                RandomConnectionDrops = new StubConnectionDropOptions { Enabled = true },
            });

            await Assert.ThrowsAsync<InvalidOperationException>(() => dps.StartAsync(TestContext.Current.CancellationToken));
        }

        [Fact]
        public async Task Provisioning_ForHub_InheritsTheHubsConnectionDropper()
        {
            await using var broker = await InProcessMqttBroker.StartAsync(TestContext.Current.CancellationToken);
            await using var hub = new StubIotHubService(new StubIotHubServiceOptions { ConnectionDropper = broker });

            StubDeviceProvisioningServiceOptions? captured = null;

            await using StubDeviceProvisioningService dps = StubDeviceProvisioningService.ForHub(hub, options =>
            {
                captured = options;
                options.DeviceId = "device-1";
            });

            Assert.NotNull(captured);
            Assert.Same(broker, captured.ConnectionDropper);
            Assert.True(dps.CanDropConnections);
        }

        [Fact(Timeout = Setup.TestTimeoutMilliseconds)]
        public async Task Provisioning_RandomConnectionDrops_InterruptTheRegistrationInProgress()
        {
            // Every extra poll costs at least the SDK's two second retry-after floor, which is what keeps the device on the
            // DPS connection long enough for the drop below to land mid registration.
            await using var harness = await StubServiceTestHarness.StartAsync(
                IotHubGeneration.Gen2,
                configureProvisioningService: options =>
                {
                    options.AssigningPollResponses = 3;
                    options.RandomConnectionDrops = new StubConnectionDropOptions
                    {
                        Enabled = true,
                        MinInterval = TimeSpan.FromSeconds(1),
                        MaxInterval = TimeSpan.FromSeconds(2),
                        ReasonCodes = StubDisconnectReasonCodes.All,
                        MaxDrops = 1,
                        RandomSeed = 20260914,
                    };
                });

            TaskCompletionSource<StubDeviceConnectionDroppedEventArgs> dropped = new(TaskCreationOptions.RunContinuationsAsynchronously);
            harness.Dps.DeviceConnectionDropped += (_, args) => dropped.TrySetResult(args);

            // The SDK's retry policy here is NoRetry, so losing the DPS connection fails the registration outright rather
            // than silently starting it over.
            Task provisioning = harness.ProvisionAndConnectDeviceAsync();

            StubDeviceConnectionDroppedEventArgs drop = await dropped.Task.WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);

            Assert.Equal(harness.DeviceId, drop.DeviceId);
            Assert.True(drop.WasRandom);
            Assert.Contains(drop.ReasonCode, StubDisconnectReasonCodes.All);

            await Assert.ThrowsAnyAsync<Exception>(() => provisioning);
        }

        [Fact(Timeout = Setup.TestTimeoutMilliseconds)]
        public async Task Provisioning_DropDeviceConnection_DisconnectsTheRegisteringDevice()
        {
            await using var harness = await StubServiceTestHarness.StartAsync(
                IotHubGeneration.Gen2,
                configureProvisioningService: options => options.AssigningPollResponses = 3);

            Task provisioning = harness.ProvisionAndConnectDeviceAsync();
            TaskCompletionSource<int> seen = new(TaskCreationOptions.RunContinuationsAsynchronously);
            harness.DeviceMqttClient.DisconnectedAsync += args =>
            {
                seen.TrySetResult((int)args.Reason);
                return Task.CompletedTask;
            };

            // Wait until the device is actually on the broker before cutting it off. It connects to DPS with its
            // registration id as the client id, which for these tests is the device id.
            while (!(await harness.Broker.GetConnectedClientIdsAsync(TestContext.Current.CancellationToken)).Contains(harness.DeviceId))
            {
                await Task.Delay(10, TestContext.Current.CancellationToken);
            }

            bool wasDropped = await harness.Dps.DropDeviceConnectionAsync(
                harness.DeviceId,
                MqttDisconnectReasonCode.ServerBusy,
                cancellationToken: TestContext.Current.CancellationToken);

            Assert.True(wasDropped);
            Assert.Equal(MqttDisconnectReasonCode.ServerBusy, Assert.Single(harness.Dps.ConnectionDropHistory).ReasonCode);

            // MQTTnet's broker hands the reason code to the device even though provisioning runs over MQTT 3.1.1, which has
            // no server to client DISCONNECT packet of its own. A real 3.1.1 endpoint would just drop the socket.
            int reason = await seen.Task.WaitAsync(TimeSpan.FromSeconds(10), TestContext.Current.CancellationToken);
            Assert.Equal((int)MqttDisconnectReasonCode.ServerBusy, reason);

            await Assert.ThrowsAnyAsync<Exception>(() => provisioning);
        }

        /// <summary>
        /// An <see cref="IStubDeviceConnectionDropper"/> that reports a fixed set of devices as connected and records the
        /// drops asked of it, so that the stub's drop plumbing can be exercised without a broker or a device.
        /// </summary>
        private sealed class RecordingConnectionDropper(params string[] connectedDeviceIds) : IStubDeviceConnectionDropper
        {
            private readonly List<(string DeviceId, MqttDisconnectReasonCode ReasonCode, string? ReasonString)> _drops = new();

            public IReadOnlyList<(string DeviceId, MqttDisconnectReasonCode ReasonCode, string? ReasonString)> Drops
            {
                get
                {
                    lock (_drops)
                    {
                        return [.. _drops];
                    }
                }
            }

            public Task<IReadOnlyList<string>> GetConnectedDeviceIdsAsync(CancellationToken cancellationToken = default)
            {
                return Task.FromResult<IReadOnlyList<string>>(connectedDeviceIds);
            }

            public Task<bool> DropDeviceConnectionAsync(
                string deviceId,
                MqttDisconnectReasonCode reasonCode,
                string? reasonString = null,
                CancellationToken cancellationToken = default)
            {
                if (!connectedDeviceIds.Contains(deviceId, StringComparer.Ordinal))
                {
                    return Task.FromResult(false);
                }

                lock (_drops)
                {
                    _drops.Add((deviceId, reasonCode, reasonString));
                }

                return Task.FromResult(true);
            }
        }

    }
}
