using System;
using System.Collections.Generic;
using System.Text;

namespace Microsoft.Azure.Devices.Client.IntegrationTests
{
    public class TestConnectionClient : IAsyncDisposable
    {
        public ConnectionClient ConnectionClient { get; set; }

        public ConnectionContext ConnectionContext { get; set; }

        public X509AuthenticationProvider AuthenticationProvider { get; set; }

        public string PrivateKeyPem { get; set; }

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
