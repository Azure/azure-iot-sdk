using Microsoft.Azure.Devices.Client.IntegrationTests.Gen2;
using Microsoft.Azure.Devices.Client.IntegrationTests.Unified;
using Microsoft.Azure.Devices.Client.Models;
using Microsoft.Azure.Devices.Client.Models.Twin;
using Microsoft.Azure.Devices.Client.Unified.Connection;
using Microsoft.Azure.Devices.Provisioning.Service;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using Xunit;

namespace Microsoft.Azure.Devices.Client.IntegrationTests
{
    public class Setup
    {
        public static string IotHubConnectionString { get; set; } = Environment.GetEnvironmentVariable("IOTHUB_CONNECTION_STRING") ?? throw new ArgumentException("Missing env var");

        public static string DpsConnectionString { get; set; } = Environment.GetEnvironmentVariable("IOT_DPS_CONNECTION_STRING") ?? throw new ArgumentException("Missing env var");

        public static string DpsIdScope { get; set; } = Environment.GetEnvironmentVariable("IOT_DPS_ID_SCOPE") ?? throw new ArgumentException("Missing env var");

        public const string TestCertificatesPassword = "some fake password";

        public static ServiceClient GetIotHubServiceClient() => ServiceClient.CreateFromConnectionString(IotHubConnectionString);

        public static RegistryManager GetIotHubRegistryManager() => RegistryManager.CreateFromConnectionString(IotHubConnectionString);

        public static ProvisioningServiceClient GetDpsHubServiceClient() => ProvisioningServiceClient.CreateFromConnectionString(DpsConnectionString);

        public static string GetIotHubHostName()
        {
            string[] connectionStringKeyValuePairs = IotHubConnectionString.Split(";");
            foreach (string connectionStringKeyValuePair in connectionStringKeyValuePairs)
            {
                string[] keyAndValue = connectionStringKeyValuePair.Split("=");
                if (keyAndValue[0].Equals("HostName"))
                {
                    return keyAndValue[1];
                }
            }

            throw new Exception("Malformed IoT hub connection string");
        }

        public const int TestTimeoutMilliseconds = 60 * 1000;

        public static Task<Gen2DeviceTestContext> CreateConnectedGen2ConnectionClientAsync(DeviceTwin? initialTwin, CancellationToken cancellationToken = default)
        {
            Assert.Skip("No test infrastructure setup for Gen2 client testing yet.");
            return Task.FromResult((Gen2DeviceTestContext)null);
        }

        public static async Task<UnifiedDeviceTestContext> CreateConnectedUnifiedConnectionClientAsync(bool testAgainstClassicHub, CancellationToken cancellationToken = default)
        {
            if (!testAgainstClassicHub)
            {
                Assert.Skip("No AEG hub to test against yet");
            }

            ServiceClient iotHubServiceClient = ServiceClient.CreateFromConnectionString(IotHubConnectionString);
            ProvisioningServiceClient provisioningServiceClient = ProvisioningServiceClient.CreateFromConnectionString(DpsConnectionString);

            string deviceId = Guid.NewGuid().ToString();
            string registrationId = deviceId;
            string certId = Guid.NewGuid().ToString();
            string certPath = $"./{certId}.cer";
            string pfxPath = $"./{certId}.pfx";
            CreateTestCertificates(pfxPath, certPath, deviceId);

            X509Certificate2 certificate = X509CertificateLoader.LoadCertificateFromFile(certPath);
            X509Certificate2 pfx = X509CertificateLoader.LoadPkcs12FromFile(pfxPath, TestCertificatesPassword);

            // Create individual enrollment for the test device to provision from
            Attestation attestation = X509Attestation.CreateFromClientCertificates(certificate);
            IndividualEnrollment individualEnrollment = new(registrationId, attestation);
            individualEnrollment = await provisioningServiceClient.CreateOrUpdateIndividualEnrollmentAsync(individualEnrollment, cancellationToken);

            X509AuthenticationProvider x509AuthenticationProvider = new(pfx);

            ConnectionClient connectionClient = new();
            ProvisioningSettings provisioningSettings = new(DpsIdScope);

            ConnectionContext connectionContext = await RetryAroundAuthorizationAsync<ConnectionContext>(
                async () => await connectionClient.ProvisionAndConnectAsync(provisioningSettings, x509AuthenticationProvider, cancellationToken: cancellationToken),
                cancellationToken);

            return new UnifiedDeviceTestContext()
            { 
                ConnectionClient = connectionClient,
                ConnectionContext = connectionContext!,
                AuthenticationProvider = x509AuthenticationProvider,
            };
        }

