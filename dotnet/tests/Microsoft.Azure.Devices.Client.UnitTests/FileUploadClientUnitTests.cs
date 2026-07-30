using Microsoft.Azure.Devices.Client.FileUpload;
using System.Net;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using System.Text.Json;
using Xunit;

namespace Microsoft.Azure.Devices.Client.UnitTests
{
    /// <summary>
    /// Offline coverage for <see cref="FileUploadClient"/>. Every case drives the client
    /// through a stub <see cref="HttpMessageHandler"/>, so the requests it builds and the
    /// responses it parses are asserted without a hub, a storage account or a network.
    /// </summary>
    public class FileUploadClientUnitTests
    {
        private const string DeviceId = "SomeDeviceId";
        // Lower case: Uri normalizes the host, so this is what the handler observes.
        private const string HostName = "somehub.azure-devices.net";

        private const string SasUriResponseJson = """
            {
              "correlationId": "corr-123",
              "hostName": "acct.blob.core.windows.net",
              "containerName": "uploads",
              "blobName": "SomeDeviceId/data.txt",
              "sasToken": "?sv=2021-04-12&sig=abc%2F123&se=2026-01-01&sp=rw"
            }
            """;

        /// <summary>Records every request and returns a programmed response.</summary>
        private sealed class StubHandler : HttpMessageHandler
        {
            public List<HttpMethod> Methods { get; } = new();
            public List<string> RequestUris { get; } = new();
            public List<string> RequestBodies { get; } = new();
            public int DisposeCount { get; private set; }

            public HttpStatusCode ResponseStatus { get; set; } = HttpStatusCode.OK;
            public string ResponseBody { get; set; } = SasUriResponseJson;

            protected override async Task<HttpResponseMessage> SendAsync(
                HttpRequestMessage request,
                CancellationToken cancellationToken)
            {
                cancellationToken.ThrowIfCancellationRequested();

                Methods.Add(request.Method);
                RequestUris.Add(request.RequestUri!.ToString());
                RequestBodies.Add(
                    request.Content == null
                        ? string.Empty
                        : await request.Content.ReadAsStringAsync(cancellationToken));

                return new HttpResponseMessage(ResponseStatus)
                {
                    Content = new StringContent(ResponseBody, Encoding.UTF8, "application/json"),
                };
            }

            protected override void Dispose(bool disposing)
            {
                DisposeCount++;
                base.Dispose(disposing);
            }
        }

        private static MockConnectionClient ConnectedClient(bool isAzureEventGrid = false)
        {
            MockConnectionClient connectionClient = new();
            connectionClient.SetCurrentConnectionContext(new ConnectionContext()
            {
                DeviceId = DeviceId,
                IotHubHostName = HostName,
                IsAzureEventGrid = isAzureEventGrid,
            });
            return connectionClient;
        }

        /// <summary>A self-signed certificate stands in for the device identity.</summary>
        private static X509Certificate2 TestCertificate()
        {
            using var rsa = System.Security.Cryptography.RSA.Create(2048);
            var request = new CertificateRequest(
                "CN=" + DeviceId, rsa, System.Security.Cryptography.HashAlgorithmName.SHA256,
                System.Security.Cryptography.RSASignaturePadding.Pkcs1);
            return request.CreateSelfSigned(DateTimeOffset.UtcNow.AddDays(-1), DateTimeOffset.UtcNow.AddDays(1));
        }

        private static (FileUploadClient client, StubHandler handler) CreateClient(
            bool isAzureEventGrid = false)
        {
            StubHandler handler = new();
            HttpClient httpClient = new(handler) { BaseAddress = new Uri("https://" + HostName) };
            return (new FileUploadClient(ConnectedClient(isAzureEventGrid), TestCertificate(), httpClient), handler);
        }

