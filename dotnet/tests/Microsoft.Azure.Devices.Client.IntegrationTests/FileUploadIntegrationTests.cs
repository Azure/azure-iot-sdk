using Azure.Storage.Blobs.Models;
using Azure.Storage.Blobs.Specialized;
using Microsoft.Azure.Devices.Client.FileUpload;
using System.Text;
using Xunit;

namespace Microsoft.Azure.Devices.Client.IntegrationTests
{
    public class FileUploadIntegrationTests
    {
        [Theory(Timeout = Setup.TestTimeoutMilliseconds, Skip = "TODO Test infrastructure has issues. See CI pipeline yaml")]
        [InlineData(true, false)]
        [InlineData(true, true)]
        [InlineData(false, false)]
        public async Task TestFileUpload(bool testAgainstClassicHub, bool withProvidedHttpClient)
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
                    fileUploadClient = new FileUploadClient(testDeviceContext.ConnectionClient, testDeviceContext.AuthenticationProvider.ClientCertificate, userProvidedHttpClient);
                }
                else
                {
                    fileUploadClient = new FileUploadClient(testDeviceContext.ConnectionClient, testDeviceContext.AuthenticationProvider.ClientCertificate);
                }
            }
            else 
            {
                fileUploadClient = new FileUploadClient(testDeviceContext.ConnectionClient, testDeviceContext.AuthenticationProvider.ClientCertificate);
            }

            FileUploadSasUriRequest sasUriRequest = new()
            {
                BlobName = "TestFile.txt",
            };

            var sasUri = await fileUploadClient.GetFileUploadSasUriAsync(sasUriRequest, cts.Token);

            // Use the Azure Storage SDK to upload a dummy file using the credentials provided by IoT Hub
            var blobClient = new BlockBlobClient(sasUri.GetBlobUri());
            MemoryStream dummyFileStream = new MemoryStream(Encoding.UTF8.GetBytes("Hello world"));
            await blobClient.UploadAsync(dummyFileStream, new BlobUploadOptions(), cts.Token);

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

        [Theory(Timeout = Setup.TestTimeoutMilliseconds, Skip = "TODO Test infrastructure has issues. See CI pipeline yaml")]
        [InlineData(true)]
        [InlineData(false)]
        public async Task TestFileUpload_BadFormat(bool testAgainstClassicHub)
        {
            using CancellationTokenSource cts = new();
            cts.CancelAfter(Setup.TestTimeoutMilliseconds - 1000);

            await using TestConnectionClient testDeviceContext = await Setup.CreateConnectedConnectionClientAsync(testAgainstClassicHub, cts.Token);

            FileUploadClient fileUploadClient;
            fileUploadClient = new FileUploadClient(testDeviceContext.ConnectionClient, testDeviceContext.AuthenticationProvider.ClientCertificate);

            FileUploadSasUriRequest sasUriRequest = new()
            {
                BlobName = "",
            };

            // Check that that the file upload client understands how to parse a service error
            var exception = await Assert.ThrowsAsync<IotHubServiceException>(async () => await fileUploadClient.GetFileUploadSasUriAsync(sasUriRequest, cts.Token));
            Assert.Equal(400004, exception.ErrorDetails.ErrorCode);

            // Check that that the file upload client understands how to parse a service error
            FileUploadCompletionNotification badFormatCompletionNotification = new()
            {
                CorrelationId = "ThisCorrelationIdDoesNotExist",
                IsSuccess = true,
            };
            exception = await Assert.ThrowsAsync<IotHubServiceException>(async () => await fileUploadClient.CompleteFileUploadSasUriAsync(badFormatCompletionNotification, cts.Token));
            Assert.Equal(400000, exception.ErrorDetails.ErrorCode);

            fileUploadClient.Dispose();
        }
    }
}
