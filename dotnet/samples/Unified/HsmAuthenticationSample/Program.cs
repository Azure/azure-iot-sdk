// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using HsmAuthenticationSample;
using Microsoft.Azure.Iot.Device;
using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Models.Telemetry;
using Microsoft.Azure.Iot.Device.Unified.Connection;
using Microsoft.Azure.Iot.Device.Unified.Telemetry;
using System.Net.Security;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text;

internal class Program
{
    private static async Task Main()
    {
        // This sample is SoftHSM-only and, like the SDK's SoftHSM integration tests, runs on Linux/macOS only: the
        // key is opened from the PKCS#11 token through OpenSSL's pkcs11 provider (RSAOpenSsl over SafeEvpPKeyHandle),
        // which does not exist on Windows. On Windows a device would instead surface the key through a CNG Key
        // Storage Provider and load the certificate from the certificate store; that path is out of scope here.
        if (OperatingSystem.IsWindows())
        {
            Console.WriteLine("This SoftHSM sample runs on Linux/macOS only: it uses the OpenSSL pkcs11 provider,");
            Console.WriteLine("which is unavailable on Windows (surface the key through a CNG Key Storage Provider there).");
            return;
        }

        await RunAsync();
    }

    // Linux / macOS: open the SoftHSM token key natively and use it for the whole flow. This is the same token the
    // SDK's SoftHSM integration tests use, configured from the outputs of c/eng/setup-softhsm.sh.
    private static async Task RunAsync()
    {
        RSA hsmBackedKey;
        try
        {
            // 1. Open the device's private key from the PKCS#11 token as a native OpenSSL key. The private key never
            //    leaves the token: this handle only references it, and the provider/engine signs inside the token.
            hsmBackedKey = SoftHsmKey.Open();
        }
        catch (InvalidOperationException ex)
        {
            Console.WriteLine(ex.Message);
            return;
        }

        using (hsmBackedKey)
        {
            // 2. Prove the delegation works: the signature below is produced inside the token and verifies against
            //    the public key. No private-key bytes are ever read into managed memory.
            DemonstrateHsmSigning(hsmBackedKey);

            // 3. Bind the token-held key to the device certificate. Because the key is a native OpenSSL key,
            //    CopyWithPrivateKey duplicates the handle by reference rather than exporting the private key.
            using X509Certificate2 deviceCertificate = CreateHsmBackedCertificate(hsmBackedKey);

            // 4. Build the authentication provider. The certificate carries a non-exportable, token-held private key.
            //    The optional callbacks show custom server validation (for example certificate pinning) and client
            //    certificate selection. Neither callback touches the token-held private key.
            var authentication = new X509AuthenticationProvider(
                deviceCertificate,
                certificateChain: null,
                remoteCertificateValidationCallback: ValidateServerCertificate,
                localCertificateSelectionCallback: SelectClientCertificate);

            string? idScope = Environment.GetEnvironmentVariable("DPS_ID_SCOPE");
            if (string.IsNullOrEmpty(idScope))
            {
                Console.WriteLine();
                Console.WriteLine("Set the DPS_ID_SCOPE environment variable, and enroll the device certificate's public");
                Console.WriteLine("key in DPS, to run the provisioning + telemetry portion of this sample.");
                return;
            }

            await ProvisionAndSendTelemetryAsync(idScope, authentication);
        }
    }

    private static void DemonstrateHsmSigning(RSA hsmBackedKey)
    {
        byte[] data = Encoding.UTF8.GetBytes("proof that the HSM performs the signature");

        // Signing routes through the OpenSSL PKCS#11 provider into the token.
        byte[] signature = hsmBackedKey.SignData(data, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);

        // Verifying uses only the public key.
        using RSA publicKey = RSA.Create();
        publicKey.ImportParameters(hsmBackedKey.ExportParameters(includePrivateParameters: false));
        bool valid = publicKey.VerifyData(data, signature, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);

        Console.WriteLine($"Private key type    : {hsmBackedKey.GetType().Name} (signs inside the token)");
        Console.WriteLine($"HSM signature valid : {valid}");
    }

    // Linux / macOS: build a certificate whose private key is the token-held key. The certificate is self-signed by
    // the token (proving the wiring), then the same key is attached with CopyWithPrivateKey, which keeps a reference
    // to the token key rather than exporting it. In production this certificate would instead be the one issued for
    // the token's public key, for example via a CSR to DPS.
    private static X509Certificate2 CreateHsmBackedCertificate(RSA hsmBackedKey)
    {
        string deviceId = Environment.GetEnvironmentVariable("SAMPLE_DEVICE_ID") ?? "hsm-sample-device";

        var request = new CertificateRequest(
            $"CN={deviceId}",
            hsmBackedKey,
            HashAlgorithmName.SHA256,
            RSASignaturePadding.Pkcs1);

        var generator = X509SignatureGenerator.CreateForRSA(hsmBackedKey, RSASignaturePadding.Pkcs1);
        byte[] serialNumber = RandomNumberGenerator.GetBytes(16);
        serialNumber[0] &= 0x7F;

        DateTimeOffset now = DateTimeOffset.UtcNow;
        using X509Certificate2 publicCertificate = request.Create(
            request.SubjectName,
            generator,
            now.AddDays(-1),
            now.AddYears(1),
            serialNumber);

        return publicCertificate.CopyWithPrivateKey(hsmBackedKey);
    }

    // Validates the server (remote) certificate. Replace with certificate pinning or a private/enterprise root check
    // as required. This callback never touches the device's HSM-backed private key.
    private static bool ValidateServerCertificate(
        object sender,
        X509Certificate? certificate,
        X509Chain? chain,
        SslPolicyErrors sslPolicyErrors)
    {
        return sslPolicyErrors == SslPolicyErrors.None;
    }

    // Selects which client certificate to present during the handshake. Use this hook to pick a specific certificate
    // or to swap in a rotated one. The selected certificate still signs using its HSM-backed key.
    private static X509Certificate SelectClientCertificate(
        object sender,
        string targetHost,
        X509CertificateCollection localCertificates,
        X509Certificate? remoteCertificate,
        string[] acceptableIssuers)
    {
        return localCertificates[0];
    }

    private static async Task ProvisionAndSendTelemetryAsync(string idScope, X509AuthenticationProvider authentication)
    {
        using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(30));
        Console.CancelKeyPress += (_, eventArgs) =>
        {
            cts.Cancel();
            eventArgs.Cancel = true;
        };

        using var connectionClient = new ConnectionClient();
        var telemetryClient = new TelemetryClient(connectionClient);

        var provisioningSettings = new ProvisioningSettings(idScope);
        ConnectionContext context = await connectionClient.ProvisionAndConnectAsync(
            provisioningSettings,
            authentication,
            cancellationToken: cts.Token);
        Console.WriteLine($"Device {context.DeviceId} provisioned and connected using its HSM-backed certificate.");

        var telemetry = new DeviceToCloudTelemetry
        {
            Payload = Encoding.UTF8.GetBytes("Hello from an HSM-backed device!"),
            MessageId = Guid.NewGuid().ToString(),
        };

        await telemetryClient.SendTelemetryAsync(telemetry, cts.Token);
        Console.WriteLine($"Sent telemetry with message id {telemetry.MessageId}.");

        await connectionClient.DisconnectAsync();
    }
}
