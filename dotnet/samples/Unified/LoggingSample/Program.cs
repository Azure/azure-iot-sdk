// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Authentication;
using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Models.Telemetry;
using Microsoft.Azure.Iot.Device.Unified.Connection;
using Microsoft.Azure.Iot.Device.Unified.Models;
using Microsoft.Azure.Iot.Device.Unified.Telemetry;
using SetupSampleDevice;
using System.Diagnostics;
using System.Text;

internal class Program
{
    private static async Task Main()
    {
        string logPath = Path.GetFullPath("iot-sdk.log");
        using StreamWriter logWriter = File.CreateText(logPath);
        using TextWriterTraceListener fileListener = new(logWriter)
        {
            Filter = new EventTypeFilter(SourceLevels.All),
            TraceOutputOptions = TraceOptions.DateTime,
        };
        using ConsoleTraceListener consoleListener = new()
        {
            Filter = new EventTypeFilter(SourceLevels.Information),
        };

        Trace.Listeners.Add(fileListener);
        Trace.Listeners.Add(consoleListener);
        Trace.AutoFlush = true;

        try
        {
            Trace.TraceInformation("Capturing SDK and MQTTnet diagnostics to {0}", logPath);
            await RunDeviceAsync();
        }
        finally
        {
            Trace.Flush();
            Trace.Listeners.Remove(fileListener);
            Trace.Listeners.Remove(consoleListener);
        }
    }

    private static async Task RunDeviceAsync()
    {
        using CancellationTokenSource cts = new(TimeSpan.FromSeconds(30));
        ConsoleCancelEventHandler cancelHandler = (sender, eventArgs) =>
        {
            eventArgs.Cancel = true;
            cts.Cancel();
        };
        Console.CancelKeyPress += cancelHandler;

        try
        {
            using var certificate = SampleConstants.LoadBootCertificate();
            X509AuthenticationProvider authentication = new(certificate, null);
            ProvisioningSettings provisioningSettings = new(SampleConstants.LoadIdScope());
            using ConnectionClient connectionClient = new(new ConnectionClientOptions
            {
                EnableMqttLogging = true,
            });
            TelemetryClient telemetryClient = new(connectionClient);

            try
            {
                await connectionClient.ProvisionAndConnectAsync(
                    provisioningSettings, authentication, cancellationToken: cts.Token);
                Trace.TraceInformation("Device connected. Sending telemetry; press Ctrl+C to stop.");

                while (!cts.IsCancellationRequested)
                {
                    DeviceToCloudTelemetry telemetry = new()
                    {
                        Payload = Encoding.UTF8.GetBytes("Hello from the logging sample!"),
                        MessageId = Guid.NewGuid().ToString(),
                    };
                    await telemetryClient.SendTelemetryAsync(telemetry, cts.Token);
                    Trace.TraceInformation("Sent telemetry with message Id {0}", telemetry.MessageId);
                    await Task.Delay(TimeSpan.FromSeconds(2), cts.Token);
                }
            }
            catch (OperationCanceledException) when (cts.IsCancellationRequested)
            {
                Trace.TraceInformation("Sample stopped by Ctrl+C or the 30-second timeout.");
            }

            using CancellationTokenSource disconnectCts = new(TimeSpan.FromSeconds(10));
            await connectionClient.DisconnectAsync(disconnectCts.Token);
        }
        finally
        {
            Console.CancelKeyPress -= cancelHandler;
        }
    }
}
