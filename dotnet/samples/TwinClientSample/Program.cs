using Microsoft.Azure.Devices.Client;
using Microsoft.Azure.Devices.Client.Twin;
using SetupSampleDevice;
using System.Security.Cryptography.X509Certificates;
using System.Text.Json;

internal class Program
{
    static Twin? currentTwin = null;

    private static async Task Main(string[] args)
    {
        using CancellationTokenSource cts = new CancellationTokenSource();
        cts.CancelAfter(TimeSpan.FromMinutes(10));

        // Cancel sample on key press
        Console.CancelKeyPress += (sender, eventArgs) =>
        {
            cts.Cancel();
            eventArgs.Cancel = true;
        };

        string deviceId = SampleConstants.LoadDeviceId();
        string idScope = SampleConstants.LoadIdScope();
        X509AuthenticationProvider authentication = SampleConstants.LoadAuthenticationProvider();

        using ConnectionClient connectionClient = new ConnectionClient();

        using TwinClient twinClient = new TwinClient(connectionClient);

        Action<DesiredPatchReceivedEventArgs> HandleDesiredPropertiesUpdateAsync = async (args) =>
        {
            if (currentTwin == null)
            {
                // The full twin wasn't received yet, so disregard any piecemeal updates that came before it 
                return;
            }

            Console.WriteLine($"Received desired property update");
            currentTwin.DesiredVersion = args.DesiredPropertiesVersion;
            currentTwin.Desired = args.DesiredProperties;

            // Some application-level processing based on what desired properties changed

            ReportedPatchRequest reportedPatch = new()
            {
                ReportedProperties = args.DesiredProperties, // Echo back the desired properties as the current reported properties
                IfMatch = 1 //TODO how does this work again?
            };

            Console.WriteLine($"Responding to desired patch by sending a reported patch");
            ReportedPatchResponse patchResponse = await twinClient.UpdateReportedPropertiesAsync(reportedPatch);
            currentTwin.ReportedVersion = patchResponse.Version;
            if (patchResponse.Result == Result.Ok)
            {
                currentTwin.Reported = args.DesiredProperties;
                currentTwin.ReportedVersion = args.DesiredPropertiesVersion;
            }

            Console.WriteLine($"The current twin is now: {JsonSerializer.Serialize(currentTwin)}");
        };

        twinClient.DesiredPatchReceived += HandleDesiredPropertiesUpdateAsync;

        ProvisioningSettings provisioningSettings = new(idScope);
        TwinPushOptions twinPushOptions = new()
        {
            ReceiveDesiredPropertyUpdates = true,
            ReceiveReportedPropertiesUponConnect = true,
        };

        var connectionContext = await connectionClient.ProvisionAndConnectAsync(provisioningSettings, authentication, twinPushOptions);
        Console.WriteLine($"Device {deviceId} is now provisioned and connected to IoT Hub. Now listening for desired property patches");

        currentTwin = connectionContext.InitialTwinPush;
        Console.WriteLine($"The current twin is: {JsonSerializer.Serialize(currentTwin)}");

        try
        {
            Console.WriteLine("Press 'Ctrl+C' to end the sample");
            await Task.Delay(-1, cts.Token);
        }
        catch (OperationCanceledException)
        {
            Console.WriteLine("Sample timeout has completed. Shutting down the sample...");
        }

        twinClient.DesiredPatchReceived -= HandleDesiredPropertiesUpdateAsync;
        await connectionClient.DisconnectAsync();
    }
}