        [Fact]
        public async Task GetSasUriBuildsTheRequestAndParsesTheResponse()
        {
            (FileUploadClient client, StubHandler handler) = CreateClient();

            FileUploadSasUriResponse response = await client.GetFileUploadSasUriAsync(
                new FileUploadSasUriRequest { BlobName = "data.txt" },
                TestContext.Current.CancellationToken);

            Assert.Equal(HttpMethod.Post, Assert.Single(handler.Methods));
            // The api-version is an internal constant; assert the route and that a version
            // is carried, not the version literal.
            Assert.StartsWith(
                $"https://{HostName}/devices/{DeviceId}/files?api-version=",
                Assert.Single(handler.RequestUris));
            Assert.Contains("\"blobName\":\"data.txt\"", Assert.Single(handler.RequestBodies));

            Assert.Equal("corr-123", response.CorrelationId);
            Assert.Equal("acct.blob.core.windows.net", response.HostName);
            Assert.Equal("uploads", response.ContainerName);
            Assert.Equal("SomeDeviceId/data.txt", response.BlobName);
            Assert.Equal(
                "https://acct.blob.core.windows.net/uploads/SomeDeviceId%2Fdata.txt?sv=2021-04-12&sig=abc%2F123&se=2026-01-01&sp=rw",
                response.GetBlobUri().ToString());
        }

        [Fact]
        public async Task GetSasUriSurfacesTheServiceErrorCode()
        {
            (FileUploadClient client, StubHandler handler) = CreateClient();
            handler.ResponseStatus = HttpStatusCode.BadRequest;
            handler.ResponseBody = JsonSerializer.Serialize(new
            {
                Message = "{\"errorCode\":400020,\"message\":\"Storage endpoint or blob name is invalid\"}",
                ExceptionMessage = "bad request",
            });

            IotHubServiceException exception = await Assert.ThrowsAsync<IotHubServiceException>(
                async () => await client.GetFileUploadSasUriAsync(
                    new FileUploadSasUriRequest { BlobName = "data.txt" },
                    TestContext.Current.CancellationToken));

            Assert.Equal(400020, exception.ErrorDetails!.ErrorCode);
            Assert.Contains("Storage endpoint or blob name is invalid", exception.Message);
        }

        [Fact]
        public async Task GetSasUriReportsAnUnrecognizedErrorBody()
        {
            (FileUploadClient client, StubHandler handler) = CreateClient();
            handler.ResponseStatus = HttpStatusCode.ServiceUnavailable;
            handler.ResponseBody = "<html>gateway is unhappy</html>";

            IotHubServiceException exception = await Assert.ThrowsAsync<IotHubServiceException>(
                async () => await client.GetFileUploadSasUriAsync(
                    new FileUploadSasUriRequest { BlobName = "data.txt" },
                    TestContext.Current.CancellationToken));

            Assert.Contains("unexpected format", exception.Message);
        }

        [Fact]
        public async Task CompleteBuildsTheNotificationRequest()
        {
            (FileUploadClient client, StubHandler handler) = CreateClient();
            handler.ResponseStatus = HttpStatusCode.NoContent;
            handler.ResponseBody = string.Empty;

            await client.CompleteFileUploadSasUriAsync(
                new FileUploadCompletionNotification
                {
                    CorrelationId = "corr-123",
                    IsSuccess = true,
                    StatusCode = 200,
                    StatusDescription = "OK",
                },
                TestContext.Current.CancellationToken);

            Assert.Equal(HttpMethod.Post, Assert.Single(handler.Methods));
            Assert.StartsWith(
                $"https://{HostName}/devices/{DeviceId}/files/notifications?api-version=",
                Assert.Single(handler.RequestUris));

            string body = Assert.Single(handler.RequestBodies);
            Assert.Contains("\"correlationId\":\"corr-123\"", body);
            Assert.Contains("\"isSuccess\":true", body);
        }

