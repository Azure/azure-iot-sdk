using Microsoft.Azure.Devices.Client;
using Microsoft.Azure.Devices.Client.Twin;
using System.Security.Cryptography.X509Certificates;

internal class Program
{
    static Twin? currentTwin = null;

    private static async Task Main(string[] args)
    {
        using CancellationTokenSource cts = new CancellationTokenSource();
        cts.CancelAfter(TimeSpan.FromSeconds(20));

        string idScope = Environment.GetEnvironmentVariable("DPS_ID_SCOPE") ?? throw new Exception("");
        string pcks12CertificatePath = Environment.GetEnvironmentVariable("X509_CERTIFICATE_PATH") ?? throw new Exception("");
        string pcks12CertificatePassword = Environment.GetEnvironmentVariable("X509_CERTIFICATE_PASSWORD") ?? throw new Exception("");
        X509AuthenticationProvider authentication = new(X509CertificateLoader.LoadPkcs12FromFile(pcks12CertificatePath, pcks12CertificatePassword));

        using ConnectionClient connectionClient = new ConnectionClient();

        using TwinClient twinClient = new TwinClient(connectionClient);

        Action<DesiredPatchReceivedEventArgs> HandleDesiredPropertiesUpdateAsync = async (args) =>
        {
            Console.WriteLine($"Received desired property update");
            currentTwin.DesiredVersion = args.DesiredPropertiesVersion;
            currentTwin.Desired = args.DesiredProperties;

            // Some application-level processing based on what desired properties changed

            ReportedPatchRequest reportedPatch = new()
            {
                ReportedProperties = args.DesiredProperties, // Echo back the desired properties as the current reported properties
                IfMatch = 1 //TODO how does this work again?
            };

            ReportedPatchResponse patchResponse = await twinClient.UpdateReportedPropertiesAsync(reportedPatch);
            currentTwin.ReportedVersion = patchResponse.Version;
            if (patchResponse.Result == Result.Ok)
            {
                currentTwin.Reported = args.DesiredProperties;
                currentTwin.ReportedVersion = args.DesiredPropertiesVersion;
            }
        };

        twinClient.DesiredPatchReceived += HandleDesiredPropertiesUpdateAsync;

        ProvisioningSettings provisioningSettings = new(idScope);
        TwinPushOptions twinPushOptions = new()
        {
            ReceiveDesiredPropertyUpdates = true,
            ReceiveReportedPropertiesUponConnect = true,
        };

        var connectionContext = await connectionClient.ProvisionAndConnectAsync(provisioningSettings, authentication, twinPushOptions);
        currentTwin = connectionContext.InitialTwinPush;

        await Task.Delay(-1, cts.Token);

        twinClient.DesiredPatchReceived -= HandleDesiredPropertiesUpdateAsync;
        await connectionClient.DisconnectAsync();
    }
}