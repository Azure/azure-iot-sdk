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

            // Simulates the device's own identity key living inside an HSM: the RSA key material never leaves the
            // SoftHSM2 token, and every signing operation the SDK performs -- CSR generation here, and later the
            // TLS handshake -- is delegated to it via PKCS#11 (Pkcs11Interop) rather than performed against an
            // in-memory key this process holds. This mirrors the C SDK's e2e "key custody" test
            // (c/tests/e2e/tests/e2e_custody_test.c): the SAME provisioning script, c/eng/setup-softhsm.sh, is run
            // by CI (see ci-dotnet.yml) to create the token this test opens, and the environment variable names
            // below are exactly the ones that script exports -- no .NET-specific renaming -- so both SDKs'
            // pipelines provision a token the same way.
            string pkcs11LibraryPath = Environment.GetEnvironmentVariable("PKCS11_PROVIDER_MODULE")
                ?? throw new InvalidOperationException("Missing PKCS11_PROVIDER_MODULE environment variable (path to libsofthsm2.so/.dll, exported by c/eng/setup-softhsm.sh).");
            string tokenLabel = Environment.GetEnvironmentVariable("AZ_IOT_PKCS11_TOKEN_LABEL")
                ?? throw new InvalidOperationException("Missing AZ_IOT_PKCS11_TOKEN_LABEL environment variable (exported by c/eng/setup-softhsm.sh).");
            string keyLabel = Environment.GetEnvironmentVariable("AZ_IOT_PKCS11_KEY_LABEL")
                ?? throw new InvalidOperationException("Missing AZ_IOT_PKCS11_KEY_LABEL environment variable (exported by c/eng/setup-softhsm.sh).");
            string userPin = Environment.GetEnvironmentVariable("AZ_IOT_PKCS11_PIN")
                ?? throw new InvalidOperationException("Missing AZ_IOT_PKCS11_PIN environment variable (exported by c/eng/setup-softhsm.sh).");

            using SoftHsmRsaKey hsmKey = SoftHsmRsaKey.Open(pkcs11LibraryPath, tokenLabel, keyLabel, userPin);

            // Create initial CSR to be processed by DPS, signed by the SoftHSM2-held key.
            string csrBase64 = CertificateUtilities.GenerateCsrWithPrivateKey(registrationId, hsmKey);

            ConnectionClient connectionClient = new()
            {
                HandleCertificateSigningCompleteAsync = (IssuedCertificates) =>
                {
                    // Unlike the software-key path (TestCertificateManagementWithDpsAndHub), the issued leaf
                    // certificate is bound directly to the token-backed key handle via the HSM-backed
                    // X509AuthenticationProvider constructor: no PFX export/reimport round-trip, and no private
                    // key material is ever held by this process outside the token.
                    byte[] leafCertBytes = Convert.FromBase64String(IssuedCertificates[0]);
                    using X509Certificate2 publicOnlyLeafCert = X509CertificateLoader.LoadCertificate(leafCertBytes);

                    return Task.FromResult(new X509AuthenticationProvider(publicOnlyLeafCert, hsmKey));
                }
            };

            ProvisioningSettings provisioningSettings = new(DpsIdScope)
            {
                CertificateSigningRequest = new(hsmKey, csrBase64),
            };

            ConnectionContext connectionContext = await RetryAroundAuthorizationAsync<ConnectionContext>(
                async () => await connectionClient.ProvisionAndConnectAsync(provisioningSettings, x509AuthenticationProvider, cancellationToken: TestContext.Current.CancellationToken),
                TestContext.Current.CancellationToken);

            Assert.NotNull(connectionContext.IssuedClientCertificates);
            Assert.NotEmpty(connectionContext.IssuedClientCertificates);
            // The composed certificate reports a private key even though it was never present in managed memory --
            // it is the SoftHSM2 token driving every private-key operation.
            Assert.True(connectionContext.AuthenticationProvider.ClientCertificate.HasPrivateKey);

            var secondCsrBase64 = CertificateUtilities.GenerateCsrWithPrivateKey(connectionContext.DeviceId, hsmKey);
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