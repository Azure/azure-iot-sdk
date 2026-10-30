// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Authentication;
using Microsoft.Azure.Iot.Device.MQTTv5.Connection;
using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Devices.Common.Exceptions;
using Microsoft.Azure.Devices.Provisioning.Service;
using System.Net;

namespace Microsoft.Azure.Iot.Device.IntegrationTests.MQTTv5
{
    public class MQTTv5DeviceTestContext : IAsyncDisposable
    {
        public required ConnectionClient ConnectionClient { get; set; }

        public required string DeviceId { get; set; }

        public required IotHubConnectionType ConnectionProfile { get; set; }

        public required X509AuthenticationProvider AuthenticationProvider { get; set; }

        public string? PrivateKeyPem { get; set; }

        // Keep the original certificate-bearing enrollment; DPS GET responses contain certificate metadata only.
        public IndividualEnrollment? IndividualEnrollment { get; set; }

        public async ValueTask DisposeAsync()
        {
            try
            {
                // Disconnect before deleting the device identity. Deleting it while the connection is still open makes
                // IoT hub drop that connection as an identity fault, and this client responds to that fault by
                // re-provisioning and connecting again. That recovery races this teardown's disconnect, so the
                // connection is closed first to make sure there is nothing left for the hub to fault.
                try
                {
                    await ConnectionClient.DisconnectAsync();
                }
                finally
                {
                    ConnectionClient.Dispose();
                }
            }
            finally
            {
                try
                {
                    using var registryManager = Setup.GetMQTTv5IotHubRegistryManager();
                    await registryManager.RemoveDeviceAsync(DeviceId);
                }
                catch (DeviceNotFoundException)
                {
                    // Recovery tests may fail while the device identity is deliberately absent.
                }
                finally
                {
                    if (IndividualEnrollment != null)
                    {
                        using var provisioningServiceClient = Setup.GetDpsHubServiceClient(testAgainstClassicHub: false);
                        try
                        {
                            await provisioningServiceClient.DeleteIndividualEnrollmentAsync(IndividualEnrollment.RegistrationId);
                        }
                        catch (ProvisioningServiceClientHttpException exception) when (exception.StatusCode == HttpStatusCode.NotFound)
                        {
                            // The enrollment may still be absent if recovery failed before it was restored.
                        }
                    }
                }
            }
        }
    }
}
