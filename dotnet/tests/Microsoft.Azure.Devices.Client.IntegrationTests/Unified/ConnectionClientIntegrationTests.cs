using Microsoft.Azure.Devices.Client.Unified.Connection;
using System;
using System.Collections.Generic;
using System.Security.Cryptography.X509Certificates;
using Xunit;

namespace Microsoft.Azure.Devices.Client.IntegrationTests.Unified
{
    public class ConnectionClientIntegrationTests
    {
        [Theory(Timeout = Setup.TestTimeoutMilliseconds)]
        [InlineData(true)]
        [InlineData(false)]
        public async Task CanConnectDirectlyToIotHub(bool testAgainstClassicHub)
        {
            if (!testAgainstClassicHub)
            {
                Assert.Skip("No AEG hub to test against yet");
            }

            string deviceId = Guid.NewGuid().ToString();
            string certId = Guid.NewGuid().ToString();
            string certPath = $"./{certId}.cer";
            string pfxPath = $"./{certId}.pfx";
            Setup.CreateTestCertificates(pfxPath, certPath, deviceId);

            X509Certificate2 certificate = X509CertificateLoader.LoadCertificateFromFile(certPath);
            X509Certificate2 pfx = X509CertificateLoader.LoadPkcs12FromFile(pfxPath, Setup.TestCertificatesPassword);
            var authenticationProvider = new X509AuthenticationProvider(pfx);

            Device device = new(deviceId)
            {
                Authentication = new AuthenticationMechanism()
                {
                    X509Thumbprint = new()
                    {
                        PrimaryThumbprint = certificate.Thumbprint
                    }
                }
            };

            if (testAgainstClassicHub)
            {
                await Setup.GetGen1IotHubRegistryManager().AddDeviceAsync(device, TestContext.Current.CancellationToken);
            }
            else
            { 
                await Setup.GetGen2IotHubRegistryManager().AddDeviceAsync(device, TestContext.Current.CancellationToken);
            }

            ConnectionContext connectionContext = new()
            {
                DeviceId = deviceId,
                IsAzureEventGrid = !testAgainstClassicHub,
                IotHubHostName = testAgainstClassicHub ? Setup.GetGen1IotHubHostName() : Setup.GetGen2IotHubHostName(),
                AuthenticationProvider = authenticationProvider,
            };

            ConnectionClient connectionClient = new();

            // This basic retry logic covers the issue where a device is created on the Hub side, but it still 
            // rejects the connection for authorization reasons. Usually, after a few seconds, the device is ready to 
            // authorize the newly created device.
             await Setup.RetryAroundAuthorizationAsync(
                async () => await connectionClient.ConnectAsync(connectionContext, TestContext.Current.CancellationToken),
                TestContext.Current.CancellationToken);
        }
    }
}
