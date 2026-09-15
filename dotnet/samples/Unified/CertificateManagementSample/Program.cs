using Microsoft.Azure.Devices.Client;
using Microsoft.Azure.Devices.Client.Models;
using Microsoft.Azure.Devices.Client.Models.CertificateManagement;
using Microsoft.Azure.Devices.Client.Unified.Connection;
using SetupSampleDevice;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using static Microsoft.Azure.Devices.Client.IntegrationTests.CertificateUtilities;

internal class Program
{
    public static async Task Main(string[] args)
    {
        string deviceId = SampleConstants.LoadDeviceId();
        string registrationId = deviceId;
        string idScope = SampleConstants.LoadIdScope();
        X509AuthenticationProvider authentication = SampleConstants.LoadAuthenticationProvider();

        // Create initial certificate signing request for DPS to fulfill while provisioning
        var (csrBase64, privateKey) = GenerateCsrAndPrivateKey(registrationId, CsrAlgorithm.RSA);

        using ConnectionClient connectionClient = new()
        {
            // Setup callback to handle constructing X509AuthenticationProvider each time a CSR completes
            HandleCertificateSigningCompleteAsync = (IssuedCertificates) =>
            {
                // Convert to PEM and save
                string pemChain = ConvertToPem(IssuedCertificates);

                using X509Certificate2 deviceCertTemp = CreateCertificateWithPrivateKey(IssuedCertificates, privateKey);

                // Export and reimport with Exportable flag
                byte[] pfxBytes = deviceCertTemp.Export(X509ContentType.Pfx);
                return Task.FromResult(new X509AuthenticationProvider(X509CertificateLoader.LoadPkcs12(pfxBytes, (string?)null, X509KeyStorageFlags.Exportable)));
            }
        };

        // Provision and connect to IoT hub using the certificates signed by DPS. Save those certificates signed by DPS locally
        ProvisioningSettings provisioningSettings = new(idScope)
        {
            CertificateSigningRequest = new(privateKey, csrBase64),
        };

        // Provision the device using the boot certificates. Once provisioned, the device will connect to IoT hub using the operational certificates
        ConnectionContext connectionContext = await connectionClient.ProvisionAndConnectAsync(provisioningSettings, authentication);

        // Create a new certificate signing request to send to IoT Hub this time
        csrBase64 = GenerateCsrWithPrivateKey(registrationId, privateKey);
        var certificateSigningRequest = new IotHubCertificateSigningRequest(registrationId, csrBase64, null, "*");
        CertificateSigningOperation pendingCsr = await connectionClient.SendCertificateSigningRequestAsync(certificateSigningRequest);

        try
        {
            await pendingCsr.Accepted;
            CertificateSigningResponse certificateSigningResponse = await pendingCsr.Completed;
        }
        catch (CertificateSigningRequestFailedException ex)
        {
            Console.WriteLine($"Certificate signing request failed: {ex.Error.Message}");
        }

        // At this point, the certificate signing request has completed and the connection client has already swapped the underlying authentication provider to use
        // the newly signed certificates. The next time that the connection client needs to reconnect to IoT hub, it will use these new certificates. No further action
        // is needed from the application layer for this to take effect.
    }
}