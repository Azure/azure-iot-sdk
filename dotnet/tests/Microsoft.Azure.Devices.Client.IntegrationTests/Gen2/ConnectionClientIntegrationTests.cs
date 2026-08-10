using Microsoft.Azure.Devices.Client.Gen2.Connection;
using System.Security.Cryptography.X509Certificates;
using Xunit;

namespace Microsoft.Azure.Devices.Client.IntegrationTests.Gen2
{
    public class ConnectionClientIntegrationTests
    {
        [Fact(Timeout = Setup.TestTimeoutMilliseconds)]
        public async Task CanConnectDirectlyToIotHub()
        {
            Assert.Skip("No AEG hub to test against yet");

            string deviceId = Guid.NewGuid().ToString();
            string certId = Guid.NewGuid().ToString();
            string certPath = $"./{certId}.cer";
            string pfxPath = $"./{certId}.pfx";
            Setup.CreateTestCertificates(pfxPath, certPath, deviceId);

            X509Certificate2 certificate = X509CertificateLoader.LoadCertificateFromFile(certPath);
            X509Certificate2 pfx = X509CertificateLoader.LoadPkcs12FromFile(pfxPath, Setup.TestCertificatesPassword);

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

            await Setup.GetGen2IotHubRegistryManager().AddDeviceAsync(device, TestContext.Current.CancellationToken);

            ConnectionContext connectionContext = new()
            {
                DeviceId = deviceId,
                IsAzureEventGrid = true,
                IotHubHostName = Setup.GetGen2IotHubHostName(),
            };

            ConnectionClient connectionClient = new();

            // This basic retry logic covers the issue where a device is created on the Hub side, but it still 
            // rejects the connection for authorization reasons. Usually, after a few seconds, the device is ready to 
            // authorize the newly created device.
            await Setup.RetryAroundAuthorizationAsync(
               async () => await connectionClient.ConnectAsync(connectionContext, new X509AuthenticationProvider(pfx), cancellationToken: TestContext.Current.CancellationToken),
               TestContext.Current.CancellationToken);
        }
    }
}
