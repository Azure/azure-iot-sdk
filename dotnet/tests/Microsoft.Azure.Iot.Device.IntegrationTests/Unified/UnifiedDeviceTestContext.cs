// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Devices.Provisioning.Service;
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

        /// <summary>
        /// The DPS individual enrollment this device provisioned from. Caching the whole enrollment (rather than just
        /// its registration id) lets a test that deletes the enrollment mid-run restore the exact same enrollment
        /// afterwards, and lets this context clean the enrollment up when it is disposed.
        /// </summary>
        public required IndividualEnrollment IndividualEnrollment { get; set; }

        /// <summary>
        /// The DPS registration id of the <see cref="IndividualEnrollment"/> this device provisioned from.
        /// </summary>
        public string RegistrationId => IndividualEnrollment.RegistrationId;

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
                try
                {
                    if (ConnectionContext.DeviceId != null)
                    {
                        using var registryManager = Setup.GetMQTTv3IotHubRegistryManager();
                        await registryManager.RemoveDeviceAsync(ConnectionContext.DeviceId);
                    }
                }
                catch (Exception)
                { 
                    // Device identity may have already been deleted or the test may have failed to provision it. In either case,
                    // ignore failures
                }
                finally
                {
                    // Also remove the DPS enrollment this device provisioned from so that repeated test runs do not
                    // leave a growing pile of individual enrollments behind. This runs even if removing the device
                    // identity above failed, and a missing enrollment is treated as already cleaned up.
                    try
                    {
                        using var provisioningServiceClient = Setup.GetDpsHubServiceClient();
                        await provisioningServiceClient.DeleteIndividualEnrollmentAsync(RegistrationId);
                    }
                    catch
                    {
                        // The enrollment may already be absent (deleted by the test or never created).
                    }
                }
            }

            GC.SuppressFinalize(this);
        }
    }
}
