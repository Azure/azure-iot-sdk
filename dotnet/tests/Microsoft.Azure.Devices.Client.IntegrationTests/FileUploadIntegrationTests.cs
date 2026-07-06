using Azure.Storage.Blobs.Models;
using Azure.Storage.Blobs.Specialized;
using Microsoft.Azure.Devices.Client.FileUpload;
using System.Text;
using Xunit;

namespace Microsoft.Azure.Devices.Client.IntegrationTests
{
    public class FileUploadIntegrationTests
    {
        [Theory(Timeout = Setup.TestTimeoutMilliseconds)]
        [InlineData(true, false, false)]
        [InlineData(true, true, false)]
        [InlineData(false, false, false)]
        [InlineData(true, false, true)]
        [InlineData(true, true, true)]
        [InlineData(false, false, true)]
        public async Task TestFileUpload(bool testAgainstClassicHub, bool withProvidedHttpClient, bool actuallyUploadFile)
        {
            using CancellationTokenSource cts = new();
            cts.CancelAfter(Setup.TestTimeoutMilliseconds - 1000);

            await using TestConnectionClient testDeviceContext = await Setup.CreateConnectedConnectionClientAsync(testAgainstClassicHub, cts.Token);

            FileUploadClient fileUploadClient;
            if (testAgainstClassicHub)
            {
                if (withProvidedHttpClient)
                {
                    var handler = new HttpClientHandler();
                    handler.ClientCertificates.Add(testDeviceContext.AuthenticationProvider.ClientCertificate);
                    handler.ServerCertificateCustomValidationCallback = (message, cert2, chain, errors) => true;
                    HttpClient userProvidedHttpClient = new(handler);
                    fileUploadClient = new FileUploadClient(testDeviceContext.ConnectionContext, userProvidedHttpClient);
                }
                else
                {
                    fileUploadClient = new FileUploadClient(testDeviceContext.ConnectionContext, testDeviceContext.AuthenticationProvider);
                }
            }
            else 
            {
                fileUploadClient = new FileUploadClient(testDeviceContext.ConnectionClient);
            }

            FileUploadSasUriRequest sasUriRequest = new()
            {
                BlobName = "TestFile.txt",
            };

            var sasUri = await fileUploadClient.GetFileUploadSasUriAsync(sasUriRequest, cts.Token);

            if (actuallyUploadFile)
            {
                // Use the Azure Storage SDK to upload a dummy file using the credentials provided by IoT Hub
                var blobClient = new BlockBlobClient(sasUri.GetBlobUri());
                MemoryStream dummyFileStream = new MemoryStream(Encoding.UTF8.GetBytes("Hello world"));
                await blobClient.UploadAsync(dummyFileStream, new BlobUploadOptions(), cts.Token);
            }

            FileUploadCompletionNotification completionNotification = new()
            {
                CorrelationId = sasUri.CorrelationId,
                IsSuccess = true,
                StatusCode = 200,
                StatusDescription = "OK",
            };

            await fileUploadClient.CompleteFileUploadSasUriAsync(completionNotification, cts.Token);

            fileUploadClient.Dispose();
        }
    }
}
