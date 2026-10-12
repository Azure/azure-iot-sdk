// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Authentication;
using Xunit;

namespace Microsoft.Azure.Iot.Device.IntegrationTests.Unified
{
    public class SymmetricKeyAuthenticationIntegrationTests
    {
        [Fact(Timeout = Setup.TestTimeoutMilliseconds)]
        public async Task CanProvisionAndConnectWithSymmetricKeyIndividualEnrollment()
        {
            // The same key signs the DPS SAS token and, after provisioning, the IoT hub SAS token
            await using UnifiedDeviceTestContext testDeviceContext = await Setup.CreateConnectedUnifiedConnectionClientWithSymmetricKeyAsync(cancellationToken: TestContext.Current.CancellationToken);

            Assert.IsType<SymmetricKeyAuthenticationProvider>(testDeviceContext.AuthenticationProvider);
            Assert.False(string.IsNullOrEmpty(testDeviceContext.ConnectionContext.IotHubHostName));
            Assert.Equal(testDeviceContext.RegistrationId, testDeviceContext.ConnectionContext.DeviceId);
            Assert.Equal(
                testDeviceContext.ConnectionContext.IotHubHostName,
                testDeviceContext.ConnectionClient.GetCurrentConnectionContext()?.IotHubHostName);
        }
    }
}
