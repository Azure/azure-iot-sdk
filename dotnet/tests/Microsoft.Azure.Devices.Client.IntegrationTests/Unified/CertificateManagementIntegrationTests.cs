using Microsoft.Azure.Devices.Client.Models.CertificateManagement;
using Microsoft.Azure.Devices.Client.Unified.Connection;
using System.Security.Cryptography.X509Certificates;
using Xunit;

namespace Microsoft.Azure.Devices.Client.IntegrationTests.Unified
{
    public class CertificateManagementIntegrationTests
    {
        [Theory(Timeout = Setup.TestTimeoutMilliseconds, Skip = "Test infrastructure not setup yet")]
        [InlineData(true)]
        [InlineData(false)]
        public async Task TestCertificateManagementWithDpsAndHub(bool testAgainstClassicHub)
        {
            using CancellationTokenSource cts = new();
            cts.CancelAfter(Setup.TestTimeoutMilliseconds - 1000);

            await using UnifiedDeviceTestContext testDeviceContext = await Setup.CreateConnectedUnifiedConnectionClientWithCertificateSigningAsync(testAgainstClassicHub, cts.Token);
            ConnectionClient connectionClient = testDeviceContext.ConnectionClient;

            var (csrBase64, privateKey) = Setup.GenerateCsr(testDeviceContext.ConnectionContext.DeviceId, Setup.CsrAlgorithm.RSA);
            var certificateSigningRequest = new CertificateSigningRequest(testDeviceContext.ConnectionContext.DeviceId, csrBase64, null, "*");
            CertificateSigningOperation pendingCsr = await connectionClient.SendCertificateSigningRequestAsync(certificateSigningRequest, cts.Token);

            try
            {
                await pendingCsr.Accepted.WaitAsync(cts.Token);
            }
            catch (CertificateSigningRequestFailedException e)
            {
                Assert.Fail(e.Error.Message);
            }

            CertificateSigningResponse? csrResponse = null;
            try
            {
                csrResponse = await pendingCsr.Completed.WaitAsync(cts.Token);
            }
            catch (CertificateSigningRequestFailedException e)
            {
                Assert.Fail(e.Error.Message);
            }
            
            await connectionClient.DisconnectAsync(cts.Token);

            // Upon getting the newly signed certificate, disconnect from IoT Hub and then reconnect with that new certificate
            await connectionClient.DisconnectAsync(cts.Token);

            X509AuthenticationProvider newX509AuthenticationProvider = new(CreateX509CertificateFromKeyAndCert(CertificateListToPem(csrResponse.Certificates), testDeviceContext.PrivateKeyPem!));
            testDeviceContext.ConnectionContext.AuthenticationProvider = newX509AuthenticationProvider;
            await connectionClient.ConnectAsync(testDeviceContext.ConnectionContext, cancellationToken:cts.Token);

            await connectionClient.DisconnectAsync(cts.Token);
        }

        private static X509Certificate2 CreateX509CertificateFromKeyAndCert(string certificate, string key)
        {
            // Create X509Certificate2 from PEM
            using var cert = X509Certificate2.CreateFromPem(certificate, key);

            // Note: On Windows, we need to export and reimport to allow ephemeral key use
            byte[] certificateBytes = cert.Export(X509ContentType.Pfx);
            using var exportedCert = X509CertificateLoader.LoadCertificate(certificateBytes);

            return exportedCert;
        }

        private static string CertificateListToPem(IReadOnlyList<string> certList)
        {
            const string beginHeader = "-----BEGIN CERTIFICATE-----\r\n";
            const string endFooter = "\r\n-----END CERTIFICATE-----";
            string separator = endFooter + "\r\n" + beginHeader;
            return beginHeader + string.Join(separator, certList) + endFooter;
        }
    }
}
