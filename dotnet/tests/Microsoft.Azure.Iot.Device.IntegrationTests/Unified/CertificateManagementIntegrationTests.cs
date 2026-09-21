// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

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
    public class CertificateManagementIntegrationTests
    {
        [Theory(Timeout = Setup.TestTimeoutMilliseconds, Skip = "Disabled CI infrastructure temporarily")]
        [InlineData(true)]
        [InlineData(false)]
        public async Task TestCertificateManagementWithDpsAndHub(bool testAgainstClassicHub)
        {
            using CancellationTokenSource cts = new();
            cts.CancelAfter(Setup.TestTimeoutMilliseconds - 1000);

            if (!testAgainstClassicHub)
            {
                Assert.Skip("No AEG hub to test against yet");
            }

            string registrationId = Environment.GetEnvironmentVariable("IOT_DPS_GROUP_X509_REGISTRATION_ID")
                ?? throw new InvalidOperationException("Missing IOT_DPS_GROUP_X509_REGISTRATION_ID environment variable.");
            string certificatePem = DecodeBase64EnvironmentVariable("IOT_DPS_GROUP_X509_CERTIFICATE");
            string privateKeyPem = DecodeBase64EnvironmentVariable("IOT_DPS_GROUP_X509_KEY");

            using X509Certificate2 deviceCertificate = CreateX509CertificateFromKeyAndCert(certificatePem, privateKeyPem);
            X509AuthenticationProvider x509AuthenticationProvider = new(deviceCertificate);

            // Create initial CSR to be processed by DPS
            var (csrBase64, privateKey) = CertificateUtilities.GenerateCsrAndPrivateKey(registrationId, CertificateUtilities.CsrAlgorithm.RSA);

            ConnectionClient connectionClient = new()
            {
                HandleCertificateSigningCompleteAsync = (IssuedCertificates) =>
                {
                    // Convert to PEM and save
                    string pemChain = CertificateUtilities.ConvertToPem(IssuedCertificates);

                    using X509Certificate2 deviceCertTemp = CertificateUtilities.CreateCertificateWithPrivateKey(IssuedCertificates, privateKey);

                    // Export and reimport with Exportable flag
                    byte[] pfxBytes = deviceCertTemp.Export(X509ContentType.Pfx);
                    return Task.FromResult(new X509AuthenticationProvider(X509CertificateLoader.LoadPkcs12(pfxBytes, (string?)null, X509KeyStorageFlags.Exportable)));
                }
            };

            ProvisioningSettings provisioningSettings = new(DpsIdScope)
            {
                CertificateSigningRequest = new(privateKey, csrBase64),
            };

            ConnectionContext connectionContext = await RetryAroundAuthorizationAsync<ConnectionContext>(
                async () => await connectionClient.ProvisionAndConnectAsync(provisioningSettings, x509AuthenticationProvider, cancellationToken: TestContext.Current.CancellationToken),
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

        [Theory(Timeout = Setup.TestTimeoutMilliseconds, Skip = "Disabled CI infrastructure temporarily")]
        [InlineData(true)]
        [InlineData(false)]
        public async Task TestCertificateManagementWithDpsAndHubUsingHsmBackedKey(bool testAgainstClassicHub)
        {
            using CancellationTokenSource cts = new();
            cts.CancelAfter(Setup.TestTimeoutMilliseconds - 1000);

            if (!testAgainstClassicHub)
            {
                Assert.Skip("No AEG hub to test against yet");
            }

            string registrationId = Environment.GetEnvironmentVariable("IOT_DPS_GROUP_X509_REGISTRATION_ID")
                ?? throw new InvalidOperationException("Missing IOT_DPS_GROUP_X509_REGISTRATION_ID environment variable.");
            string certificatePem = DecodeBase64EnvironmentVariable("IOT_DPS_GROUP_X509_CERTIFICATE");
            string privateKeyPem = DecodeBase64EnvironmentVariable("IOT_DPS_GROUP_X509_KEY");

            // The enrollment identity used to bootstrap the initial DPS registration. This is unrelated to the
            // HSM-backed path under test below: it authenticates the enrollment, not the device's own issued
            // identity.
            using X509Certificate2 deviceCertificate = CreateX509CertificateFromKeyAndCert(certificatePem, privateKeyPem);
            X509AuthenticationProvider x509AuthenticationProvider = new(deviceCertificate);

            // Simulates the device's own identity key living inside an HSM: the RSA key material never leaves
            // MockHsmRsa, and every signing operation the SDK performs -- CSR generation here, and later the TLS
            // handshake -- is delegated to it rather than performed against an in-memory key this process holds.
            using RSA hsmHeldKey = RSA.Create(2048);
            using var mockHsmKey = new MockHsmRsa(hsmHeldKey);

            // Create initial CSR to be processed by DPS, signed by the mock HSM key.
            string csrBase64 = CertificateUtilities.GenerateCsrWithPrivateKey(registrationId, mockHsmKey);

            ConnectionClient connectionClient = new()
            {
                HandleCertificateSigningCompleteAsync = (IssuedCertificates) =>
                {
                    // Unlike the software-key path (TestCertificateManagementWithDpsAndHub), the issued leaf
                    // certificate is bound directly to the mock HSM key handle via the HSM-backed
                    // X509AuthenticationProvider constructor: no PFX export/reimport round-trip, and no private
                    // key material is ever held by this process outside the mock HSM.
                    byte[] leafCertBytes = Convert.FromBase64String(IssuedCertificates[0]);
                    using X509Certificate2 publicOnlyLeafCert = X509CertificateLoader.LoadCertificate(leafCertBytes);

                    return Task.FromResult(new X509AuthenticationProvider(publicOnlyLeafCert, mockHsmKey));
                }
            };

            ProvisioningSettings provisioningSettings = new(DpsIdScope)
            {
                CertificateSigningRequest = new(mockHsmKey, csrBase64),
            };

            ConnectionContext connectionContext = await RetryAroundAuthorizationAsync<ConnectionContext>(
                async () => await connectionClient.ProvisionAndConnectAsync(provisioningSettings, x509AuthenticationProvider, cancellationToken: TestContext.Current.CancellationToken),
                TestContext.Current.CancellationToken);

            Assert.NotNull(connectionContext.IssuedClientCertificates);
            Assert.NotEmpty(connectionContext.IssuedClientCertificates);
            // The composed certificate reports a private key even though it was never present in managed memory --
            // it is the mock HSM key driving every private-key operation.
            Assert.True(connectionContext.AuthenticationProvider.ClientCertificate.HasPrivateKey);

            var secondCsrBase64 = CertificateUtilities.GenerateCsrWithPrivateKey(connectionContext.DeviceId, mockHsmKey);
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