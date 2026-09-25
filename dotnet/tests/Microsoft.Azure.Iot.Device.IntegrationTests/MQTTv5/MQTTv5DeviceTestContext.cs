// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.MQTTv5.Connection;
using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Provisioning.Models;

namespace Microsoft.Azure.Iot.Device.IntegrationTests.MQTTv5
{
    public class MQTTv5DeviceTestContext : IAsyncDisposable
    {
        public required ConnectionClient ConnectionClient { get; set; }

        public required string DeviceId { get; set; }

        public required ConnectionProfile ConnectionProfile { get; set; }

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
                await Setup.GetMQTTv3IotHubRegistryManager().RemoveDeviceAsync(DeviceId);
            }
        }
    }
}
