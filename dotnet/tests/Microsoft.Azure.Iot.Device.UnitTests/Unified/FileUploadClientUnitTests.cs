// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Authentication;
using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Models.FileUpload;
using Microsoft.Azure.Iot.Device.Mqtt;
using Microsoft.Azure.Iot.Device.Unified.Connection;
using Microsoft.Azure.Iot.Device.Unified.FileUpload;
using System.Net;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using Xunit;

namespace Microsoft.Azure.Iot.Device.UnitTests.Unified
{
    public class FileUploadClientUnitTests
    {
        private const string HostName = "some-hub.azure-devices.net";
        private const string DeviceId = "device-id";

        [Fact]
        public async Task SymmetricKeyAuthenticationSendsSasTokenInAuthorizationHeader()
        {
            var authentication = new SymmetricKeyAuthenticationProvider("registration-id", "0ne00000000", new byte[] { 0x01, 0x02, 0x03 });
            var handler = new CapturingHandler();
            using var fileUploadClient = new FileUploadClient(new FakeConnectionClient(authentication), new HttpClient(handler));

            await fileUploadClient.GetFileUploadSasUriAsync(new FileUploadSasUriRequest { BlobName = "file.txt" }, TestContext.Current.CancellationToken);
            await fileUploadClient.CompleteFileUploadSasUriAsync(new FileUploadCompletionNotification { CorrelationId = "id", IsSuccess = true }, TestContext.Current.CancellationToken);

            Assert.Equal(2, handler.AuthorizationHeaders.Count);
            foreach (string? authorization in handler.AuthorizationHeaders)
            {
                Assert.NotNull(authorization);
                Assert.StartsWith($"SharedAccessSignature sr={WebUtility.UrlEncode($"{HostName}/devices/{DeviceId}")}&sig=", authorization);
            }
        }

        [Fact]
        public async Task X509AuthenticationDoesNotSendAuthorizationHeader()
        {
            using RSA key = RSA.Create(2048);
            using X509Certificate2 certificate = new CertificateRequest($"CN={DeviceId}", key, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1)
                .CreateSelfSigned(DateTimeOffset.UtcNow.AddDays(-1), DateTimeOffset.UtcNow.AddDays(1));
            var handler = new CapturingHandler();
            using var fileUploadClient = new FileUploadClient(new FakeConnectionClient(new X509AuthenticationProvider(certificate)), new HttpClient(handler));

            await fileUploadClient.GetFileUploadSasUriAsync(new FileUploadSasUriRequest { BlobName = "file.txt" }, TestContext.Current.CancellationToken);

            Assert.Equal(new string?[] { null }, handler.AuthorizationHeaders);
        }

        [Fact]
        public async Task HttpClientIsRecreatedOnlyWhenClientCertificateChanges()
        {
            using X509Certificate2 firstCertificate = CreateCertificate();
            using X509Certificate2 secondCertificate = CreateCertificate();
            var authentication = new SwitchableAuthenticationProvider { Credentials = CreateSasCredentials() };
            var handlers = new List<(X509Certificate2? Certificate, CapturingHandler Handler)>();
            using var fileUploadClient = new FileUploadClient(new FakeConnectionClient(authentication), certificate =>
            {
                var handler = new CapturingHandler();
                handlers.Add((certificate, handler));
                return handler;
            });

            // SAS tokens are renewed per request, so they don't need a new HTTP client
            await GetSasUriAsync(fileUploadClient);
            authentication.Credentials = CreateSasCredentials();
            await GetSasUriAsync(fileUploadClient);
            Assert.Single(handlers);
            Assert.Null(handlers[0].Certificate);

            // An issued certificate replaces the SAS token
            authentication.Credentials = new AuthenticationCredentials { ClientCertificate = firstCertificate };
            await GetSasUriAsync(fileUploadClient);
            Assert.Equal(2, handlers.Count);
            Assert.Same(firstCertificate, handlers[1].Certificate);
            Assert.True(handlers[0].Handler.IsDisposed);
            Assert.Equal(new string?[] { null }, handlers[1].Handler.AuthorizationHeaders);

            // An equal certificate loaded into a different instance is not a change
            using X509Certificate2 firstCertificateCopy = X509CertificateLoader.LoadCertificate(firstCertificate.RawData);
            authentication.Credentials = new AuthenticationCredentials { ClientCertificate = firstCertificateCopy };
            await GetSasUriAsync(fileUploadClient);
            Assert.Equal(2, handlers.Count);

            // A rotated certificate replaces the previous one
            authentication.Credentials = new AuthenticationCredentials { ClientCertificate = secondCertificate };
            await GetSasUriAsync(fileUploadClient);
            Assert.Equal(3, handlers.Count);
            Assert.Same(secondCertificate, handlers[2].Certificate);
            Assert.True(handlers[1].Handler.IsDisposed);
            Assert.False(handlers[2].Handler.IsDisposed);
        }

