using System;
using System.Collections.Generic;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using Xunit;
using static Microsoft.ApplicationInsights.MetricDimensionNames.TelemetryContext;

namespace Microsoft.Azure.Devices.Client.IntegrationTests
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

            await Setup.GetIotHubRegistryManager().AddDeviceAsync(device, TestContext.Current.CancellationToken);

            ConnectionContext connectionContext = new()
            {
                DeviceId = deviceId,
                IsAzureEventGrid = testAgainstClassicHub,
                IotHubHostName = Setup.GetIotHubHostName(),
            };

            ConnectionClient connectionClient = new();
            await connectionClient.ConnectAsync(connectionContext, new X509AuthenticationProvider(pfx), null, TestContext.Current.CancellationToken);
        }
    }
}
