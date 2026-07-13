using Azure.Storage.Blobs.Models;
using Azure.Storage.Blobs.Specialized;
using Microsoft.Azure.Devices.Client;
using Microsoft.Azure.Devices.Client.FileUpload;
using Microsoft.Azure.Devices.Client.Telemetry;
using Microsoft.Azure.Devices.Client.Twin;
using SetupSampleDevice;
using System.Security.Cryptography.X509Certificates;
using System.Text;

internal class Program
{
    private static async Task Main(string[] args)
    {
        using CancellationTokenSource cts = new CancellationTokenSource();
        cts.CancelAfter(TimeSpan.FromSeconds(20));

        string deviceId = SampleConstants.LoadDeviceId();
        string hostName = SampleConstants.LoadHostname();
        string idScope = SampleConstants.LoadIdScope();
        X509AuthenticationProvider authentication = SampleConstants.LoadAuthenticationProvider();

        using ConnectionClient connectionClient = new ConnectionClient();

        FileUploadClient fileUploadClient = new FileUploadClient(connectionClient);

        ProvisioningSettings provisioningSettings = new(idScope);
        var connectionContext = await connectionClient.ProvisionAndConnectAsync(provisioningSettings, authentication);
        Console.WriteLine("Device is now connected.");

        FileUploadSasUriRequest sasUriRequest = new()
        {
            BlobName = "TestFile.txt",
        };
        Console.WriteLine("Getting a SAS URI from IoT hub...");
        var sasUri = await fileUploadClient.GetFileUploadSasUriAsync(sasUriRequest, cts.Token);

        bool wasFileUploadSuccessful = false;
        try
        {
            // Use the Azure Storage SDK to upload a dummy file using the credentials provided above by IoT Hub
            Console.WriteLine("Uploading a file to Azure Storage using the Azure Storage SDK.");
            var blobClient = new BlockBlobClient(sasUri.GetBlobUri());
            MemoryStream dummyFileStream = new MemoryStream(Encoding.UTF8.GetBytes("Hello world"));
            await blobClient.UploadAsync(dummyFileStream, new BlobUploadOptions(), cts.Token);
            wasFileUploadSuccessful = true;
        }
        catch (Exception e)
        {
            Console.WriteLine("Encountered an error during file upload. Will send a negative completion notification to IoT hub");
        }

        FileUploadCompletionNotification completionNotification = new()
        {
            CorrelationId = sasUri.CorrelationId,
            IsSuccess = wasFileUploadSuccessful,
            StatusCode = wasFileUploadSuccessful ? 200 : 500,
            StatusDescription = wasFileUploadSuccessful ? "OK" : "FAILURE",
        };

        Console.WriteLine("Notifying IoT Hub that the SAS URI is no longer in use.");
        await fileUploadClient.CompleteFileUploadSasUriAsync(completionNotification, cts.Token);

        await connectionClient.DisconnectAsync();
    }
}