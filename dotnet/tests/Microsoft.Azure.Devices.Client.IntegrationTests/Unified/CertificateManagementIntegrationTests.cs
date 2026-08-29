using Microsoft.Azure.Devices.Client.Models;
using Microsoft.Azure.Devices.Client.Models.CertificateManagement;
using Microsoft.Azure.Devices.Client.Provisioning;
using Microsoft.Azure.Devices.Client.Unified.Connection;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using Xunit;
using Xunit.Sdk;
using static Microsoft.Azure.Devices.Client.IntegrationTests.Setup;

namespace Microsoft.Azure.Devices.Client.IntegrationTests.Unified
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

            string registrationId = Environment.GetEnvironmentVariable("IOT_DPS_GROUP_X509_REGISTRATION_ID")
                ?? throw new InvalidOperationException("Missing IOT_DPS_GROUP_X509_REGISTRATION_ID environment variable.");
            string certificatePem = DecodeBase64EnvironmentVariable("IOT_DPS_GROUP_X509_CERTIFICATE");
            string privateKeyPem = DecodeBase64EnvironmentVariable("IOT_DPS_GROUP_X509_KEY");

            using X509Certificate2 deviceCertificate = CreateX509CertificateFromKeyAndCert(certificatePem, privateKeyPem);
            X509AuthenticationProvider x509AuthenticationProvider = new(deviceCertificate);

            // Create initial CSR to be processed by DPS
            var (csrBase64, privateKey) = GenerateCsr(registrationId, CsrAlgorithm.RSA);

            ConnectionClient connectionClient = new();
            ProvisioningSettings provisioningSettings = new(DpsIdScope)
            {
                CertificateSigningRequest = new(privateKey, csrBase64),
            };

            ConnectionContext connectionContext = await RetryAroundAuthorizationAsync<ConnectionContext>(
                async () => await connectionClient.ProvisionAndConnectAsync(provisioningSettings, x509AuthenticationProvider, cancellationToken: TestContext.Current.CancellationToken),
                TestContext.Current.CancellationToken);

            Assert.NotNull(connectionContext.IssuedClientCertificates);
            Assert.NotEmpty(connectionContext.IssuedClientCertificates);

            var (secondCsrBase64, secondPrivateKey) = Setup.GenerateCsr(connectionContext.DeviceId, Setup.CsrAlgorithm.RSA);
            var certificateSigningRequest = new CertificateSigningRequest(connectionContext.DeviceId, secondCsrBase64, null, "*");
            CertificateSigningOperation pendingCsr = await connectionClient.SendCertificateSigningRequestAsync(certificateSigningRequest, cts.Token);

            try
            {
                await pendingCsr.Accepted.WaitAsync(cts.Token);
            }
            catch (CertificateSigningRequestFailedException e)
            {
                Assert.Fail(e.Error.Message);
            }

            CertificateSigningResponse? hubCsrResponse = null;
            try
            {
                hubCsrResponse = await pendingCsr.Completed.WaitAsync(cts.Token);
            }
            catch (CertificateSigningRequestFailedException e)
            {
                Assert.Fail(e.Error.Message);
            }

            // Upon getting the newly signed certificate, disconnect from IoT Hub and then reconnect with that new certificate
            await connectionClient.DisconnectAsync(cts.Token);

            Assert.NotNull(hubCsrResponse.Certificates);
            Assert.NotEmpty(hubCsrResponse.Certificates);

            // Convert to PEM and save
            string pemChain = CertificateUtilities.ConvertToPem(hubCsrResponse.Certificates);

            using X509Certificate2 deviceCertTemp = CertificateUtilities.CreateCertificateWithPrivateKey(hubCsrResponse.Certificates, provisioningSettings.CertificateSigningRequest!.PrivateKey);

            // Export and reimport with Exportable flag
            byte[] pfxBytes = deviceCertTemp.Export(X509ContentType.Pfx);
            connectionContext.AuthenticationProvider = new(new X509Certificate2(pfxBytes, (string?)null, X509KeyStorageFlags.Exportable));

            await connectionClient.ConnectAsync(connectionContext, cancellationToken: cts.Token);

            await connectionClient.DisconnectAsync(cts.Token);
        }

        private static X509Certificate2 CreateX509CertificateFromKeyAndCert(string certificate, string key)
        {
            using X509Certificate2 cert = X509Certificate2.CreateFromPem(certificate, key);

            byte[] certificateBytes = cert.Export(X509ContentType.Pfx);
            return X509CertificateLoader.LoadPkcs12(certificateBytes, null, X509KeyStorageFlags.Exportable);
        }

        private static string DecodeBase64EnvironmentVariable(string variableName)
        {
            string encodedValue = Environment.GetEnvironmentVariable(variableName)
                ?? throw new InvalidOperationException($"Missing {variableName} environment variable.");

            return Encoding.UTF8.GetString(Convert.FromBase64String(encodedValue));
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