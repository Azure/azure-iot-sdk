using Microsoft.Azure.Devices.Client.Models;
using Microsoft.Azure.Devices.Client.Unified.Connection;

namespace Microsoft.Azure.Devices.Client.IntegrationTests.Unified
{
    public class UnifiedDeviceTestContext : IAsyncDisposable
    {
        public required ConnectionClient ConnectionClient { get; set; }

        public required ConnectionContext ConnectionContext { get; set; }

        public required X509AuthenticationProvider AuthenticationProvider { get; set; }

        public string? PrivateKeyPem { get; set; }

        public async ValueTask DisposeAsync()
        {
            if (ConnectionContext.DeviceId != null)
            {
                await Setup.GetGen1IotHubRegistryManager().RemoveDeviceAsync(ConnectionContext.DeviceId);
            }

            await ConnectionClient.DisconnectAsync();

            ConnectionClient.Dispose();
        }
    }
}
