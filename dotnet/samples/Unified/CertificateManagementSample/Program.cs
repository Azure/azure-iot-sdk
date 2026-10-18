// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Authentication;
using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Models.CertificateManagement;
using Microsoft.Azure.Iot.Device.Unified.Connection;
using SetupSampleDevice;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using static Microsoft.Azure.Iot.Device.IntegrationTests.CertificateUtilities;

internal class Program
{
    public static async Task Main(string[] args)
    {
        string deviceId = SampleConstants.LoadDeviceId();
        string registrationId = deviceId;
        string idScope = SampleConstants.LoadIdScope();
        // Create initial certificate signing request for DPS to fulfill while provisioning
        var (csrBase64, privateKey) = GenerateCsrAndPrivateKey(registrationId, CsrAlgorithm.RSA);

        // Pair any certificate issued for this CSR (by DPS or IoT hub) with its private key when connecting to IoT hub
        X509AuthenticationProvider authentication = SampleConstants.LoadAuthenticationProvider(privateKey);

        using ConnectionClient connectionClient = new();

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

        // At this point, the certificate signing request has completed and the connection client has recorded the newly signed certificates. The next time that
        // the connection client needs to reconnect to IoT hub, the authentication provider pairs them with the CSR private key and presents them. No further
        // action is needed from the application layer for this to take effect.
    }
}