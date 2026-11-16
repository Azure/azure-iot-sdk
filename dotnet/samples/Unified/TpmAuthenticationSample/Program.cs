// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Devices.Provisioning.Service;
using Microsoft.Azure.Iot.Device.Authentication;
using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Models.Telemetry;
using Microsoft.Azure.Iot.Device.Provisioning.Models;
using Microsoft.Azure.Iot.Device.Unified.Connection;
using Microsoft.Azure.Iot.Device.Unified.Telemetry;
using SetupSampleDevice;
using System.Text;
using TpmAuthenticationSample;

/// <summary>
/// Provisions a device that attests with a TPM, connects it to the IoT Hub it is assigned and sends telemetry.
/// </summary>
/// <remarks>
/// Run with <c>--simulator</c> to use a TPM simulator listening on 127.0.0.1:2321 instead of the platform TPM.
/// </remarks>
internal class Program
{
    private static async Task Main(string[] args)
    {
        using CancellationTokenSource cts = new();
        cts.CancelAfter(TimeSpan.FromMinutes(2));

        // Cancel sample on key press
        Console.CancelKeyPress += (sender, eventArgs) =>
        {
            cts.Cancel();
            eventArgs.Cancel = true;
        };

        string registrationId = Environment.GetEnvironmentVariable("SAMPLE_TPM_REGISTRATION_ID") ?? "tpm-sample-device";
        string idScope = SampleConstants.LoadIdScope();

        // The SDK only depends on ITpmSecurityProvider. This sample implements it with the Microsoft.TSS library.
        using TssTpmSecurityProvider tpm = args.Contains("--simulator")
            ? TssTpmSecurityProvider.CreateForSimulator()
            : TssTpmSecurityProvider.CreateForPlatformTpm();

        // DPS identifies a TPM device by its endorsement key, so its individual enrollment is created with it.
        string endorsementKey = Convert.ToBase64String(await tpm.GetEndorsementKeyAsync(cts.Token));
        Console.WriteLine($"Registration Id: {registrationId}");
        Console.WriteLine($"Endorsement key: {endorsementKey}");

        string? dpsConnectionString = Environment.GetEnvironmentVariable("IOT_DPS_CONNECTION_STRING");
        if (!string.IsNullOrEmpty(dpsConnectionString))
        {
            using ProvisioningServiceClient provisioningServiceClient = ProvisioningServiceClient.CreateFromConnectionString(dpsConnectionString);
            await provisioningServiceClient.CreateOrUpdateIndividualEnrollmentAsync(
                new IndividualEnrollment(registrationId, new TpmAttestation(endorsementKey)),
                cts.Token);
            Console.WriteLine("Created or updated the device's individual enrollment.");
        }

        TpmAuthenticationProvider authentication = new(registrationId, idScope, tpm);

        // Device Provisioning Service only supports TPM attestation over HTTPS. The device still connects to its
        // assigned IoT Hub over MQTT.
        ProvisioningSettings provisioningSettings = new(idScope)
        {
            Transport = ProvisioningTransport.Http,
        };

        using ConnectionClient connectionClient = new();
        TelemetryClient telemetryClient = new(connectionClient);

        ConnectionContext connectionContext = await connectionClient.ProvisionAndConnectAsync(provisioningSettings, authentication, cancellationToken: cts.Token);
        Console.WriteLine($"Device {connectionContext.DeviceId} is now provisioned and connected to {connectionContext.IotHubHostName}.");

        DeviceToCloudTelemetry telemetry = new()
        {
            Payload = Encoding.UTF8.GetBytes("Hello from a TPM device!"),
            MessageId = Guid.NewGuid().ToString(),
        };

        Console.WriteLine($"Sending telemetry with message Id {telemetry.MessageId}");
        await telemetryClient.SendTelemetryAsync(telemetry, cts.Token);

        await connectionClient.DisconnectAsync(null, cts.Token);
    }
}
