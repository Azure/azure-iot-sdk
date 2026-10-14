// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Authentication;
using Microsoft.Azure.Iot.Device.Authentication.Boot;
using Microsoft.Azure.Iot.Device.Authentication.Operational;
using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Models.CertificateManagement;
using Microsoft.Azure.Iot.Device.Provisioning;
using Microsoft.Azure.Iot.Device.Unified.Connection;
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
        [Theory(Timeout = Setup.TestTimeoutMilliseconds)]
        [InlineData(true)]
        [InlineData(false)]
        public async Task TestCertificateManagementWithDpsAndHub(bool testAgainstClassicHub)
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
            X509BootAuthenticationProvider bootAuthenticationProvider = new(deviceCertificate);
            X509OperationalAuthenticationProvider operationalAuthenticationProvider = new(privateKey);

            ConnectionClient connectionClient = new();

            ProvisioningSettings provisioningSettings = new(DpsIdScope)
            {
                CertificateSigningRequest = new(privateKey, csrBase64),
            };

            ConnectionContext connectionContext = await RetryAroundAuthorizationAsync<ConnectionContext>(
                async () => await connectionClient.ProvisionAndConnectAsync(provisioningSettings, bootAuthenticationProvider, operationalAuthenticationProvider, cancellationToken: TestContext.Current.CancellationToken),
                TestContext.Current.CancellationToken);

            Assert.NotNull(connectionContext.IssuedClientCertificates);
            Assert.NotEmpty(connectionContext.IssuedClientCertificates);

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

        // Requires a DPS with certificate management enabled and a symmetric-key enrollment group linked to its signing
        // CA, so the class's CertificateManagement trait routes it to ci-dotnet-e2e-csr rather than ci-dotnet.
        [Fact(Timeout = Setup.TestTimeoutMilliseconds)]
        public async Task CanProvisionWithSymmetricKeyAndCertificateSigningThenConnectWithIssuedCertificate()
        {
            string? groupKey = Environment.GetEnvironmentVariable("IOT_DPS_SYMM_KEY_GROUP_PRIMARY_KEY");
            if (string.IsNullOrWhiteSpace(groupKey))
            {
                Assert.Skip("IOT_DPS_SYMM_KEY_GROUP_PRIMARY_KEY is not set; this test needs a symmetric-key enrollment group linked to a certificate policy.");
            }

            string registrationId = $"sas-csr-{Guid.NewGuid()}";
            using HMACSHA256 hmac = new(Convert.FromBase64String(groupKey));
            string deviceKey = Convert.ToBase64String(hmac.ComputeHash(Encoding.UTF8.GetBytes(registrationId)));

            var (csrBase64, privateKey) = CertificateUtilities.GenerateCsrAndPrivateKey(registrationId, CertificateUtilities.CsrAlgorithm.RSA);

            SymmetricKeyBootAuthenticationProvider bootAuthentication = new(registrationId, Setup.DpsIdScope, deviceKey);
            X509OperationalAuthenticationProvider operationalAuthentication = new(privateKey);
            ProvisioningSettings provisioningSettings = new(Setup.DpsIdScope)
            {
                CertificateSigningRequest = new(privateKey, csrBase64),
            };

            ConnectionClient connectionClient = new();
            ConnectionContext? connectionContext = null;
            try
            {
                // Reaching the hub at all proves it accepted the issued certificate: once DPS issues one, the provider
                // no longer offers the hub a SAS token.
                connectionContext = await Setup.RetryAroundAuthorizationAsync<ConnectionContext>(
                    async () => await connectionClient.ProvisionAndConnectAsync(provisioningSettings, bootAuthentication, operationalAuthentication, cancellationToken: TestContext.Current.CancellationToken),
                    TestContext.Current.CancellationToken);

                Assert.Equal(registrationId, connectionContext.DeviceId);
                Assert.NotNull(connectionContext.IssuedClientCertificates);
                Assert.NotEmpty(connectionContext.IssuedClientCertificates);

                AuthenticationCredentials hubCredentials = await operationalAuthentication.GetCredentialsAsync(
                    connectionContext.IotHubHostName,
                    connectionContext.DeviceId,
                    connectionContext.IssuedClientCertificates,
                    TestContext.Current.CancellationToken);
                using X509Certificate2 issuedCertificate = X509CertificateLoader.LoadCertificate(Convert.FromBase64String(connectionContext.IssuedClientCertificates[0]));
                Assert.NotNull(hubCredentials.ClientCertificate);
                Assert.Equal(issuedCertificate.Thumbprint, hubCredentials.ClientCertificate.Thumbprint);
                Assert.True(hubCredentials.ClientCertificate.HasPrivateKey);
                Assert.Empty(hubCredentials.Password);

                // DPS is still authenticated with a SAS token derived from the symmetric key.
                AuthenticationCredentials dpsCredentials = await bootAuthentication.GetCredentialsAsync(TestContext.Current.CancellationToken);
                Assert.Null(dpsCredentials.ClientCertificate);
                Assert.StartsWith("SharedAccessSignature ", Encoding.UTF8.GetString(dpsCredentials.Password));
            }
            finally
            {
                await connectionClient.DisconnectAsync(TestContext.Current.CancellationToken);
                connectionClient.Dispose();

                if (connectionContext != null)
                {
                    try
                    {
                        using var registryManager = Setup.GetMQTTv3IotHubRegistryManager();
                        await registryManager.RemoveDeviceAsync(connectionContext.DeviceId, TestContext.Current.CancellationToken);
                    }
                    catch (Exception)
                    {
                        // Best-effort cleanup; the device registered through an enrollment group, so there is no
                        // individual enrollment to remove.
                    }
                }
            }
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