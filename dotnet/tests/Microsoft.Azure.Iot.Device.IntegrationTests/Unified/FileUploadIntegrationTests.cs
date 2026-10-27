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

namespace Microsoft.Azure.Iot.Device.IntegrationTests.Unified
{
    public class FileUploadIntegrationTests
    {
        [Theory(Timeout = Setup.TestTimeoutMilliseconds)]
        [InlineData(true, false)]
        [InlineData(true, true)]
        [InlineData(false, false, Traits = new[] { Setup.HubTrait, Setup.MQTTv5HubTraitValue })]
        public async Task TestFileUpload(bool testAgainstClassicHub, bool withProvidedHttpClient)
        {
            if (!testAgainstClassicHub)
            {
                Assert.Skip("file upload is not supported for MQTTv5 hubs yet");
            }

            UnifiedDeviceTestContext testDeviceContext = await Setup.CreateConnectedUnifiedConnectionClientAsync(testAgainstClassicHub, null, TestContext.Current.CancellationToken);

            FileUploadClient fileUploadClient;
            if (testAgainstClassicHub)
            {
                if (withProvidedHttpClient)
                {
                    var handler = new HttpClientHandler();
                    handler.ClientCertificates.Add(Assert.IsType<X509AuthenticationProvider>(testDeviceContext.AuthenticationProvider).PrimaryClientCertificate);
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

            await UploadFileAndCompleteAsync(fileUploadClient, TestContext.Current.CancellationToken);

            await testDeviceContext.DisposeAsync(); // Dispose this before any feature clients so that the test device identity can be cleaned up and the MQTT client disconnected gracefully
            fileUploadClient.Dispose();
        }

        [Theory(Timeout = Setup.TestTimeoutMilliseconds)]
        [InlineData(false)]
        [InlineData(true)]
        public async Task TestFileUpload_SymmetricKey(bool withProvidedHttpClient)
        {
            UnifiedDeviceTestContext testDeviceContext = await Setup.CreateConnectedUnifiedConnectionClientWithSymmetricKeyAsync(cancellationToken: TestContext.Current.CancellationToken);

            // Neither HTTP client is configured with a client certificate, so IoT Hub can only authenticate these requests with the SAS token
            Assert.IsType<SymmetricKeyAuthenticationProvider>(testDeviceContext.AuthenticationProvider);
            Assert.Null(testDeviceContext.ConnectionContext.IssuedClientCertificates);
            FileUploadClient fileUploadClient = withProvidedHttpClient
                ? new FileUploadClient(testDeviceContext.ConnectionClient, new HttpClient())
                : new FileUploadClient(testDeviceContext.ConnectionClient);

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
        [InlineData(false, Traits = new[] { Setup.HubTrait, Setup.MQTTv5HubTraitValue })]
        public async Task TestFileUpload_BadFormat(bool testAgainstClassicHub)
        {
            if (!testAgainstClassicHub)
            {
                Assert.Skip("file upload is not supported for MQTTv5 hubs yet");
            }

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
