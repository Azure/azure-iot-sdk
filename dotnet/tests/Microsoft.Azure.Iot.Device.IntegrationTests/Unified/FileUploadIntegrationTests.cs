// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Azure.Storage.Blobs.Models;
using Azure.Storage.Blobs.Specialized;
using System.Text;
using Xunit;
using Microsoft.Azure.Iot.Device.Unified.FileUpload;
using Microsoft.Azure.Iot.Device.Models.FileUpload;
using Microsoft.Azure.Iot.Device.Exceptions;
using Microsoft.Azure.Iot.Device.Authentication;
using Microsoft.Azure.Iot.Device.Unified.Models;

namespace Microsoft.Azure.Iot.Device.IntegrationTests.Unified
{
    public class FileUploadIntegrationTests
    {
        [Theory(Timeout = Setup.TestTimeoutMilliseconds)]
        [InlineData(true, false)]
        [InlineData(true, true)]
        [InlineData(false, false)]
        public async Task TestFileUpload(bool testAgainstClassicHub, bool withProvidedHttpClient)
        {
            // The device's certificate is only known once it is set up, but the HTTP client must be supplied when the connection client is
            // constructed. A handler's settings can still change until it sends its first request, so the certificate is added to it afterwards.
            HttpClientHandler? handler = withProvidedHttpClient ? new HttpClientHandler() : null;
            using HttpClient? userProvidedHttpClient = handler == null ? null : new HttpClient(handler);
            UnifiedDeviceTestContext testDeviceContext = await Setup.CreateConnectedUnifiedConnectionClientAsync(
                testAgainstClassicHub,
                new ConnectionClientOptions { HttpClient = userProvidedHttpClient },
                cancellationToken: TestContext.Current.CancellationToken);
            handler?.ClientCertificates.Add(Assert.IsType<X509AuthenticationProvider>(testDeviceContext.AuthenticationProvider).PrimaryClientCertificate);

            FileUploadClient fileUploadClient = new(testDeviceContext.ConnectionClient);

            await UploadFileAndCompleteAsync(fileUploadClient, TestContext.Current.CancellationToken);

            await testDeviceContext.DisposeAsync(); // Dispose this before any feature clients so that the test device identity can be cleaned up and the MQTT client disconnected gracefully
            fileUploadClient.Dispose();
        }

        [Theory(Timeout = Setup.TestTimeoutMilliseconds)]
        [InlineData(false)]
        [InlineData(true)]
        public async Task TestFileUpload_SymmetricKey(bool withProvidedHttpClient)
        {
            using HttpClient? userProvidedHttpClient = withProvidedHttpClient ? new HttpClient() : null;
            UnifiedDeviceTestContext testDeviceContext = await Setup.CreateConnectedUnifiedConnectionClientWithSymmetricKeyAsync(
                new ConnectionClientOptions { HttpClient = userProvidedHttpClient },
                cancellationToken: TestContext.Current.CancellationToken);

            // Neither HTTP client is configured with a client certificate, so IoT Hub can only authenticate these requests with the SAS token
            Assert.IsType<SymmetricKeyAuthenticationProvider>(testDeviceContext.AuthenticationProvider);
            Assert.Null(testDeviceContext.ConnectionContext.IssuedClientCertificates);
            FileUploadClient fileUploadClient = new(testDeviceContext.ConnectionClient);

            await UploadFileAndCompleteAsync(fileUploadClient, TestContext.Current.CancellationToken);

            await testDeviceContext.DisposeAsync(); // Dispose this before any feature clients so that the test device identity can be cleaned up and the MQTT client disconnected gracefully
            fileUploadClient.Dispose();
        }

        /// <summary>
        /// Gets a SAS URI from IoT Hub, uploads a small blob with it, and notifies IoT Hub that the upload completed.
        /// </summary>
        internal static async Task UploadFileAndCompleteAsync(FileUploadClient fileUploadClient, CancellationToken cancellationToken)
        {
            FileUploadSasUriRequest sasUriRequest = new()
            {
                BlobName = "TestFile.txt",
            };

            var sasUri = await fileUploadClient.GetFileUploadSasUriAsync(sasUriRequest, cancellationToken);

            // Use the Azure Storage SDK to upload a dummy file using the credentials provided by IoT Hub
            var blobClient = new BlockBlobClient(sasUri.GetBlobUri());
            MemoryStream dummyFileStream = new MemoryStream(Encoding.UTF8.GetBytes("Hello world"));
            await blobClient.UploadAsync(dummyFileStream, new BlobUploadOptions(), cancellationToken);

            FileUploadCompletionNotification completionNotification = new()
            {
                CorrelationId = sasUri.CorrelationId,
                IsSuccess = true,
                StatusCode = 200,
                StatusDescription = "OK",
            };

            await fileUploadClient.CompleteFileUploadSasUriAsync(completionNotification, cancellationToken);
        }

        [Theory(Timeout = Setup.TestTimeoutMilliseconds)]
        [InlineData(true)]
        [InlineData(false)]
        public async Task TestFileUpload_BadFormat(bool testAgainstClassicHub)
        {
            UnifiedDeviceTestContext testDeviceContext = await Setup.CreateConnectedUnifiedConnectionClientAsync(testAgainstClassicHub, null, cancellationToken: TestContext.Current.CancellationToken);

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
