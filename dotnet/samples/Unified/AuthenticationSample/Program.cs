// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Authentication;
using Microsoft.Azure.Iot.Device.Authentication.Boot;
using Microsoft.Azure.Iot.Device.Authentication.Operational;
using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Models.CertificateManagement;
using Microsoft.Azure.Iot.Device.Unified.Connection;
using SetupSampleDevice;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;

internal class Program
{
    public static async Task Main(string[] args)
    {
        using CancellationTokenSource cts = new CancellationTokenSource();
        cts.CancelAfter(TimeSpan.FromMinutes(10));

        // Cancel sample on key press
        Console.CancelKeyPress += (sender, eventArgs) =>
        {
            cts.Cancel();
            eventArgs.Cancel = true;
        };
    }

    // Device uses x509 boot certificate to both provision via DPS and to connect to IoT Hub after provisioning
    public static async Task X509BootAndX509Operational(CancellationToken cancellationToken)
    {
        BootAuthenticationProvider bootAuthenticationProvider = new X509BootAuthenticationProvider(SampleConstants.LoadX509BootCertificate());
        OperationalAuthenticationProvider operationalAuthenticationProvider = new X509OperationalAuthenticationProvider(SampleConstants.LoadX509BootCertificate());
        ProvisioningSettings provisioningSettings = new(SampleConstants.LoadIdScope());
        ConnectionClient client = new();
        await client.ProvisionAndConnectAsync(provisioningSettings, bootAuthenticationProvider, operationalAuthenticationProvider, cancellationToken: cancellationToken);

        Console.WriteLine("Device successfully provisioned using boot certificate and connected to IoT Hub using boot certificate");
    }

    public static async Task SymmetricKeyBootAndIssuedCertificatesOperational(CancellationToken cancellationToken)
    {
        BootAuthenticationProvider bootAuthenticationProvider = new SymmetricKeyBootAuthenticationProvider(SampleConstants.LoadSymmetricKeyDeviceId(), SampleConstants.LoadIdScope(), SampleConstants.LoadSymmetricKey());
        string registrationId = bootAuthenticationProvider.GetRegistrationId();

        // Every certificate signing request in this sample is signed with this private key, so every certificate issued for them
        // (by DPS or by IoT Hub) pairs with it.
        AsymmetricAlgorithm privateKey = SampleConstants.LoadSymmetricKeyDevicePrivateKey();

        // This approach relies on creating new X509Certificate2 objects from the device's private key and the newly issued client certificates.
        // The IssuedCertificateOperationalAuthenticationProvider class offers another constructor which allows for custom handling of this process.
        OperationalAuthenticationProvider operationalAuthenticationProvider = new IssuedCertificateOperationalAuthenticationProvider(privateKey);

        // Ask DPS to sign a certificate while provisioning. The device connects to IoT Hub with that DPS-issued certificate.
        ProvisioningSettings provisioningSettings = new(SampleConstants.LoadIdScope())
        {
            CertificateSigningRequest = new(privateKey, CreateCertificateSigningRequest(registrationId, privateKey)),
        };

        using ConnectionClient client = new();
        ConnectionContext connectionContext = await client.ProvisionAndConnectAsync(provisioningSettings, bootAuthenticationProvider, operationalAuthenticationProvider, cancellationToken: cancellationToken);

        Console.WriteLine("Device successfully provisioned using boot certificate and connected to IoT Hub using certificates issued by DPS");
        Console.WriteLine($"DPS issued certificate thumbprint: {GetLeafThumbprint(connectionContext.IssuedClientCertificates)}");

        // Ask IoT Hub to sign a new certificate. The "*" replace value replaces any certificate IoT Hub previously issued to this device.
        IotHubCertificateSigningRequest iotHubCertificateSigningRequest = new(
            connectionContext.DeviceId,
            CreateCertificateSigningRequest(connectionContext.DeviceId, privateKey),
            null,
            "*");
        CertificateSigningOperation certificateSigningOperation = await client.SendCertificateSigningRequestAsync(iotHubCertificateSigningRequest, cancellationToken);

        CertificateSigningResponse certificateSigningResponse;
        try
        {
            await certificateSigningOperation.Accepted.WaitAsync(cancellationToken);
            Console.WriteLine("IoT Hub accepted the certificate signing request");

            certificateSigningResponse = await certificateSigningOperation.Completed.WaitAsync(cancellationToken);
        }
        catch (CertificateSigningRequestFailedException ex)
        {
            Console.WriteLine($"Certificate signing request failed: {ex.Error.Message}");
            return;
        }

        // The connection client records the certificates IoT Hub issued on its connection context. The current connection keeps using the DPS
        // issued certificate; the IoT Hub issued certificate is presented the next time the client connects to IoT Hub.
        Console.WriteLine($"IoT Hub issued certificate thumbprint: {GetLeafThumbprint(certificateSigningResponse.Certificates)}");

        await client.DisconnectAsync(cancellationToken);
        Console.WriteLine("Device disconnected from IoT Hub");

        // Disconnecting keeps the connection context, so this reconnects directly to the same IoT Hub (without provisioning again) using the
        // certificate that IoT Hub issued.
        connectionContext = await client.ProvisionAndConnectAsync(provisioningSettings, bootAuthenticationProvider, operationalAuthenticationProvider, cancellationToken: cancellationToken);

        await client.DisconnectAsync(cancellationToken);
    }

