using System;
using System.Collections.Generic;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using Xunit;
using static Microsoft.ApplicationInsights.MetricDimensionNames.TelemetryContext;
using static Microsoft.Azure.Amqp.Serialization.SerializableType;

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
                IsAzureEventGrid = !testAgainstClassicHub,
                IotHubHostName = Setup.GetIotHubHostName(),
            };

            ConnectionClient connectionClient = new();

            // This basic retry logic covers the issue where a device is created on the Hub side, but it still 
            // rejects the connection for authorization reasons. Usually, after a few seconds, the device is ready to 
            // authorize the newly created device.
             await Setup.RetryAroundAuthorizationAsync<Twin.Twin>(
                async () => await connectionClient.ConnectAsync(connectionContext, new X509AuthenticationProvider(pfx), null, TestContext.Current.CancellationToken),
                TestContext.Current.CancellationToken);
        }
    }
}
