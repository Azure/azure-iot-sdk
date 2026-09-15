using Microsoft.Azure.Devices.Client.Provisioning.Models;
using Microsoft.Azure.Devices.Client.Gen2.Connection;
using Microsoft.Azure.Devices.Client.Gen2.Telemetry;
using Microsoft.Azure.Devices.Client.IntegrationTests.StubService;
using Microsoft.Azure.Devices.Client.Models;
using Microsoft.Azure.Devices.Client.Models.Telemetry;
using System.Text;
using Xunit;

namespace Microsoft.Azure.Devices.Client.IntegrationTests.Gen2
{
    /// <summary>
    /// Provisioning tests for the gen2 connection client that run entirely in process against the stub device
    /// provisioning service and the stub IoT hub. No cloud resources are involved.
    /// </summary>
    public class ProvisioningIntegrationTests
    {
        [Fact(Timeout = Setup.TestTimeoutMilliseconds)]
        public async Task CanProvisionAndConnectToStubIotHub()
        {
            CancellationToken cancellationToken = TestContext.Current.CancellationToken;

            await using StubServiceTestEnvironment environment =
                await StubServiceTestEnvironment.StartAsync(IotHubGeneration.Gen2, cancellationToken: cancellationToken);

            using ConnectionClient connectionClient = new(environment.CreateConnectionClientOptions());

            ConnectionContext connectionContext = await connectionClient.ProvisionAndConnectAsync(
                environment.CreateProvisioningSettings(),
                environment.CreateAuthenticationProvider(),
                cancellationToken: cancellationToken);

            Assert.Equal(environment.DeviceId, connectionContext.DeviceId);
            Assert.Equal(StubServiceTestEnvironment.HubHostName, connectionContext.IotHubHostName);
            Assert.Equal(ConnectionProfile.MqttV5, connectionContext.ConnectionProfile);

            // Completing the presence handshake proves the device connected with the protocol that only the Event Grid
            // based hub speaks.
            StubDeviceBirthEventArgs birth = await environment.Hub.WaitForDeviceBirthAsync(environment.DeviceId, cancellationToken);

            Assert.Equal(environment.DeviceId, birth.DeviceId);
        }

        [Fact(Timeout = Setup.TestTimeoutMilliseconds)]
        public async Task CanSendTelemetryAfterProvisioning()
        {
            CancellationToken cancellationToken = TestContext.Current.CancellationToken;

            await using StubServiceTestEnvironment environment =
                await StubServiceTestEnvironment.StartAsync(IotHubGeneration.Gen2, cancellationToken: cancellationToken);

            using ConnectionClient connectionClient = new(environment.CreateConnectionClientOptions());

            await connectionClient.ProvisionAndConnectAsync(
                environment.CreateProvisioningSettings(),
                environment.CreateAuthenticationProvider(),
                cancellationToken: cancellationToken);

            using TelemetryClient telemetryClient = new(connectionClient);

            TaskCompletionSource<StubTelemetryReceivedEventArgs> received = new();
            environment.Hub.TelemetryReceived += (_, args) => received.TrySetResult(args);

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
