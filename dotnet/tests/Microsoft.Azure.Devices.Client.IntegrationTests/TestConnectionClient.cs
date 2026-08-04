using Microsoft.Azure.Devices.Client.Connection.Unified;
using System;
using System.Collections.Generic;
using System.Text;

namespace Microsoft.Azure.Devices.Client.IntegrationTests
{
    public class TestConnectionClient : IAsyncDisposable
    {
        public required ConnectionClient ConnectionClient { get; set; }

        public required ConnectionContext ConnectionContext { get; set; }

        public required X509AuthenticationProvider AuthenticationProvider { get; set; }

        public string? PrivateKeyPem { get; set; }

        public async ValueTask DisposeAsync()
        {
            if (ConnectionContext.DeviceId != null)
            {
                await Setup.GetIotHubRegistryManager().RemoveDeviceAsync(ConnectionContext.DeviceId);
            }

            await ConnectionClient.DisconnectAsync();

            ConnectionClient.Dispose();
        }
    }
}
