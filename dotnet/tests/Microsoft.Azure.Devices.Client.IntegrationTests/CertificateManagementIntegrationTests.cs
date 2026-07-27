using Microsoft.Azure.Devices.Client.CertificateManagement;
using Microsoft.Azure.Devices.Provisioning.Service;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using Xunit;
using Xunit.Sdk;
using static Microsoft.Azure.Devices.Client.IntegrationTests.Setup;

namespace Microsoft.Azure.Devices.Client.IntegrationTests
{
    public class CertificateManagementIntegrationTests
    {
        [Theory(Timeout = Setup.TestTimeoutMilliseconds)]
        [InlineData(true)]
        [InlineData(false)]
        public async Task TestCertificateManagementWithDpsAndHub(bool testAgainstClassicHub)
        {
            using CancellationTokenSource cts = new();
            cts.CancelAfter(Setup.TestTimeoutMilliseconds - 1000);

            if (!testAgainstClassicHub)
            {
                Assert.Skip("No AEG hub to test against yet");
            }

            ServiceClient iotHubServiceClient = ServiceClient.CreateFromConnectionString(IotHubConnectionString);
            ProvisioningServiceClient provisioningServiceClient = ProvisioningServiceClient.CreateFromConnectionString(DpsConnectionString);

            string registrationId = Setup.GetEnvVarOrThrow("IOT_DPS_INDIVIDUAL_REGISTRATION_ID");
            string deviceId = registrationId;
            string certificatePem = Setup.GetEnvVarOrThrow("IOT_DPS_INDIVIDUAL_X509_CERTIFICATE");
            string pfxPem = Setup.GetEnvVarOrThrow("IOT_DPS_INDIVIDUAL_X509_KEY");

            Assert.False(string.IsNullOrWhiteSpace(certificatePem));
            Assert.False(string.IsNullOrWhiteSpace(pfxPem));

            byte[] certificateBytes = Convert.FromBase64String(certificatePem);
            byte[] pfxBytes = Convert.FromBase64String(pfxPem);

            X509Certificate2 certificate = X509CertificateLoader.LoadCertificate(certificateBytes);
            X509Certificate2 pfx = X509CertificateLoader.LoadPkcs12(pfxBytes, null);

            // Create individual enrollment for the test device to provision from
            Attestation attestation = X509Attestation.CreateFromClientCertificates(certificate);
            IndividualEnrollment individualEnrollment = new(registrationId, attestation);
            individualEnrollment = await provisioningServiceClient.CreateOrUpdateIndividualEnrollmentAsync(individualEnrollment, TestContext.Current.CancellationToken);

            X509AuthenticationProvider x509AuthenticationProvider = new(pfx);

            var (csrBase64, privateKey) = GenerateCsr(registrationId, CsrAlgorithm.RSA);

            ConnectionClient connectionClient = new();
            ProvisioningSettings provisioningSettings = new(DpsIdScope)
            {
                ProvisioningCertificateSigningRequest = csrBase64,
            };

            ConnectionContext connectionContext = await RetryAroundAuthorizationAsync<ConnectionContext>(
                async () => await connectionClient.ProvisionAndConnectAsync(provisioningSettings, x509AuthenticationProvider, cancellationToken: TestContext.Current.CancellationToken),
                TestContext.Current.CancellationToken);

            await using TestConnectionClient testDeviceContext = new TestConnectionClient()
            {
                ConnectionClient = connectionClient,
                ConnectionContext = connectionContext!,
                PrivateKeyPem = pfxPem,
                AuthenticationProvider = x509AuthenticationProvider,
            };
            
            var (secondCsrBase64, secondPrivateKey) = Setup.GenerateCsr(testDeviceContext.ConnectionContext.DeviceId, Setup.CsrAlgorithm.RSA);
            var certificateSigningRequest = new CertificateSigningRequest(testDeviceContext.ConnectionContext.DeviceId, secondCsrBase64, null, "*");
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
            await connectionClient.ConnectAsync(testDeviceContext.ConnectionContext, newX509AuthenticationProvider, cancellationToken:cts.Token);

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

        private static AsymmetricAlgorithm LoadPrivateKeyFromPem(string keyPem)
        {
            // Try ECC first, then RSA
            if (keyPem.Contains("EC PRIVATE KEY") || keyPem.Contains("PRIVATE KEY"))
            {
                try
                {
                    var ecdsa = ECDsa.Create();
                    ecdsa.ImportFromPem(keyPem);
                    return ecdsa;
                }
                catch (CryptographicException)
                {
                    // Not an ECC key, try RSA
                }
            }

            var rsa = RSA.Create();
            rsa.ImportFromPem(keyPem);
            return rsa;
        }

    }
}