    // Device uses x509 boot certificate to provision via DPS and uses newly issued certificates generated during provisioning to connect to IoT Hub.
    // It then asks IoT Hub to sign a new certificate, and disconnects and reconnects to IoT Hub using the certificate that IoT Hub issued.
    public static async Task X509BootAndIssuedCertificatesOperational(CancellationToken cancellationToken)
    {
        BootAuthenticationProvider bootAuthenticationProvider = new X509BootAuthenticationProvider(SampleConstants.LoadX509BootCertificate());
        string registrationId = bootAuthenticationProvider.GetRegistrationId();

        // Every certificate signing request in this sample is signed with this private key, so every certificate issued for them
        // (by DPS or by IoT Hub) pairs with it.
        AsymmetricAlgorithm privateKey = SampleConstants.LoadX509PrivateKey();

        // This approach relies on creating new X509Certificate2 objects from the device's private key and the newly issued client certificates.
        // The IssuedCertificateOperationalAuthenticationProvider class offers another constructor which allows for custom handling of this process.
        OperationalAuthenticationProvider operationalAuthenticationProvider = new IssuedCertificateOperationalAuthenticationProvider(privateKey);

        // Ask DPS to sign a certificate while provisioning. The device connects to IoT Hub with that DPS-issued certificate.
        ProvisioningSettings provisioningSettings = new(SampleConstants.LoadIdScope())
        {
            CertificateSigningRequest = new(privateKey, CreateCertificateSigningRequest(registrationId, privateKey)),
        };

        using ConnectionClient client = new();
        ConnectionContext connectionContext = await client.ProvisionAndConnectAsync(provisioningSettings, bootAuthenticationProvider, operationalAuthenticationProvider, cancellationToken: cancellationToken);

        Console.WriteLine("Device successfully provisioned using boot certificate and connected to IoT Hub using certificates issued by DPS");
        Console.WriteLine($"DPS issued certificate thumbprint: {GetLeafThumbprint(connectionContext.IssuedClientCertificates)}");

        // Ask IoT Hub to sign a new certificate. The "*" replace value replaces any certificate IoT Hub previously issued to this device.
        IotHubCertificateSigningRequest iotHubCertificateSigningRequest = new(
            connectionContext.DeviceId,
            CreateCertificateSigningRequest(connectionContext.DeviceId, privateKey),
            null,
            "*");
        CertificateSigningOperation certificateSigningOperation = await client.SendCertificateSigningRequestAsync(iotHubCertificateSigningRequest, cancellationToken);

        CertificateSigningResponse certificateSigningResponse;
        try
        {
            await certificateSigningOperation.Accepted.WaitAsync(cancellationToken);
            Console.WriteLine("IoT Hub accepted the certificate signing request");

            certificateSigningResponse = await certificateSigningOperation.Completed.WaitAsync(cancellationToken);
        }
        catch (CertificateSigningRequestFailedException ex)
        {
            Console.WriteLine($"Certificate signing request failed: {ex.Error.Message}");
            return;
        }

        // The connection client records the certificates IoT Hub issued on its connection context. The current connection keeps using the DPS
        // issued certificate; the IoT Hub issued certificate is presented the next time the client connects to IoT Hub.
        Console.WriteLine($"IoT Hub issued certificate thumbprint: {GetLeafThumbprint(certificateSigningResponse.Certificates)}");

        await client.DisconnectAsync(cancellationToken);
        Console.WriteLine("Device disconnected from IoT Hub");

        // Disconnecting keeps the connection context, so this reconnects directly to the same IoT Hub (without provisioning again) using the
        // certificate that IoT Hub issued.
        connectionContext = await client.ProvisionAndConnectAsync(provisioningSettings, bootAuthenticationProvider, operationalAuthenticationProvider, cancellationToken: cancellationToken);

        await client.DisconnectAsync(cancellationToken);
    }

    private static string CreateCertificateSigningRequest(string commonName, AsymmetricAlgorithm privateKey)
    {
        CertificateRequest request = privateKey switch
        {
            RSA rsa => new CertificateRequest($"CN={commonName}", rsa, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1),
            ECDsa ecdsa => new CertificateRequest($"CN={commonName}", ecdsa, HashAlgorithmName.SHA256),
            _ => throw new NotSupportedException("Only RSA and ECDsa private keys are supported."),
        };

        return Convert.ToBase64String(request.CreateSigningRequest());
    }

    // Issued certificate chains are base64 encoded DER certificates, leaf first
    private static string? GetLeafThumbprint(IReadOnlyList<string>? issuedCertificates)
    {
        if (issuedCertificates is not { Count: > 0 })
        {
            return null;
        }

        using X509Certificate2 leaf = X509CertificateLoader.LoadCertificate(Convert.FromBase64String(issuedCertificates[0]));
        return leaf.Thumbprint;
    }
}