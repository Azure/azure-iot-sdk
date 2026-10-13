// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using CaptureProxy;
using Microsoft.Azure.Iot.Device.Authentication;
using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.MqttNetAdapter;
using Microsoft.Azure.Iot.Device.MQTTnetAdapter;
using Microsoft.Azure.Iot.Device.Unified.Connection;
using System;
using System.Collections.Generic;
using System.Net;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using Xunit;

namespace Microsoft.Azure.Iot.Device.IntegrationTests.Unified
{
    public class ConnectionClientIntegrationTests
    {
        [Theory(Timeout = Setup.TestTimeoutMilliseconds)]
        [InlineData(true)]
        [InlineData(false)]
        public async Task CanConnectOverWebsocket(bool isClassicHub)
        {
            MqttNetClientOptions mqttClientOptions = new()
            {
                UseWebsocket = true,
            };

            ConnectionClientOptions connectionClientOptions = new()
            {
                MqttClient = new MqttNetClient(mqttClientOptions)
            };

            UnifiedDeviceTestContext testDeviceContext = await Setup.CreateConnectedUnifiedConnectionClientAsync(isClassicHub, connectionClientOptions, TestContext.Current.CancellationToken);

            await testDeviceContext.DisposeAsync(); // Dispose this before any feature clients so that the test device identity can be cleaned up and the MQTT client disconnected gracefully
        }

        [Theory(Timeout = Setup.TestTimeoutMilliseconds)]
        [InlineData(true)]
        [InlineData(false)]
        public async Task CanConnectOverWebsocketAndHttpProxy(bool isClassicHub)
        {
            int proxyPort = isClassicHub ? 8877 : 8878; // Use a different port per test so they don't collide if run in parallel
            var httpProxy = new HttpProxy(proxyPort);

            // Start the proxy server
            httpProxy.Start();


            MqttNetClientOptions mqttClientOptions = new()
            {
                UseWebsocket = true,
                Proxy = new WebProxy("localhost", proxyPort)
            };

            ConnectionClientOptions connectionClientOptions = new()
            {
                MqttClient = new MqttNetClient(mqttClientOptions)
            };

            UnifiedDeviceTestContext testDeviceContext = await Setup.CreateConnectedUnifiedConnectionClientAsync(isClassicHub, connectionClientOptions, TestContext.Current.CancellationToken);

            await testDeviceContext.DisposeAsync(); // Dispose this before any feature clients so that the test device identity can be cleaned up and the MQTT client disconnected gracefully


            // Stop the proxy server
            httpProxy.Stop();
            httpProxy.Dispose();
        }

        [Fact(Timeout = Setup.TestTimeoutMilliseconds)]
        public async Task CanProvisionAndConnectWithSymmetricKeyIndividualEnrollment()
        {
            // The same key signs the DPS SAS token and, after provisioning, the IoT hub SAS token
            await using UnifiedDeviceTestContext testDeviceContext = await Setup.CreateConnectedUnifiedConnectionClientWithSymmetricKeyAsync(cancellationToken: TestContext.Current.CancellationToken);

            Assert.IsType<SymmetricKeyAuthenticationProvider>(testDeviceContext.AuthenticationProvider);
            Assert.False(string.IsNullOrEmpty(testDeviceContext.ConnectionContext.IotHubHostName));
            Assert.Equal(testDeviceContext.RegistrationId, testDeviceContext.ConnectionContext.DeviceId);
            Assert.Equal(
                testDeviceContext.ConnectionContext.IotHubHostName,
                testDeviceContext.ConnectionClient.GetCurrentConnectionContext()?.IotHubHostName);
        }

        // Requires a DPS with certificate management enabled and a symmetric-key enrollment group linked to its signing
        // CA, so it runs in the ci-dotnet-e2e-csr pipeline rather than ci-dotnet (matched by "SymmetricKeyAndCertificateSigning").
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

            SymmetricKeyWithIssuedCertificateAuthenticationProvider authentication = new(registrationId, Setup.DpsIdScope, deviceKey, privateKey);
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
                    async () => await connectionClient.ProvisionAndConnectAsync(provisioningSettings, authentication, cancellationToken: TestContext.Current.CancellationToken),
                    TestContext.Current.CancellationToken);

                Assert.Equal(registrationId, connectionContext.DeviceId);
                Assert.NotNull(connectionContext.IssuedClientCertificates);
                Assert.NotEmpty(connectionContext.IssuedClientCertificates);

                AuthenticationCredentials hubCredentials = authentication.GetIotHubAuthentication(
                    connectionContext.IotHubHostName,
                    connectionContext.DeviceId,
                    connectionContext.IssuedClientCertificates);
                using X509Certificate2 issuedCertificate = X509CertificateLoader.LoadCertificate(Convert.FromBase64String(connectionContext.IssuedClientCertificates[0]));
                Assert.NotNull(hubCredentials.ClientCertificate);
                Assert.Equal(issuedCertificate.Thumbprint, hubCredentials.ClientCertificate.Thumbprint);
                Assert.True(hubCredentials.ClientCertificate.HasPrivateKey);
                Assert.Empty(hubCredentials.Password);

                // DPS is still authenticated with a SAS token derived from the symmetric key.
                AuthenticationCredentials dpsCredentials = authentication.GetDeviceProvisioningServiceAuthentication();
                Assert.Null(dpsCredentials.ClientCertificate);
                Assert.StartsWith("SharedAccessSignature ", Encoding.UTF8.GetString(dpsCredentials.Password));
            }
            finally
            {
                await connectionClient.DisconnectAsync();
                connectionClient.Dispose();

                if (connectionContext != null)
                {
                    try
                    {
                        using var registryManager = Setup.GetMQTTv3IotHubRegistryManager();
                        await registryManager.RemoveDeviceAsync(connectionContext.DeviceId);
                    }
                    catch (Exception)
                    {
                        // Best-effort cleanup; the device registered through an enrollment group, so there is no
                        // individual enrollment to remove.
                    }
                }
            }
        }
    }
}
