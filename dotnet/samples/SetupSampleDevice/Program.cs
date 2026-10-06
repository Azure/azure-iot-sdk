// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Devices;
using Microsoft.Azure.Devices.Provisioning.Service;
using System.Diagnostics;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using System.Text.Json;

namespace SetupSampleDevice;

internal class Program
{
    public static string DpsConnectionString = ""; // The connection string of your DPS
    public static string DpsIdScope = ""; // The Id scope of your DPS
    public static string IotHubHostName = ""; // The host name of the IoT hub linked to your DPS

    // Run this to register a test device on your IoT hub using the v1 IoT hub service client. It also locally saves the device credentials such that the other project
    // can be run using those credentials
    private static async Task Main(string[] args)
    {
        ArgumentException.ThrowIfNullOrEmpty(DpsConnectionString);
        ArgumentException.ThrowIfNullOrEmpty(DpsIdScope);
        ArgumentException.ThrowIfNullOrEmpty(IotHubHostName);

        await RegisterSampleDeviceAndSaveCredentialsAsync();
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