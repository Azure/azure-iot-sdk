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

        private sealed class CapturingHandler : HttpMessageHandler
        {
            public List<string?> AuthorizationHeaders { get; } = new();

            protected override Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken cancellationToken)
            {
                AuthorizationHeaders.Add(request.Headers.TryGetValues("Authorization", out var values) ? values.Single() : null);
                bool isNotification = request.RequestUri!.AbsolutePath.EndsWith("/notifications", StringComparison.Ordinal);
                return Task.FromResult(isNotification
                    ? new HttpResponseMessage(HttpStatusCode.NoContent)
                    : new HttpResponseMessage(HttpStatusCode.OK) { Content = new StringContent(SasUriResponseJson) });
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
