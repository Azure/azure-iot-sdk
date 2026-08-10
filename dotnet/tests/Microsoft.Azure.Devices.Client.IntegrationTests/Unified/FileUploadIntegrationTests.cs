using Azure.Storage.Blobs.Models;
using Azure.Storage.Blobs.Specialized;
using System.Text;
using Xunit;
using Microsoft.Azure.Devices.Client.Unified.FileUpload;
using Microsoft.Azure.Devices.Client.IntegrationTests.Unified;
using Microsoft.Azure.Devices.Client.Models.FileUpload;
using Microsoft.Azure.Devices.Client.Exceptions;

namespace Microsoft.Azure.Devices.Client.IntegrationTests.Unified
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

            await using UnifiedDeviceTestContext testDeviceContext = await Setup.CreateConnectedUnifiedConnectionClientAsync(testAgainstClassicHub, cts.Token);

            FileUploadClient fileUploadClient;
            if (testAgainstClassicHub)
            {
                if (withProvidedHttpClient)
                {
                    var handler = new HttpClientHandler();
                    handler.ClientCertificates.Add(testDeviceContext.AuthenticationProvider.ClientCertificate);
                    HttpClient userProvidedHttpClient = new(handler);
                    fileUploadClient = new FileUploadClient(testDeviceContext.ConnectionClient, userProvidedHttpClient);
                }
                else
                {
                    fileUploadClient = new FileUploadClient(testDeviceContext.ConnectionClient);
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

            await using UnifiedDeviceTestContext testDeviceContext = await Setup.CreateConnectedUnifiedConnectionClientAsync(testAgainstClassicHub, cts.Token);

            FileUploadClient fileUploadClient;
            fileUploadClient = new FileUploadClient(testDeviceContext.ConnectionClient);

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
