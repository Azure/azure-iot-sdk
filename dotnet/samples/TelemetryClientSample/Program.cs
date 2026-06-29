using Microsoft.Azure.Devices.Client;
using Microsoft.Azure.Devices.Client.Telemetry;
using Microsoft.Azure.Devices.Client.Twin;
using System.Security.Cryptography.X509Certificates;
using System.Text;

internal class Program
{
    private static async Task Main(string[] args)
    {
        using CancellationTokenSource cts = new CancellationTokenSource();
        cts.CancelAfter(TimeSpan.FromSeconds(20));

        string idScope = Environment.GetEnvironmentVariable("DPS_ID_SCOPE") ?? throw new Exception("");
        string pcks12CertificatePath = Environment.GetEnvironmentVariable("X509_CERTIFICATE_PATH") ?? throw new Exception("");
        string pcks12CertificatePassword = Environment.GetEnvironmentVariable("X509_CERTIFICATE_PASSWORD") ?? throw new Exception("");
        X509AuthenticationProvider authentication = new(X509CertificateLoader.LoadPkcs12FromFile(pcks12CertificatePath, pcks12CertificatePassword));

        using ConnectionClient connectionClient = new ConnectionClient();

        TelemetryClient telemetryClient = new TelemetryClient(connectionClient);

        ProvisioningSettings provisioningSettings = new(idScope);
        var connectionContext = await connectionClient.ProvisionAndConnectAsync(provisioningSettings, authentication);

        while (!cts.Token.IsCancellationRequested)
        {
            OutgoingTelemetryMessage outgoingTelemetry = new()
            {
                Payload = Encoding.UTF8.GetBytes("Hello world!"),
                MessageId = Guid.NewGuid().ToString(),
            };

            await telemetryClient.SendTelemetryAsync(outgoingTelemetry);
            await Task.Delay(TimeSpan.FromSeconds(1));
        }

        await connectionClient.DisconnectAsync();
    }
}