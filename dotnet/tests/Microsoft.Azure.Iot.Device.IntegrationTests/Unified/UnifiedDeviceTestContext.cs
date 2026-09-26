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
            try
            {
                // Disconnect before deleting the device identity. Deleting it while the connection is still open makes
                // IoT hub drop that connection as an identity fault, and this client responds to that fault by
                // re-provisioning and connecting again. That recovery races this teardown's disconnect, so the
                // connection is closed first to make sure there is nothing left for the hub to fault.
                await ConnectionClient.DisconnectAsync();

                ConnectionClient.Dispose();
            }
            finally
            {
                if (ConnectionContext.DeviceId != null)
                {
                    var registryManager = ConnectionContext.ConnectionProfile == Provisioning.Models.ConnectionProfile.MqttV5
                        ? Setup.GetMQTTv5IotHubRegistryManager()
                        : Setup.GetMQTTv3IotHubRegistryManager();
                    await registryManager.RemoveDeviceAsync(ConnectionContext.DeviceId);
                }
            }
        }
    }
}
