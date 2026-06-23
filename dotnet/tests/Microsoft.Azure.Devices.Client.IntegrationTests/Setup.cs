using Microsoft.Azure.Devices.Client.CertificateManagement;
using Microsoft.Azure.Devices.Provisioning.Service;
using System.Reflection.Metadata;
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

        private const string _testCertificatesPassword = "some fake password";

        public static ServiceClient GetIotHubServiceClient() => ServiceClient.CreateFromConnectionString(IotHubConnectionString);

        public static RegistryManager GetIotHubRegistryManager() => RegistryManager.CreateFromConnectionString(IotHubConnectionString);

        public static ProvisioningServiceClient GetDpsHubServiceClient() => ProvisioningServiceClient.CreateFromConnectionString(DpsConnectionString);

        public const int TestTimeoutMilliseconds = 60 * 1000;

        public static async Task<TestConnectionClient> CreateConnectedConnectionClientAsync(bool testAgainstClassicHub, CancellationToken cancellationToken = default)
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
            X509Certificate2 pfx = X509CertificateLoader.LoadPkcs12FromFile(pfxPath, _testCertificatesPassword);

            // Create individual enrollment for the test device to provision from
            Attestation attestation = X509Attestation.CreateFromClientCertificates(certificate);
            IndividualEnrollment individualEnrollment = new(registrationId, attestation);
            individualEnrollment = await provisioningServiceClient.CreateOrUpdateIndividualEnrollmentAsync(individualEnrollment, cancellationToken);

            X509AuthenticationProvider x509AuthenticationProvider = new(pfx);

            ConnectionClient connectionClient = new();
            ProvisioningSettings provisioningSettings = new(DpsIdScope);

            //TODO seeing some Unauthorized errors likely because the enrollment was just created and service isn't ready for the connection
            // yet. adding some basic retry to cover that
            bool connected = false;
            int retryCount = 0;
            ConnectionContext? connectionContext = null;
            while (!connected)
            {
                try
                {
                    connectionContext = await connectionClient.ProvisionAndConnectAsync(provisioningSettings, x509AuthenticationProvider, cancellationToken: cancellationToken);
                    connected = true;
                }
                catch (Exception e)
                {
                    if (e.Message.Contains("NotAuthorized"))
                    {
                        retryCount++;
                        await Task.Delay(TimeSpan.FromSeconds(1));

                        if (retryCount > 4)
                        {
                            throw;
                        }
                    }
                    else
                    {
                        throw;
                    }
                }

            }

            return new TestConnectionClient()
            { 
                ConnectionClient = connectionClient,
                ConnectionContext = connectionContext!,
            };
        }

        public static async Task<TestConnectionClient> CreateConnectedConnectionClientWithCertificateSigningAsync(bool testAgainstClassicHub, CancellationToken cancellationToken = default)
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

            //TODO seeing some Unauthorized errors likely because the enrollment was just created and service isn't ready for the connection
            // yet. adding some basic retry to cover that
            bool connected = false;
            int retryCount = 0;
            ConnectionContext? connectionContext = null;
            while (!connected)
            {
                try
                {
                    connectionContext = await connectionClient.ProvisionAndConnectAsync(provisioningSettings, x509AuthenticationProvider, cancellationToken: cancellationToken);
                    connected = true;
                }
                catch (Exception e)
                {
                    if (e.Message.Contains("NotAuthorized"))
                    {
                        retryCount++;
                        await Task.Delay(TimeSpan.FromSeconds(1));

                        if (retryCount > 4)
                        {
                            throw;
                        }
                    }
                    else
                    {
                        throw;
                    }
                }

            }

            return new TestConnectionClient()
            {
                ConnectionClient = connectionClient,
                ConnectionContext = connectionContext!,
                PrivateKeyPem = pfxPem,
            };
        }

        public static void CreateTestCertificates(string pfxPath, string certPath, string deviceId)
        {
            var ecdsa = ECDsa.Create(); // generate asymmetric key pair
            var rsa = RSA.Create();
            var req = new CertificateRequest($"cn={deviceId}, O=TEST, C=US", rsa, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);
            var cert = req.CreateSelfSigned(DateTimeOffset.Now, DateTimeOffset.Now.AddHours(1));

            // Create PFX (PKCS #12) with private key
            File.WriteAllBytes(pfxPath, cert.Export(X509ContentType.Pfx, _testCertificatesPassword));

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
