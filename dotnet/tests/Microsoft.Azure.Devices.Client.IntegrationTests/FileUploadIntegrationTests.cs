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
        public async Task TestFileUpload(bool testAgainstClassicHub, bool withProvidedHttpClient, bool actuallyUploadAFile)
        {
            using CancellationTokenSource cts = new();
            cts.CancelAfter(Setup.TestTimeoutMilliseconds - 1000);

            await using TestConnectionClient testDeviceContext = await Setup.CreateConnectedConnectionClientAsync(testAgainstClassicHub, cts.Token);

            FileUploadClient fileUploadClient;
            if (testAgainstClassicHub)
            {
                if (withProvidedHttpClient)
                {
                    using HttpClient httpClient = new();
                    fileUploadClient = new FileUploadClient(testDeviceContext.ConnectionContext, httpClient);
                }
                else
                {
                    fileUploadClient = new FileUploadClient(testDeviceContext.ConnectionContext);
                }
            }
            else 
            {
                fileUploadClient = new FileUploadClient(testDeviceContext.ConnectionClient);
            }

            FileUploadSasUriRequest sasUriRequest = new()
            {
                BlobName = Guid.NewGuid().ToString(),
            };

            var sasUri = await fileUploadClient.GetFileUploadSasUriAsync(sasUriRequest, cts.Token);

            if (actuallyUploadAFile)
            {
                // Create a BlobServiceClient that will authenticate through Active Directory
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
