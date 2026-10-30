// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Authentication;
using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Models.CertificateManagement;
using Microsoft.Azure.Iot.Device.Provisioning;
using Microsoft.Azure.Iot.Device.Unified.Connection;
using Microsoft.Azure.Iot.Device.Unified.FileUpload;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using Xunit;
using Xunit.Sdk;
using static Microsoft.Azure.Iot.Device.IntegrationTests.Setup;

namespace Microsoft.Azure.Iot.Device.IntegrationTests.Unified
{
    [Trait("Category", "CertificateManagement")]
    public class CertificateManagementIntegrationTests
    {
        [Fact(Timeout = Setup.TestTimeoutMilliseconds)]
        public async Task CanProvisionWithSymmetricKeyAndCertificateSigningThenConnectWithIssuedCertificate()
        {
            using RSA privateKey = RSA.Create(2048);
            await using UnifiedDeviceTestContext device = await CreateConnectedUnifiedConnectionClientWithSymmetricKeyAsync(
                cancellationToken: TestContext.Current.CancellationToken,
                issuedCertificatePrivateKey: privateKey);

            SymmetricKeyAuthenticationProvider authentication = Assert.IsType<SymmetricKeyAuthenticationProvider>(device.AuthenticationProvider);
            ConnectionContext connectionContext = device.ConnectionContext;
            Assert.Null(device.IndividualEnrollment);
            Assert.Equal(device.RegistrationId, connectionContext.DeviceId);
            Assert.Same(connectionContext, device.ConnectionClient.GetCurrentConnectionContext());
            Assert.NotNull(connectionContext.IssuedClientCertificates);
            Assert.NotEmpty(connectionContext.IssuedClientCertificates);

            await device.ConnectionClient.DisconnectAsync(TestContext.Current.CancellationToken);
            Assert.Same(connectionContext, device.ConnectionClient.GetCurrentConnectionContext());

            ProvisioningSettings provisioningSettings = new(DpsIdScope)
            {
                CertificateSigningRequest = new(privateKey, CertificateUtilities.GenerateCsrWithPrivateKey(device.RegistrationId, privateKey)),
            };
            ConnectionContext reconnectedContext = await device.ConnectionClient.ProvisionAndConnectAsync(
                provisioningSettings, authentication, cancellationToken: TestContext.Current.CancellationToken);

            // Re-provisioning would return a new context, even if DPS assigned the same device and hub.
            Assert.Same(connectionContext, reconnectedContext);
            Assert.Same(connectionContext, device.ConnectionClient.GetCurrentConnectionContext());

            // A successful Hub connection with an issued certificate must not fall back to a SAS token.
            AuthenticationCredentials hubCredentials = await authentication.GetPrimaryOperationalAuthenticationCredentialsAsync(
                connectionContext.IotHubHostName,
                connectionContext.DeviceId,
                connectionContext.IssuedClientCertificates,
                TestContext.Current.CancellationToken);
            using X509Certificate2 issuedCertificate = X509CertificateLoader.LoadCertificate(Convert.FromBase64String(connectionContext.IssuedClientCertificates[0]));
            Assert.NotNull(hubCredentials.ClientCertificate);
            Assert.Equal(issuedCertificate.Thumbprint, hubCredentials.ClientCertificate.Thumbprint);
            Assert.True(hubCredentials.ClientCertificate.HasPrivateKey);
            Assert.Empty(hubCredentials.Password);

            AuthenticationCredentials dpsCredentials = await authentication.GetPrimaryBootAuthenticationCredentialsAsync(TestContext.Current.CancellationToken);
            Assert.Null(dpsCredentials.ClientCertificate);
            Assert.StartsWith("SharedAccessSignature ", Encoding.UTF8.GetString(dpsCredentials.Password));
        }

        [Fact(Timeout = Setup.TestTimeoutMilliseconds)]
        public async Task FileUploadUsesIssuedCertificateInsteadOfSymmetricKey()
        {
            using RSA privateKey = RSA.Create(2048);
            UnifiedDeviceTestContext device = await CreateConnectedUnifiedConnectionClientWithSymmetricKeyAsync(
                cancellationToken: TestContext.Current.CancellationToken,
                issuedCertificatePrivateKey: privateKey);

            FileUploadClient fileUploadClient = new(device.ConnectionClient);
            try
            {
                ConnectionContext connectionContext = device.ConnectionContext;
                Assert.NotNull(connectionContext.IssuedClientCertificates);
                Assert.NotEmpty(connectionContext.IssuedClientCertificates);

                // The provider can mint SAS tokens, but once a certificate is issued the file upload client must authenticate with it instead
                AuthenticationCredentials hubCredentials = await device.AuthenticationProvider.GetPrimaryOperationalAuthenticationCredentialsAsync(
                    connectionContext.IotHubHostName,
                    connectionContext.DeviceId,
                    connectionContext.IssuedClientCertificates,
                    TestContext.Current.CancellationToken);
                using X509Certificate2 issuedCertificate = X509CertificateLoader.LoadCertificate(Convert.FromBase64String(connectionContext.IssuedClientCertificates[0]));
                Assert.NotNull(hubCredentials.ClientCertificate);
                Assert.Equal(issuedCertificate.Thumbprint, hubCredentials.ClientCertificate.Thumbprint);
                Assert.Empty(hubCredentials.Password);

                await FileUploadIntegrationTests.UploadFileAndCompleteAsync(fileUploadClient, TestContext.Current.CancellationToken);
            }
            finally
            {
                try
                {
                    await device.DisposeAsync(); // Clean up the device before disposing feature clients.
                }
                finally
                {
                    fileUploadClient.Dispose();
                }
            }
        }

