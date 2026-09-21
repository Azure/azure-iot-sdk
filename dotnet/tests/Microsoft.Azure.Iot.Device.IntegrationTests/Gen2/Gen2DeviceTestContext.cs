// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Gen2.Connection;
using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Provisioning.Models;

namespace Microsoft.Azure.Iot.Device.IntegrationTests.Gen2
{
    public class Gen2DeviceTestContext : IAsyncDisposable
    {
        public required ConnectionClient ConnectionClient { get; set; }

        public required string DeviceId { get; set; }

        public required ConnectionProfile ConnectionProfile { get; set; }

        public required X509AuthenticationProvider AuthenticationProvider { get; set; }

        public string? PrivateKeyPem { get; set; }

        public async ValueTask DisposeAsync()
        {
            await Setup.GetGen1IotHubRegistryManager().RemoveDeviceAsync(DeviceId);

            await ConnectionClient.DisconnectAsync();

            ConnectionClient.Dispose();
        }
    }
}
