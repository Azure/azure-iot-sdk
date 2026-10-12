// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Devices.Provisioning.Service;
using Microsoft.Azure.Iot.Device.Authentication;
using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Unified.Connection;
using System.Security.Cryptography;
using Xunit;

namespace Microsoft.Azure.Iot.Device.IntegrationTests.Unified
{
    public class SymmetricKeyAuthenticationIntegrationTests
    {
        [Fact(Timeout = Setup.TestTimeoutMilliseconds)]
        public async Task CanProvisionAndConnectWithSymmetricKeyIndividualEnrollment()
        {
            CancellationToken cancellationToken = TestContext.Current.CancellationToken;

            string registrationId = Guid.NewGuid().ToString();
            string symmetricKey = Convert.ToBase64String(RandomNumberGenerator.GetBytes(32));

            using ProvisioningServiceClient provisioningServiceClient = Setup.GetDpsHubServiceClient();
            Attestation attestation = new SymmetricKeyAttestation(symmetricKey, symmetricKey);
            IndividualEnrollment individualEnrollment = new(registrationId, attestation);
            await provisioningServiceClient.CreateOrUpdateIndividualEnrollmentAsync(individualEnrollment, cancellationToken);

            ConnectionClient? connectionClient = null;
            ConnectionContext? connectionContext = null;
            try
            {
                SymmetricKeyAuthenticationProvider authentication = new(registrationId, Setup.DpsIdScope, symmetricKey);
                ProvisioningSettings provisioningSettings = new(Setup.DpsIdScope);
                connectionClient = new ConnectionClient();

                // The same key is used to sign the DPS SAS token and, after provisioning, the IoT hub SAS token
                connectionContext = await Setup.RetryAroundAuthorizationAsync<ConnectionContext>(
                    async () => await connectionClient.ProvisionAndConnectAsync(provisioningSettings, authentication, cancellationToken: cancellationToken),
                    cancellationToken);

                Assert.False(string.IsNullOrEmpty(connectionContext.IotHubHostName));
                Assert.Equal(registrationId, connectionContext.DeviceId);
                Assert.Equal(connectionContext.IotHubHostName, connectionClient.GetCurrentConnectionContext()?.IotHubHostName);
            }
            finally
            {
                try
                {
                    if (connectionClient != null)
                    {
                        // Disconnect before deleting the identity so the hub has no live connection to fault
                        await connectionClient.DisconnectAsync();
                        connectionClient.Dispose();
                    }
                }
                finally
                {
                    await CleanUpAsync(provisioningServiceClient, registrationId, connectionContext?.DeviceId);
                }
            }
        }

        private static async Task CleanUpAsync(ProvisioningServiceClient provisioningServiceClient, string registrationId, string? deviceId)
        {
            try
            {
                if (deviceId != null)
                {
                    using var registryManager = Setup.GetMQTTv3IotHubRegistryManager();
                    await registryManager.RemoveDeviceAsync(deviceId);
                }
            }
            catch
            {
                // The device identity may never have been created or may already be gone
            }

            try
            {
                await provisioningServiceClient.DeleteIndividualEnrollmentAsync(registrationId);
            }
            catch
            {
                // The enrollment may already be absent
            }
        }
    }
}