        public static async Task<UnifiedDeviceTestContext> CreateConnectedUnifiedConnectionClientWithCertificateSigningAsync(bool testAgainstClassicHub, CancellationToken cancellationToken = default)
        {
            if (!testAgainstClassicHub)
            {
                Assert.Skip("No AEG hub to test against yet");
            }

            ServiceClient iotHubServiceClient = ServiceClient.CreateFromConnectionString(IotHubConnectionString);
            ProvisioningServiceClient provisioningServiceClient = ProvisioningServiceClient.CreateFromConnectionString(DpsConnectionString);

            string registrationId = Environment.GetEnvironmentVariable("IOT_DPS_INDIVIDUAL_REGISTRATION_ID") ?? throw new Exception("TODO");
            string deviceId = registrationId;
            string certificatePem = Environment.GetEnvironmentVariable("IOT_DPS_INDIVIDUAL_X509_CERTIFICATE") ?? throw new Exception("TODO");
            string pfxPem = Environment.GetEnvironmentVariable("IOT_DPS_INDIVIDUAL_X509_KEY") ?? throw new Exception("TODO");

            Assert.False(string.IsNullOrWhiteSpace(certificatePem));
            Assert.False(string.IsNullOrWhiteSpace(pfxPem));

            byte[] certificateBytes = Convert.FromBase64String(certificatePem);
            byte[] pfxBytes = Convert.FromBase64String(pfxPem);

            X509Certificate2 certificate = X509CertificateLoader.LoadCertificate(certificateBytes);
            X509Certificate2 pfx = X509CertificateLoader.LoadPkcs12(pfxBytes, null);

            // Create individual enrollment for the test device to provision from
            Attestation attestation = X509Attestation.CreateFromClientCertificates(certificate);
            IndividualEnrollment individualEnrollment = new(registrationId, attestation);
            individualEnrollment = await provisioningServiceClient.CreateOrUpdateIndividualEnrollmentAsync(individualEnrollment, cancellationToken);

            X509AuthenticationProvider x509AuthenticationProvider = new(pfx);

            var (csrBase64, privateKey) = GenerateCsr(registrationId, CsrAlgorithm.RSA);

            ConnectionClient connectionClient = new();
            ProvisioningSettings provisioningSettings = new(DpsIdScope)
            {
                ProvisioningCertificateSigningRequest = csrBase64,
            };

            ConnectionContext connectionContext = await RetryAroundAuthorizationAsync<ConnectionContext>(
                async () => await connectionClient.ProvisionAndConnectAsync(provisioningSettings, x509AuthenticationProvider, cancellationToken: cancellationToken), 
                cancellationToken);

            return new UnifiedDeviceTestContext()
            {
                ConnectionClient = connectionClient,
                ConnectionContext = connectionContext!,
                PrivateKeyPem = pfxPem,
                AuthenticationProvider = x509AuthenticationProvider,
            };
        }

        public static void CreateTestCertificates(string pfxPath, string certPath, string deviceId)
        {
            var ecdsa = ECDsa.Create(); // generate asymmetric key pair
            var rsa = RSA.Create();
            var req = new CertificateRequest($"cn={deviceId}, O=TEST, C=US", rsa, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);
            var cert = req.CreateSelfSigned(DateTimeOffset.Now, DateTimeOffset.Now.AddHours(1));

            // Create PFX (PKCS #12) with private key
            File.WriteAllBytes(pfxPath, cert.Export(X509ContentType.Pfx, TestCertificatesPassword));

            // Create Base 64 encoded CER (public key only)
            File.WriteAllText(certPath,
                "-----BEGIN CERTIFICATE-----\r\n"
                + Convert.ToBase64String(cert.Export(X509ContentType.Cert), Base64FormattingOptions.InsertLineBreaks)
                + "\r\n-----END CERTIFICATE-----");
        }

        public enum CsrAlgorithm
        { 
            ECC,
            RSA,
        }

        // This basic retry logic covers the issue where a device is created on the Hub side, but it still 
        // rejects the connection for authorization reasons. Usually, after a few seconds, the device is ready to 
        // authorize the newly created device.
        public static async Task<T> RetryAroundAuthorizationAsync<T>(Func<Task<T>> taskToRetry, CancellationToken cancellationToken)
        {
            while (true)
            {
                try
                {
                    return await taskToRetry.Invoke();
                }
                catch (Exception e)
                {
                    if (e.Message.Contains("NotAuthorized"))
                    {
                        await Task.Delay(TimeSpan.FromSeconds(1), cancellationToken);
                    }
                    else
                    {
                        throw;
                    }
                }
            }
        }


        // This basic retry logic covers the issue where a device is created on the Hub side, but it still 
        // rejects the connection for authorization reasons. Usually, after a few seconds, the device is ready to 
        // authorize the newly created device.
        public static async Task RetryAroundAuthorizationAsync(Func<Task> taskToRetry, CancellationToken cancellationToken)
        {
            while (true)
            {
                try
                {
                    await taskToRetry.Invoke();
                    return;
                }
                catch (Exception e)
                {
                    if (e.Message.Contains("NotAuthorized"))
                    {
                        await Task.Delay(TimeSpan.FromSeconds(1), cancellationToken);
                    }
                    else
                    {
                        throw;
                    }
                }
            }
        }

        public static (string csrBase64, AsymmetricAlgorithm privateKey) GenerateCsr(string registrationId, CsrAlgorithm csrAlgorithm)
        {
            if (csrAlgorithm == CsrAlgorithm.ECC)
            {
                var ecdsa = ECDsa.Create(ECCurve.NamedCurves.nistP256);
                var request = new CertificateRequest(
                    $"CN={registrationId}",
                    ecdsa,
                    HashAlgorithmName.SHA256);

                byte[] csrDer = request.CreateSigningRequest();
                return (Convert.ToBase64String(csrDer), ecdsa);
            }
            else
            {
                var rsa = RSA.Create(2048);
                var request = new CertificateRequest(
                    $"CN={registrationId}",
                    rsa,
                    HashAlgorithmName.SHA256,
                    RSASignaturePadding.Pkcs1);

                byte[] csrDer = request.CreateSigningRequest();
                return (Convert.ToBase64String(csrDer), rsa);
            }
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
