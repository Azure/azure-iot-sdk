using Microsoft.Azure.Devices.Client.Gen2.Connection;
using Microsoft.Azure.Devices.Client.Models;

namespace Microsoft.Azure.Devices.Client.IntegrationTests.Gen2
{
    public class Gen2DeviceTestContext : IAsyncDisposable
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
