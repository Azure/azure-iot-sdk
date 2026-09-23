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
        // 1. Open a handle to the device's hardware security module. This sample talks to a real PKCS#11 token
        //    (SoftHSM2), configured from the outputs of c/eng/setup-softhsm.sh - the same token the SDK's
        //    CertificateManagementIntegrationTests use. The private key never leaves the module: only public-key
        //    material and signing results cross the boundary.
        using IHardwareSecurityModule hsm = SoftHsmHardwareSecurityModule.Create();

        // 2. Wrap the HSM key handle in an RSA that forwards signing to the HSM. The private key material is never in
        //    managed memory; .NET's TLS stack signs the handshake by calling into this object.
        using RSA hsmBackedKey = new HsmBackedRsa(hsm);

        // Prove the delegation works: the signature below is produced inside the HSM and verifies against the public
        // key. This runs on every operating system.
        DemonstrateHsmSigning(hsmBackedKey);

        // 3. Bind the HSM-backed key to the device certificate. How this is done depends on the OS TLS stack:
        //
        //    - Windows (SChannel): a managed custom RSA cannot back a TLS certificate. Instead the key lives in the
        //      HSM's CNG Key Storage Provider and the certificate is loaded from the certificate store, already bound
        //      to that key. SChannel then calls the KSP to sign. See LoadWindowsHsmCertificateFromStore below.
        //
        //    - Linux / macOS (OpenSSL): the managed HSM-backed key can be attached to the certificate with
        //      CopyWithPrivateKey, and OpenSSL calls into it to sign. See CreateHsmBackedCertificate below.
        if (OperatingSystem.IsWindows())
        {
            Console.WriteLine();
            Console.WriteLine("On Windows, bind the HSM key through a CNG Key Storage Provider and load the certificate");
            Console.WriteLine("from the certificate store, then pass it to X509AuthenticationProvider.");

            string subjectName = Environment.GetEnvironmentVariable("SAMPLE_DEVICE_ID") ?? "hsm-sample-device";
            try
            {
                using X509Certificate2 storeCertificate = LoadWindowsHsmCertificateFromStore(subjectName);
                _ = new X509AuthenticationProvider(storeCertificate);
                Console.WriteLine($"Loaded HSM-backed certificate '{storeCertificate.Subject}' from the store.");
            }
            catch (InvalidOperationException ex)
            {
                Console.WriteLine(ex.Message);
                Console.WriteLine("Install the HSM vendor's KSP and provision the certificate into the store to run this path.");
            }

            return;
        }

        using X509Certificate2 deviceCertificate = CreateHsmBackedCertificate(hsmBackedKey);

        // 4. Build the authentication provider. The certificate carries a non-exportable, HSM-backed private key.
        //    The optional callbacks show custom server validation (for example certificate pinning) and client
        //    certificate selection. Neither callback touches the HSM-backed private key.
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

    private static void DemonstrateHsmSigning(RSA hsmBackedKey)
    {
        byte[] data = Encoding.UTF8.GetBytes("proof that the HSM performs the signature");

        // Signing routes through HsmBackedRsa into the HSM.
        byte[] signature = hsmBackedKey.SignData(data, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);

        // Verifying uses only the public key.
        using RSA publicKey = RSA.Create();
        publicKey.ImportParameters(hsmBackedKey.ExportParameters(includePrivateParameters: false));
        bool valid = publicKey.VerifyData(data, signature, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);

        Console.WriteLine($"Private key type    : {hsmBackedKey.GetType().Name} (delegates signing to the HSM)");
        Console.WriteLine($"HSM signature valid : {valid}");
    }

    // Linux / macOS: build a certificate whose private key is the HSM-backed key. The certificate is self-signed by
    // the HSM (proving the wiring), then the same key is attached with CopyWithPrivateKey, which keeps a reference to
    // the managed HSM-backed key rather than exporting it. In production this certificate would instead be the one
    // issued for the HSM's public key, for example via a CSR to DPS.
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

    // Windows: the certificate must be bound to the HSM key through a CNG Key Storage Provider, which happens outside
    // this process (the HSM vendor's KSP installs the certificate into the store with a non-exportable key handle).
    // This helper shows how to load it; SChannel then calls the KSP to sign during the handshake.
    private static X509Certificate2 LoadWindowsHsmCertificateFromStore(string subjectName)
    {
        using var store = new X509Store(StoreName.My, StoreLocation.CurrentUser);
        store.Open(OpenFlags.ReadOnly);

        X509Certificate2Collection matches = store.Certificates.Find(
            X509FindType.FindBySubjectName,
            subjectName,
            validOnly: false);

        if (matches.Count == 0)
        {
            throw new InvalidOperationException($"No certificate found in the store for subject '{subjectName}'.");
        }

        // The returned certificate references the HSM key via its CNG provider; no private key bytes are present.
        return matches[0];
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
