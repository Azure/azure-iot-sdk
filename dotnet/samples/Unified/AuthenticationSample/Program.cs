// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Authentication.Boot;
using Microsoft.Azure.Iot.Device.Authentication.Operational;
using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Unified.Connection;
using SetupSampleDevice;

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
        // This should additionally show off doing certificate signing against IoT Hub and disconnecting/re-connecting using those newly issued certificates
    }

    // Device uses x509 boot certificate to provision via DPS and uses newly issued certificates generated during provisioning to connect to IoT Hub
    public static async Task X509BootAndIssuedCertificatesOperational(CancellationToken cancellationToken)
    {
        BootAuthenticationProvider bootAuthenticationProvider = new X509BootAuthenticationProvider(SampleConstants.LoadX509BootCertificate());

        // This approach relies on creating new X509Certificate2 objects from the device's private key and the newly issued client certificates.
        // The IssuedCertificateOperationalAuthenticationProvider class offers another constructor which allows for custom handling of this process.
        OperationalAuthenticationProvider operationalAuthenticationProvider = new IssuedCertificateOperationalAuthenticationProvider(SampleConstants.LoadPrivateKey());

        ProvisioningSettings provisioningSettings = new(SampleConstants.LoadIdScope());
        ConnectionClient client = new();
        await client.ProvisionAndConnectAsync(provisioningSettings, bootAuthenticationProvider, operationalAuthenticationProvider, cancellationToken: cancellationToken);

        Console.WriteLine("Device successfully provisioned using boot certificate and connected to IoT Hub using issued certificates");
    }
}