        [Fact]
        public async Task ReplacedHttpClientIsDisposedOnlyAfterItsInFlightRequestCompletes()
        {
            using X509Certificate2 firstCertificate = CreateCertificate();
            using X509Certificate2 secondCertificate = CreateCertificate();
            var authentication = new SwitchableAuthenticationProvider { Credentials = new AuthenticationCredentials { ClientCertificate = firstCertificate } };
            var firstRequestGate = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
            var handlers = new List<CapturingHandler>();
            using var fileUploadClient = new FileUploadClient(new FakeConnectionClient(authentication), certificate =>
            {
                var handler = new CapturingHandler(handlers.Count == 0 ? firstRequestGate.Task : null);
                handlers.Add(handler);
                return handler;
            });

            Task<FileUploadSasUriResponse> firstRequest = GetSasUriAsync(fileUploadClient);
            await handlers[0].RequestStarted.WaitAsync(TestContext.Current.CancellationToken);

            authentication.Credentials = new AuthenticationCredentials { ClientCertificate = secondCertificate };
            await GetSasUriAsync(fileUploadClient);
            Assert.Equal(2, handlers.Count);
            Assert.False(handlers[0].IsDisposed);

            firstRequestGate.SetResult();
            await firstRequest;
            Assert.True(handlers[0].IsDisposed);
            Assert.False(handlers[1].IsDisposed);
        }

        private static Task<FileUploadSasUriResponse> GetSasUriAsync(FileUploadClient fileUploadClient)
        {
            return fileUploadClient.GetFileUploadSasUriAsync(new FileUploadSasUriRequest { BlobName = "file.txt" }, TestContext.Current.CancellationToken);
        }

        private static X509Certificate2 CreateCertificate()
        {
            using RSA key = RSA.Create(2048);
            return new CertificateRequest($"CN={DeviceId}", key, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1)
                .CreateSelfSigned(DateTimeOffset.UtcNow.AddDays(-1), DateTimeOffset.UtcNow.AddDays(1));
        }

        private static AuthenticationCredentials CreateSasCredentials()
        {
            return new AuthenticationCredentials { Password = Encoding.UTF8.GetBytes($"SharedAccessSignature sr=x&sig={Guid.NewGuid()}&se=1") };
        }

        private sealed class SwitchableAuthenticationProvider : AuthenticationProvider
        {
            public AuthenticationCredentials Credentials { get; set; } = new();

            public override Task<AuthenticationCredentials> GetOperationalAuthenticationCredentialsAsync(string hostName, string deviceId, IReadOnlyList<string>? issuedClientCertificates)
                => Task.FromResult(Credentials);

            public override AuthenticationCredentials GetBootAuthenticationCredentials() => Credentials;

            public override string GetRegistrationId() => DeviceId;
        }

        private sealed class CapturingHandler : HttpMessageHandler
        {
            private readonly Task? _gate;
            private readonly TaskCompletionSource _requestStarted = new(TaskCreationOptions.RunContinuationsAsynchronously);

            public CapturingHandler(Task? gate = null)
            {
                _gate = gate;
            }

            public List<string?> AuthorizationHeaders { get; } = new();

            public bool IsDisposed { get; private set; }

            public Task RequestStarted => _requestStarted.Task;

            protected override async Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken cancellationToken)
            {
                AuthorizationHeaders.Add(request.Headers.TryGetValues("Authorization", out var values) ? values.Single() : null);
                _requestStarted.TrySetResult();
                if (_gate != null)
                {
                    await _gate.WaitAsync(cancellationToken);
                }

                bool isNotification = request.RequestUri!.AbsolutePath.EndsWith("/notifications", StringComparison.Ordinal);
                return isNotification
                    ? new HttpResponseMessage(HttpStatusCode.NoContent)
                    : new HttpResponseMessage(HttpStatusCode.OK) { Content = new StringContent(SasUriResponseJson) };
            }

            protected override void Dispose(bool disposing)
            {
                IsDisposed = true;
                base.Dispose(disposing);
            }

            private const string SasUriResponseJson =
                "{\"correlationId\":\"id\",\"hostName\":\"account.blob.core.windows.net\",\"containerName\":\"container\",\"blobName\":\"file.txt\",\"sasToken\":\"?sig=x\"}";
        }

        private sealed class FakeConnectionClient : IConnectionClient
        {
            private readonly ConnectionContext _context;

            public FakeConnectionClient(AuthenticationProvider authentication)
            {
                _context = new ConnectionContext
                {
                    DeviceId = DeviceId,
                    IotHubHostName = HostName,
                    ConnectionProfile = IotHubConnectionType.Mqttv3,
                    AuthenticationProvider = authentication,
                };
            }

            public event Func<MqttPublishReceivedEventArgs, Task> PublishReceivedAsync { add { } remove { } }

            public ConnectionContext? GetCurrentConnectionContext() => _context;

            public Task<MqttPublishAck> PublishAsync(MqttPublish publish, CancellationToken cancellationToken = default) => throw new NotSupportedException();

            public Task<MqttSubscribeAck> SubscribeAsync(MqttSubscribe subscribe, CancellationToken cancellationToken = default) => throw new NotSupportedException();

            public Task<MqttUnsubscribeAck> UnsubscribeAsync(MqttUnsubscribe unsubscribe, CancellationToken cancellationToken = default) => throw new NotSupportedException();

            public void Dispose()
            {
            }
        }
    }
}
