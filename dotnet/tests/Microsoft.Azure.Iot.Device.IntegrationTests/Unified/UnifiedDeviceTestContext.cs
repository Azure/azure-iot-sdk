// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Unified.Connection;

namespace Microsoft.Azure.Iot.Device.IntegrationTests.Unified
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
