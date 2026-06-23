using Microsoft.Azure.Devices.Client;
using Microsoft.Azure.Devices.Client.CertificateManagement;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text;

internal class Program
{
    public const string PrivateKeyPath = "./PrivateKey.pem";
    public const string InitialSignedCertificatesPath = "./PublicCertificateChain.pem"; // The certificates signed by DPS during the initial device provisioning
    public const string RenewedSignedCertificatesPath = "./RenewedPublicCertificateChain.pem"; // The certificates signed by IoT Hub at some point after the device has been provisioned. Usually done when the current certificates are soon to expire

    public static async Task Main(string[] args)
    {
        using CancellationTokenSource cts = new CancellationTokenSource();
        cts.CancelAfter(TimeSpan.FromSeconds(20));

        string idScope = Environment.GetEnvironmentVariable("DPS_ID_SCOPE") ?? throw new Exception("");
        string registrationId = Environment.GetEnvironmentVariable("DPS_REGISTRATION_ID") ?? throw new Exception("");

        // These credentials are only used when provisioning. When connecting to IoT Hub, this sample uses the certificate that was signing by DPS during provisioning
        string pcks12CertificatePath = Environment.GetEnvironmentVariable("X509_CERTIFICATE_PATH") ?? throw new Exception("");
        string pcks12CertificatePassword = Environment.GetEnvironmentVariable("X509_CERTIFICATE_PASSWORD") ?? throw new Exception("");
        X509AuthenticationProvider authentication = new(X509CertificateLoader.LoadPkcs12FromFile(pcks12CertificatePath, pcks12CertificatePassword));

        ConnectionClient connectionClient = new();

        // Create initial certificate signing request for DPS to fulfill while provisioning
        var (csrBase64, privateKey) = GenerateCsr(registrationId, CsrAlgorithm.RSA);
        SavePrivateKey(privateKey, PrivateKeyPath);

        // Provision and connect to IoT hub using the certificates signed by DPS. Save those certificates signed by DPS locally
        ProvisioningSettings provisioningSettings = new(idScope)
        {
            ProvisioningCertificateSigningRequest = csrBase64,
        };
        ConnectionContext connectionContext = await connectionClient.ProvisionAndConnectAsync(provisioningSettings, authentication);
        File.WriteAllText(InitialSignedCertificatesPath, ConvertToPem(connectionContext.IssuedClientCertificates)); // Save the DPS-issued certificate locally

        // Create a new certificate signing request to send to IoT Hub this time
        (csrBase64, privateKey) = GenerateCsr(registrationId, CsrAlgorithm.RSA);
        var certificateSigningRequest = new CertificateSigningRequest(registrationId, csrBase64, null, "*");
        CertificateSigningOperation pendingCsr = await connectionClient.SendCertificateSigningRequestAsync(certificateSigningRequest);
        await pendingCsr.Accepted;
        CertificateSigningResponse certificateSigningResponse = await pendingCsr.Completed;

        // Save the newly signed certificates locally
        File.WriteAllText(RenewedSignedCertificatesPath, ConvertToPem(certificateSigningResponse.Certificates)); // Save the IoT hub-renewed issued certificate locally

        // Upon getting the newly signed certificate, disconnect from IoT Hub and then reconnect with that new certificate
        await connectionClient.DisconnectAsync();
        X509AuthenticationProvider newX509AuthenticationProvider = new(CreateX509CertificateFromKeyAndCert(RenewedSignedCertificatesPath, PrivateKeyPath));
        await connectionClient.ConnectAsync(connectionContext, newX509AuthenticationProvider);
    }

    private static string ConvertToPem(IReadOnlyList<string> issuedClientCertificates)
    {
        StringBuilder pemBuilder = new StringBuilder();
        foreach (string issuedClientCertificate in issuedClientCertificates)
        {
            pemBuilder.Append("-----BEGIN CERTIFICATE-----\r\n");
            pemBuilder.Append(issuedClientCertificate);
            pemBuilder.Append("\r\n");
            pemBuilder.Append("-----END CERTIFICATE-----\r\n");
        }

        return pemBuilder.ToString();
    }

    private static X509Certificate2 CreateX509CertificateFromKeyAndCert(string certPath, string keyPath)
    {
        // Load certificate and key
        string certPem = File.ReadAllText(certPath);
        string keyPem = File.ReadAllText(keyPath);

        // Create X509Certificate2 from PEM
        using var cert = X509Certificate2.CreateFromPem(certPem, keyPem);

        // Note: On Windows, we need to export and reimport to allow ephemeral key use
        using var exportedCert = new X509Certificate2(cert.Export(X509ContentType.Pfx));

        return exportedCert;
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

    public enum CsrAlgorithm
    {
        ECC,
        RSA,
    }

    private static void SavePrivateKey(AsymmetricAlgorithm privateKey, string path)
    {
        byte[] privateKeyBytes = privateKey switch
        {
            ECDsa ecdsa => ecdsa.ExportPkcs8PrivateKey(),
            RSA rsa => rsa.ExportPkcs8PrivateKey(),
            _ => throw new NotSupportedException($"Unsupported key type: {privateKey.GetType()}")
        };

        var sb = new StringBuilder();
        sb.AppendLine("-----BEGIN PRIVATE KEY-----");
        sb.AppendLine(Convert.ToBase64String(privateKeyBytes, Base64FormattingOptions.InsertLineBreaks));
        sb.AppendLine("-----END PRIVATE KEY-----");

        File.WriteAllText(path, sb.ToString());
    }
}