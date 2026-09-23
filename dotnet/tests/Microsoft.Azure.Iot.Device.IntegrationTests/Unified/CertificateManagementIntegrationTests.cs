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

        /// <summary>
        /// Exercises the same certificate-management code path as <see cref="TestCertificateManagementWithDpsAndHub"/>,
        /// but the device authenticates with a private key held in a SoftHSM2 PKCS#11 token that never leaves it.
        /// <para>
        /// This consumes the outputs of <c>c/eng/setup-softhsm.sh</c> (the same setup the C project's custody tests
        /// use): that script imports the device key matching <c>IOT_DPS_GROUP_X509_CERTIFICATE</c> into a token and
        /// exports <c>PKCS11_PROVIDER_MODULE</c> and <c>AZ_IOT_CLIENT_KEY_URI</c>, which <see cref="SoftHsmRsaCredential"/>
        /// reads. Only the DPS handshake authentication differs from the base test; the CSR flow is identical.
        /// </para>
        /// </summary>
        [Theory(Timeout = Setup.TestTimeoutMilliseconds, Skip = "Disabled CI infrastructure temporarily")]
        [InlineData(true)]
        [InlineData(false)]
        public async Task TestCertificateManagementWithDpsAndHubUsingSoftHsm(bool testAgainstClassicHub)
        {
            using CancellationTokenSource cts = new();
            cts.CancelAfter(Setup.TestTimeoutMilliseconds - 1000);

            if (!testAgainstClassicHub)
            {
                Assert.Skip("No AEG hub to test against yet");
            }

            if (string.IsNullOrEmpty(Environment.GetEnvironmentVariable("PKCS11_PROVIDER_MODULE")))
            {
                Assert.Fail("SoftHSM token not provisioned. Run c/eng/setup-softhsm.sh and eval its exports first.");
            }

            string registrationId = Environment.GetEnvironmentVariable("IOT_DPS_GROUP_X509_REGISTRATION_ID")
                ?? throw new InvalidOperationException("Missing IOT_DPS_GROUP_X509_REGISTRATION_ID environment variable.");
            string certificatePem = DecodeBase64EnvironmentVariable("IOT_DPS_GROUP_X509_CERTIFICATE");

            // The device private key is not read from the environment here: it lives in the SoftHSM token and the
            // handshake signs through it. The certificate is public and still supplied as PEM.
            using SoftHsmRsaCredential softHsmCredential = SoftHsmRsaCredential.Load(certificatePem);
            X509AuthenticationProvider x509AuthenticationProvider = softHsmCredential.CreateAuthenticationProvider();

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