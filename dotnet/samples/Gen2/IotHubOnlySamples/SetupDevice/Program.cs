using Microsoft.Azure.Devices;
using System.Diagnostics;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using System.Text.Json;

internal class Program
{
    public static string IotHubConnectionString = "";
    public static string HostName = "";

    public static string TestCertificatesPassword = "Some dummy certificate password";

    // Run this to register a test device on your IoT hub using the v1 IoT hub service client. It also locally saves the device credentials such that the other project
    // can be run using those credentials
    private static async Task Main(string[] args)
    {
        RegistryManager iotHubRegistryManager = RegistryManager.CreateFromConnectionString(IotHubConnectionString);

        string deviceId = Guid.NewGuid().ToString();
        string certPath = "../../../../certificate.cer";
        string pfxPath = "../../../../certificate.pfx";
        CreateTestCertificates(pfxPath, certPath, deviceId);

        X509Certificate2 certificate = X509CertificateLoader.LoadCertificateFromFile(certPath);
        X509Certificate2 pfx = X509CertificateLoader.LoadPkcs12FromFile(pfxPath, TestCertificatesPassword);

        File.WriteAllText("../../../../deviceId.txt", deviceId);
        File.WriteAllText("../../../../hostname.txt", HostName);
        File.WriteAllText("../../../../thumbprint.txt", certificate.Thumbprint);
        Console.WriteLine("Device credentials saved in root of the 'Iot hub only samples' directory");
        
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

        await iotHubRegistryManager.AddDeviceAsync(device);

        Console.WriteLine("Registered device with id " + deviceId);
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
}