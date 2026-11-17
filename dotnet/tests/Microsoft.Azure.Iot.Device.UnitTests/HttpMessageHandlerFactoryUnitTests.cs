// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Authentication;
using System.Net.Security;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using Xunit;

namespace Microsoft.Azure.Iot.Device.UnitTests
{
    public class HttpMessageHandlerFactoryUnitTests
    {
        [Fact]
        public void HandlerAppliesCredentialTlsSettings()
        {
            using X509Certificate2 certificate = CreateCertificate();
            RemoteCertificateValidationCallback validation = (_, _, _, errors) => errors == SslPolicyErrors.None;
            LocalCertificateSelectionCallback selection = (_, _, _, _, _) => null!;

            using var handler = Assert.IsType<SocketsHttpHandler>(HttpMessageHandlerFactory.Create(new AuthenticationCredentials
            {
                ClientCertificate = certificate,
                RemoteCertificateValidationCallback = validation,
                LocalCertificateSelectionCallback = selection,
            }, proxy: null));

            Assert.Same(validation, handler.SslOptions.RemoteCertificateValidationCallback);
            Assert.Same(selection, handler.SslOptions.LocalCertificateSelectionCallback);
            Assert.Same(certificate, Assert.Single(handler.SslOptions.ClientCertificates!));
        }

        [Fact]
        public void HandlerPresentsClientCertificateWithoutSelectionCallback()
        {
            using X509Certificate2 certificate = CreateCertificate();

            using var handler = Assert.IsType<SocketsHttpHandler>(HttpMessageHandlerFactory.Create(new AuthenticationCredentials { ClientCertificate = certificate }, proxy: null));

            Assert.Null(handler.SslOptions.RemoteCertificateValidationCallback);
            Assert.NotNull(handler.SslOptions.LocalCertificateSelectionCallback);
            Assert.Same(certificate, handler.SslOptions.LocalCertificateSelectionCallback(this, "some-hub.azure-devices.net", new X509CertificateCollection(), null, Array.Empty<string>()));
        }

        [Fact]
        public void HandlerHasNoClientCertificateForSasCredentials()
        {
            using var handler = Assert.IsType<SocketsHttpHandler>(HttpMessageHandlerFactory.Create(new AuthenticationCredentials
            {
                Password = Encoding.UTF8.GetBytes("SharedAccessSignature sr=x&sig=y&se=1"),
            }, proxy: null));

            Assert.Null(handler.SslOptions.ClientCertificates);
            Assert.Null(handler.SslOptions.LocalCertificateSelectionCallback);
        }

        private static X509Certificate2 CreateCertificate()
        {
            using RSA key = RSA.Create(2048);
            return new CertificateRequest("CN=some-device", key, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1)
                .CreateSelfSigned(DateTimeOffset.UtcNow.AddDays(-1), DateTimeOffset.UtcNow.AddDays(1));
        }
    }
}
