// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using CaptureProxy;
using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.MqttNetAdapter;
using Microsoft.Azure.Iot.Device.MQTTnetAdapter;
using Microsoft.Azure.Iot.Device.Unified.Connection;
using System;
using System.Collections.Generic;
using System.Net;
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

        /// <summary>
        /// Exercises the HSM-based X.509 authentication path: the device provisions through DPS and connects to
        /// IoT Hub using a private key that lives in a SoftHSM2 PKCS#11 token and never leaves it. Both the DPS and
        /// Hub TLS handshakes are signed inside the token through <see cref="SoftHsmRsaCredential"/>.
        /// <para>
        /// Consumes the outputs of <c>c/eng/setup-softhsm.sh</c> (<c>PKCS11_PROVIDER_MODULE</c> and
        /// <c>AZ_IOT_CLIENT_KEY_URI</c>) plus the DPS group enrollment certificate in
        /// <c>IOT_DPS_GROUP_X509_CERTIFICATE</c>, whose public key matches the token-held private key.
        /// </para>
        /// </summary>
        [Theory(Timeout = Setup.TestTimeoutMilliseconds)]
        [InlineData(true)]
        [InlineData(false)]
        public async Task CanConnectWithHsmBackedX509UsingSoftHsm(bool isClassicHub)
        {
            if (!isClassicHub)
            {
                Assert.Skip("No AEG hub to test against yet");
            }

            if (string.IsNullOrEmpty(Environment.GetEnvironmentVariable("PKCS11_PROVIDER_MODULE")))
            {
                Assert.Fail("SoftHSM token not provisioned. Run c/eng/setup-softhsm.sh and eval its exports first.");
            }

            string certificatePem = DecodeBase64EnvironmentVariable("IOT_DPS_GROUP_X509_CERTIFICATE");

            // The device private key is never read here: it lives in the SoftHSM token and the handshake signs
            // through it. Only the public certificate is supplied.
            using SoftHsmRsaCredential softHsmCredential = SoftHsmRsaCredential.Load(certificatePem);
            X509AuthenticationProvider x509AuthenticationProvider = softHsmCredential.CreateAuthenticationProvider();

            ConnectionClient connectionClient = new();
            ProvisioningSettings provisioningSettings = new(Setup.DpsIdScope);

            ConnectionContext connectionContext = await Setup.RetryAroundAuthorizationAsync<ConnectionContext>(
                async () => await connectionClient.ProvisionAndConnectAsync(provisioningSettings, x509AuthenticationProvider, cancellationToken: TestContext.Current.CancellationToken),
                TestContext.Current.CancellationToken);

            Assert.NotNull(connectionContext);
            Assert.False(string.IsNullOrEmpty(connectionContext.DeviceId));
            Assert.False(string.IsNullOrEmpty(connectionContext.IotHubHostName));

            await connectionClient.DisconnectAsync(TestContext.Current.CancellationToken);
        }

        private static string DecodeBase64EnvironmentVariable(string variableName)
        {
            string encodedValue = Environment.GetEnvironmentVariable(variableName)
                ?? throw new InvalidOperationException($"Missing {variableName} environment variable.");

            return Encoding.UTF8.GetString(Convert.FromBase64String(encodedValue));
        }
    }
}
