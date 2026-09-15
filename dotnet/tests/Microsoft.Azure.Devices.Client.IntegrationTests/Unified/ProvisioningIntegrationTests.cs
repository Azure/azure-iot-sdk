using Microsoft.Azure.Devices.Client.Provisioning.Models;
using Microsoft.Azure.Devices.Client.IntegrationTests.StubService;
using Microsoft.Azure.Devices.Client.Models;
using Microsoft.Azure.Devices.Client.Models.Telemetry;
using Microsoft.Azure.Devices.Client.Unified.Connection;
using Microsoft.Azure.Devices.Client.Unified.Telemetry;
using System.Text;
using Xunit;

namespace Microsoft.Azure.Devices.Client.IntegrationTests.Unified
{
    /// <summary>
    /// Provisioning tests for the unified connection client that run entirely in process against the stub device
    /// provisioning service and the stub IoT hub. No cloud resources are involved.
    /// </summary>
    /// <remarks>
    /// The unified client supports both generations of hub, and the generation it uses is decided by the provisioning
    /// result rather than by the caller. These tests cover both by configuring the stub DPS to assign the device to a
    /// gen1 hub in one case and a gen2 hub in the other.
    /// </remarks>
    public class ProvisioningIntegrationTests
    {
        [Theory(Timeout = Setup.TestTimeoutMilliseconds)]
        [InlineData(IotHubGeneration.Gen1)]
        [InlineData(IotHubGeneration.Gen2)]
        public async Task CanProvisionAndConnectToStubIotHub(IotHubGeneration generation)
        {
            CancellationToken cancellationToken = TestContext.Current.CancellationToken;

            await using StubServiceTestEnvironment environment =
                await StubServiceTestEnvironment.StartAsync(generation, cancellationToken: cancellationToken);

            using ConnectionClient connectionClient = new(environment.CreateConnectionClientOptions());

            ConnectionContext connectionContext = await connectionClient.ProvisionAndConnectAsync(
                environment.CreateProvisioningSettings(),
                environment.CreateAuthenticationProvider(),
                cancellationToken);

            Assert.Equal(environment.DeviceId, connectionContext.DeviceId);
            Assert.Equal(StubServiceTestEnvironment.HubHostName, connectionContext.IotHubHostName);

            // The generation the device ends up speaking comes from the provisioning result, not from the caller.
            Assert.Equal(generation == IotHubGeneration.Gen2, connectionContext.IsGen2Hub);
        }

        [Theory(Timeout = Setup.TestTimeoutMilliseconds)]
        [InlineData(IotHubGeneration.Gen1)]
        [InlineData(IotHubGeneration.Gen2)]
        public async Task CanSendTelemetryAfterProvisioning(IotHubGeneration generation)
        {
            CancellationToken cancellationToken = TestContext.Current.CancellationToken;

            await using StubServiceTestEnvironment environment =
                await StubServiceTestEnvironment.StartAsync(generation, cancellationToken: cancellationToken);

            using ConnectionClient connectionClient = new(environment.CreateConnectionClientOptions());

            await connectionClient.ProvisionAndConnectAsync(
                environment.CreateProvisioningSettings(),
                environment.CreateAuthenticationProvider(),
                cancellationToken);

            using TelemetryClient telemetryClient = new(connectionClient);

            TaskCompletionSource<StubTelemetryReceivedEventArgs> received = new();
            environment.Hub.TelemetryReceived += (_, args) => received.TrySetResult(args);

            // Reaching the stub on the topics of the assigned generation is what proves the device landed on the
            // protocol it was provisioned onto.
            await telemetryClient.SendTelemetryAsync(
                new DeviceToCloudTelemetry
                {
                    Payload = Encoding.UTF8.GetBytes("{\"temperature\":21}"),
                    ContentType = "application/json",
                },
                cancellationToken);

            StubTelemetryReceivedEventArgs telemetry = await received.Task.WaitAsync(cancellationToken);

            Assert.Equal(environment.DeviceId, telemetry.DeviceId);
            Assert.Equal("{\"temperature\":21}", Encoding.UTF8.GetString(telemetry.Payload));
        }
    }
}
