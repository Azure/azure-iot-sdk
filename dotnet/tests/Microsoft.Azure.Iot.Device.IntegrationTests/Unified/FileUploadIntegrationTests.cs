using Azure.Storage.Blobs.Models;
using Azure.Storage.Blobs.Specialized;
using System.Text;
using Xunit;
using Microsoft.Azure.Iot.Device.Unified.FileUpload;
using Microsoft.Azure.Iot.Device.IntegrationTests.Unified;
using Microsoft.Azure.Iot.Device.Models.FileUpload;
using Microsoft.Azure.Iot.Device.Exceptions;

namespace Microsoft.Azure.Iot.Device.IntegrationTests.Unified
{
    public class FileUploadIntegrationTests
    {
        [Theory(Timeout = Setup.TestTimeoutMilliseconds, Skip = "TODO Test infrastructure has issues. See CI pipeline yaml")]
        [InlineData(true, false)]
        [InlineData(true, true)]
        [InlineData(false, false)]
        public async Task TestFileUpload(bool testAgainstClassicHub, bool withProvidedHttpClient)
        {
            UnifiedDeviceTestContext testDeviceContext = await Setup.CreateConnectedUnifiedConnectionClientAsync(testAgainstClassicHub, null, TestContext.Current.CancellationToken);

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

            var sasUri = await fileUploadClient.GetFileUploadSasUriAsync(sasUriRequest, TestContext.Current.CancellationToken);

            // Use the Azure Storage SDK to upload a dummy file using the credentials provided by IoT Hub
            var blobClient = new BlockBlobClient(sasUri.GetBlobUri());
            MemoryStream dummyFileStream = new MemoryStream(Encoding.UTF8.GetBytes("Hello world"));
            await blobClient.UploadAsync(dummyFileStream, new BlobUploadOptions(), TestContext.Current.CancellationToken);

            FileUploadCompletionNotification completionNotification = new()
            {
                CorrelationId = sasUri.CorrelationId,
                IsSuccess = true,
                StatusCode = 200,
                StatusDescription = "OK",
            };

            await fileUploadClient.CompleteFileUploadSasUriAsync(completionNotification, TestContext.Current.CancellationToken);

            await testDeviceContext.DisposeAsync(); // Dispose this before any feature clients so that the test device identity can be cleaned up and the MQTT client disconnected gracefully
            fileUploadClient.Dispose();
        }

        [Theory(Timeout = Setup.TestTimeoutMilliseconds, Skip = "TODO Test infrastructure has issues. See CI pipeline yaml")]
        [InlineData(true)]
        [InlineData(false)]
        public async Task TestFileUpload_BadFormat(bool testAgainstClassicHub)
        {
            UnifiedDeviceTestContext testDeviceContext = await Setup.CreateConnectedUnifiedConnectionClientAsync(testAgainstClassicHub, null, TestContext.Current.CancellationToken);

            FileUploadClient fileUploadClient;
            fileUploadClient = new FileUploadClient(testDeviceContext.ConnectionClient);

            FileUploadSasUriRequest sasUriRequest = new()
            {
                BlobName = "",
            };

            // Check that that the file upload client understands how to parse a service error
            var exception = await Assert.ThrowsAsync<IotHubServiceException>(async () => await fileUploadClient.GetFileUploadSasUriAsync(sasUriRequest, TestContext.Current.CancellationToken));
            Assert.NotNull(exception.ErrorDetails);
            Assert.Equal(400004, exception.ErrorDetails.ErrorCode);

            // Check that that the file upload client understands how to parse a service error
            FileUploadCompletionNotification badFormatCompletionNotification = new()
            {
                CorrelationId = "ThisCorrelationIdDoesNotExist",
                IsSuccess = true,
            };
            exception = await Assert.ThrowsAsync<IotHubServiceException>(async () => await fileUploadClient.CompleteFileUploadSasUriAsync(badFormatCompletionNotification, TestContext.Current.CancellationToken));
            Assert.NotNull(exception.ErrorDetails);
            Assert.Equal(400000, exception.ErrorDetails.ErrorCode);

            await testDeviceContext.DisposeAsync(); // Dispose this before any feature clients so that the test device identity can be cleaned up and the MQTT client disconnected gracefully
            fileUploadClient.Dispose();
        }
    }
}
