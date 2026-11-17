// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Devices;
using Microsoft.Azure.Devices.Provisioning.Service;
using System.Diagnostics;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using System.Text.Json;
using TpmAuthenticationSample;

namespace SetupSampleDevice;

internal class Program
{
    public static string DpsConnectionString = ""; // The connection string of your DPS
    public static string DpsIdScope = ""; // The Id scope of your DPS
    public static string IotHubHostName = ""; // The host name of the IoT hub linked to your DPS

    // Run this to register a test device on your IoT hub using the v1 IoT hub service client. It also locally saves the device credentials such that the other project
    // can be run using those credentials. Pass --tpm (the platform TPM) or --tpm-simulator (a TPM simulator listening on
    // 127.0.0.1:2321) to also register a device that attests with that TPM, for the TpmAuthenticationSample.
    private static async Task Main(string[] args)
    {
        ArgumentException.ThrowIfNullOrEmpty(DpsConnectionString);
        ArgumentException.ThrowIfNullOrEmpty(DpsIdScope);
        ArgumentException.ThrowIfNullOrEmpty(IotHubHostName);

        await RegisterSampleDeviceAndSaveCredentialsAsync();
        await RegisterSymmetricKeySampleDeviceAndSaveCredentialsAsync();

        if (args.Contains("--tpm") || args.Contains("--tpm-simulator"))
        {
            await RegisterTpmSampleDeviceAndSaveCredentialsAsync(useSimulator: args.Contains("--tpm-simulator"));
        }
    }

    public static async Task RegisterSampleDeviceAndSaveCredentialsAsync(CancellationToken cancellationToken = default)
    {
        ProvisioningServiceClient provisioningServiceClient = ProvisioningServiceClient.CreateFromConnectionString(DpsConnectionString);

        string deviceId = Guid.NewGuid().ToString();
        string registrationId = deviceId;

        CreateTestCertificates(deviceId);

        X509Certificate2 certificate = X509CertificateLoader.LoadCertificateFromFile(SampleConstants.CertificatePath);
        X509Certificate2 pfx = X509CertificateLoader.LoadPkcs12FromFile(SampleConstants.PfxPath, SampleConstants.TestCertificatesPassword);

        // Create individual enrollment for the test device to provision from
        Attestation attestation = X509Attestation.CreateFromClientCertificates(certificate);
        IndividualEnrollment individualEnrollment = new(registrationId, attestation);
        individualEnrollment = await provisioningServiceClient.CreateOrUpdateIndividualEnrollmentAsync(individualEnrollment, cancellationToken);

        SampleConstants.SaveDeviceId(deviceId);
        SampleConstants.SaveIdScope(DpsIdScope);
        SampleConstants.SaveIotHubHostName(IotHubHostName);

        Console.WriteLine($"Device with Id {deviceId} has been registered with DPS. That device's credentials saved in root of the samples directory");
    }

    // Registers a second sample device that authenticates with a symmetric key instead of an X.509 certificate. DPS needs a
    // separate enrollment for it because each individual enrollment has exactly one attestation mechanism.
    public static async Task RegisterSymmetricKeySampleDeviceAndSaveCredentialsAsync(CancellationToken cancellationToken = default)
    {
        using ProvisioningServiceClient provisioningServiceClient = ProvisioningServiceClient.CreateFromConnectionString(DpsConnectionString);

        string registrationId = Guid.NewGuid().ToString();
        byte[] symmetricKey = RandomNumberGenerator.GetBytes(32);

        // DPS takes the key Base64-encoded, while the device SDK takes the raw key bytes
        string encodedSymmetricKey = Convert.ToBase64String(symmetricKey);
        Attestation attestation = new SymmetricKeyAttestation(encodedSymmetricKey, encodedSymmetricKey);
        IndividualEnrollment individualEnrollment = new(registrationId, attestation);
        await provisioningServiceClient.CreateOrUpdateIndividualEnrollmentAsync(individualEnrollment, cancellationToken);

        SampleConstants.SaveSymmetricKeyRegistrationId(registrationId);
        SampleConstants.SaveSymmetricKey(symmetricKey);

        Console.WriteLine($"Device with registration Id {registrationId} has been registered with DPS using a symmetric key. That device's credentials saved in root of the samples directory");
    }

    // Registers a sample device that attests with a TPM. DPS identifies the device by its TPM's endorsement key, so this
    // must run on the device, against the same TPM (or TPM simulator) that the TpmAuthenticationSample will use. No
    // secret is saved: the device's key only ever exists inside its TPM.
    public static async Task RegisterTpmSampleDeviceAndSaveCredentialsAsync(bool useSimulator, CancellationToken cancellationToken = default)
    {
        using ProvisioningServiceClient provisioningServiceClient = ProvisioningServiceClient.CreateFromConnectionString(DpsConnectionString);

        string registrationId = Guid.NewGuid().ToString();

        string endorsementKey;
        using (TssTpmSecurityProvider tpm = useSimulator
            ? TssTpmSecurityProvider.CreateForSimulator()
            : TssTpmSecurityProvider.CreateForPlatformTpm())
        {
            endorsementKey = Convert.ToBase64String(await tpm.GetEndorsementKeyAsync(cancellationToken));

            // Any key a previous sample device was given is of no use to this new one.
            tpm.DeletePersistedKey();
        }

        IndividualEnrollment individualEnrollment = new(registrationId, new TpmAttestation(endorsementKey));
        await provisioningServiceClient.CreateOrUpdateIndividualEnrollmentAsync(individualEnrollment, cancellationToken);

        SampleConstants.SaveTpmRegistrationId(registrationId);

        Console.WriteLine($"Device with registration Id {registrationId} has been registered with DPS using its TPM's endorsement key. That device's registration Id saved in root of the samples directory");
    }

    public static void CreateTestCertificates(string deviceId)
    {
        var ecdsa = ECDsa.Create(); // generate asymmetric key pair
        var rsa = RSA.Create();
        var req = new CertificateRequest($"cn={deviceId}, O=TEST, C=US", rsa, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);
        var cert = req.CreateSelfSigned(DateTimeOffset.Now, DateTimeOffset.Now.AddHours(1));

        // Create PFX (PKCS #12) with private key
        SampleConstants.SaveX509(
            cert.Export(X509ContentType.Pfx, SampleConstants.TestCertificatesPassword),
            cert.Export(X509ContentType.Cert));
    }
}