        [Theory(Timeout = Setup.TestTimeoutMilliseconds)]
        [InlineData(true)]
        [InlineData(false)]
        public async Task CanProvisionWithX509AndCertificateSigningThenConnectWithIssuedCertificate(bool testAgainstClassicHub)
        {
            using CancellationTokenSource cts = new();
            cts.CancelAfter(Setup.TestTimeoutMilliseconds - 1000);

            if (!testAgainstClassicHub)
            {
                Assert.Skip("No MQTTv5 hub to test against yet");
            }

            string registrationId = Environment.GetEnvironmentVariable("IOT_DPS_GROUP_X509_REGISTRATION_ID")
                ?? throw new InvalidOperationException("Missing IOT_DPS_GROUP_X509_REGISTRATION_ID environment variable.");
            string certificatePem = DecodeBase64EnvironmentVariable("IOT_DPS_GROUP_X509_CERTIFICATE");
            string privateKeyPem = DecodeBase64EnvironmentVariable("IOT_DPS_GROUP_X509_KEY");

            using X509Certificate2 deviceCertificate = CreateX509CertificateFromKeyAndCert(certificatePem, privateKeyPem);

            // Create initial CSR to be processed by DPS
            var (csrBase64, privateKey) = CertificateUtilities.GenerateCsrAndPrivateKey(registrationId, CertificateUtilities.CsrAlgorithm.RSA);
            X509AuthenticationProvider x509AuthenticationProvider = new(deviceCertificate, null, privateKey);

            ConnectionClient connectionClient = new();

            ProvisioningSettings provisioningSettings = new(DpsIdScope)
            {
                CertificateSigningRequest = new(privateKey, csrBase64),
            };

            ConnectionContext connectionContext = await RetryAroundAuthorizationAsync<ConnectionContext>(
                async () => await connectionClient.ProvisionAndConnectAsync(provisioningSettings, x509AuthenticationProvider, cancellationToken: TestContext.Current.CancellationToken),
                TestContext.Current.CancellationToken);

            Assert.NotNull(connectionContext.IssuedClientCertificates);
            Assert.NotEmpty(connectionContext.IssuedClientCertificates);

            await connectionClient.DisconnectAsync(cts.Token);
            Assert.Same(connectionContext, connectionClient.GetCurrentConnectionContext());

            ConnectionContext reconnectedContext = await connectionClient.ProvisionAndConnectAsync(
                provisioningSettings, x509AuthenticationProvider, cancellationToken: cts.Token);

            // Re-provisioning would return a new context, even if DPS assigned the same device and hub.
            Assert.Same(connectionContext, reconnectedContext);
            Assert.Same(connectionContext, connectionClient.GetCurrentConnectionContext());

            var secondCsrBase64 = CertificateUtilities.GenerateCsrWithPrivateKey(connectionContext.DeviceId, privateKey);
            var certificateSigningRequest = new IotHubCertificateSigningRequest(connectionContext.DeviceId, secondCsrBase64, null, "*");

            CertificateSigningOperation pendingCsr = await connectionClient.SendCertificateSigningRequestAsync(certificateSigningRequest, cts.Token);

            try
            {
                await pendingCsr.Accepted.WaitAsync(cts.Token);
            }
            catch (CertificateSigningRequestFailedException e)
            {
                Assert.Fail(e.Error.Message);
            }

            CertificateSigningResponse? hubCsrResponse = null;
            try
            {
                hubCsrResponse = await pendingCsr.Completed.WaitAsync(cts.Token);
            }
            catch (CertificateSigningRequestFailedException e)
            {
                Assert.Fail(e.Error.Message);
            }

            // Upon getting the newly signed certificate, disconnect from IoT Hub and then reconnect with that new certificate
            await connectionClient.DisconnectAsync(cts.Token);

            Assert.NotNull(hubCsrResponse.Certificates);
            Assert.NotEmpty(hubCsrResponse.Certificates);

            await connectionClient.DisconnectAsync(cts.Token);
        }

        private static X509Certificate2 CreateX509CertificateFromKeyAndCert(string certificate, string key)
        {
            using X509Certificate2 cert = X509Certificate2.CreateFromPem(certificate, key);

            byte[] certificateBytes = cert.Export(X509ContentType.Pfx);
            return X509CertificateLoader.LoadPkcs12(certificateBytes, null, X509KeyStorageFlags.Exportable);
        }

        private static string DecodeBase64EnvironmentVariable(string variableName)
        {
            string encodedValue = Environment.GetEnvironmentVariable(variableName)
                ?? throw new InvalidOperationException($"Missing {variableName} environment variable.");

            return Encoding.UTF8.GetString(Convert.FromBase64String(encodedValue));
        }
    }
}