        [Fact]
        public async Task CompleteTreatsAnythingButNoContentAsAFailure()
        {
            (FileUploadClient client, StubHandler handler) = CreateClient();
            handler.ResponseStatus = HttpStatusCode.BadRequest;
            handler.ResponseBody = JsonSerializer.Serialize(new
            {
                Message = "{\"errorCode\":400000,\"message\":\"The correlation id was not recognized\"}",
                ExceptionMessage = "bad request",
            });

            IotHubServiceException exception = await Assert.ThrowsAsync<IotHubServiceException>(
                async () => await client.CompleteFileUploadSasUriAsync(
                    new FileUploadCompletionNotification { CorrelationId = "nope", IsSuccess = true },
                    TestContext.Current.CancellationToken));

            Assert.Equal(400000, exception.ErrorDetails!.ErrorCode);
        }

        [Fact]
        public async Task OperationsRequireAConnectedDevice()
        {
            // A connection client that never connected has no context.
            FileUploadClient client = new(new MockConnectionClient(), TestCertificate());

            await Assert.ThrowsAsync<NotSupportedException>(
                async () => await client.GetFileUploadSasUriAsync(
                    new FileUploadSasUriRequest { BlobName = "data.txt" },
                    TestContext.Current.CancellationToken));

            await Assert.ThrowsAsync<NotSupportedException>(
                async () => await client.CompleteFileUploadSasUriAsync(
                    new FileUploadCompletionNotification { CorrelationId = "corr", IsSuccess = true },
                    TestContext.Current.CancellationToken));
        }

        [Fact]
        public async Task AProvidedHttpClientIsRejectedOnAnAzureEventGridHub()
        {
            // File upload travels over MQTT on AEG, so a caller-supplied HTTP client
            // would never be used and is refused rather than silently ignored.
            (FileUploadClient client, _) = CreateClient(isAzureEventGrid: true);

            await Assert.ThrowsAsync<NotSupportedException>(
                async () => await client.GetFileUploadSasUriAsync(
                    new FileUploadSasUriRequest { BlobName = "data.txt" },
                    TestContext.Current.CancellationToken));
        }

        [Fact]
        public async Task AProvidedHttpClientIsActuallyUsed()
        {
            // Regression: initialization used to replace the caller's HttpClient with one of
            // its own, discarding the handler -- and with it any proxy, pooling or retry
            // policy the caller had configured.
            (FileUploadClient client, StubHandler handler) = CreateClient();

            await client.GetFileUploadSasUriAsync(
                new FileUploadSasUriRequest { BlobName = "data.txt" },
                TestContext.Current.CancellationToken);

            Assert.Single(handler.RequestUris);
        }

        [Fact]
        public async Task AProvidedHttpClientOutlivesTheFileUploadClient()
        {
            // The caller owns a client it supplied; disposing it here would break every
            // other component sharing it.
            StubHandler handler = new();
            using HttpClient httpClient = new(handler) { BaseAddress = new Uri("https://" + HostName) };
            FileUploadClient client = new(ConnectedClient(), TestCertificate(), httpClient);

            await client.GetFileUploadSasUriAsync(
                new FileUploadSasUriRequest { BlobName = "data.txt" },
                TestContext.Current.CancellationToken);
            client.Dispose();

            Assert.Equal(0, handler.DisposeCount);

            // Still usable after the file upload client is gone.
            using HttpResponseMessage response = await httpClient.GetAsync(
                new Uri("https://" + HostName + "/ping"), TestContext.Current.CancellationToken);
            Assert.Equal(HttpStatusCode.OK, response.StatusCode);
        }

        [Fact]
        public async Task ARequestHonoursCancellation()
        {
            (FileUploadClient client, _) = CreateClient();
            using CancellationTokenSource cts = new();
            await cts.CancelAsync();

            await Assert.ThrowsAnyAsync<OperationCanceledException>(
                async () => await client.GetFileUploadSasUriAsync(
                    new FileUploadSasUriRequest { BlobName = "data.txt" }, cts.Token));
        }
    }
}
