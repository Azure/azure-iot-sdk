// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using CaptureProxy;
using Microsoft.Azure.Devices.Provisioning.Service;
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

        [Fact(Timeout = Setup.TestTimeoutMilliseconds)]
        public async Task CanProvisionAndConnectWithSecondarySymmetricKeyWhenPrimaryKeyIsWrong()
        {
            string registrationId = Guid.NewGuid().ToString();
            byte[] enrolledPrimaryKey = RandomNumberGenerator.GetBytes(32);
            byte[] enrolledSecondaryKey = RandomNumberGenerator.GetBytes(32);
            IndividualEnrollment individualEnrollment = new(
                registrationId,
                new SymmetricKeyAttestation(Convert.ToBase64String(enrolledPrimaryKey), Convert.ToBase64String(enrolledSecondaryKey)));
            using (ProvisioningServiceClient provisioningServiceClient = Setup.GetDpsHubServiceClient())
            {
                await provisioningServiceClient.CreateOrUpdateIndividualEnrollmentAsync(individualEnrollment, TestContext.Current.CancellationToken);
            }

            // Neither DPS nor the IoT hub device that DPS creates knows this key, so they can only accept the secondary.
            SymmetricKeyAuthenticationProvider authentication = new(registrationId, Setup.DpsIdScope, RandomNumberGenerator.GetBytes(32), enrolledSecondaryKey);

            await using UnifiedDeviceTestContext testDeviceContext = await ProvisionAndConnectAsync(authentication, individualEnrollment);

            Assert.Equal(registrationId, testDeviceContext.ConnectionContext.DeviceId);
            Assert.False(string.IsNullOrEmpty(testDeviceContext.ConnectionContext.IotHubHostName));
            Assert.Same(testDeviceContext.ConnectionContext, testDeviceContext.ConnectionClient.GetCurrentConnectionContext());
        }

        [Fact(Timeout = Setup.TestTimeoutMilliseconds)]
        public async Task CanProvisionAndConnectWithSecondaryCertificateWhenPrimaryCertificateIsWrong()
        {
            string registrationId = Guid.NewGuid().ToString();
            X509Certificate2 enrolledCertificate = CreateCertificate(registrationId, out X509Certificate2 enrolledPublicCertificate);

            // Names the same device, so it yields the same registration id, but is not enrolled with DPS or registered
            // with the IoT hub, so they can only accept the secondary certificate.
            X509Certificate2 unenrolledCertificate = CreateCertificate(registrationId, out _);

            IndividualEnrollment individualEnrollment = new(registrationId, X509Attestation.CreateFromClientCertificates(enrolledPublicCertificate));
            using (ProvisioningServiceClient provisioningServiceClient = Setup.GetDpsHubServiceClient())
            {
                await provisioningServiceClient.CreateOrUpdateIndividualEnrollmentAsync(individualEnrollment, TestContext.Current.CancellationToken);
            }

            X509AuthenticationProvider authentication = new(unenrolledCertificate, enrolledCertificate);

            await using UnifiedDeviceTestContext testDeviceContext = await ProvisionAndConnectAsync(authentication, individualEnrollment);

            Assert.Equal(registrationId, testDeviceContext.ConnectionContext.DeviceId);
            Assert.False(string.IsNullOrEmpty(testDeviceContext.ConnectionContext.IotHubHostName));
            Assert.Same(testDeviceContext.ConnectionContext, testDeviceContext.ConnectionClient.GetCurrentConnectionContext());
        }

        private static async Task<UnifiedDeviceTestContext> ProvisionAndConnectAsync(AuthenticationProvider authentication, IndividualEnrollment individualEnrollment)
        {
            ConnectionClient connectionClient = new();
            try
            {
                ConnectionContext connectionContext = await Setup.RetryAroundAuthorizationAsync(
                    async () => await connectionClient.ProvisionAndConnectAsync(
                        new ProvisioningSettings(Setup.DpsIdScope),
                        authentication,
                        cancellationToken: TestContext.Current.CancellationToken),
                    TestContext.Current.CancellationToken);

                return new UnifiedDeviceTestContext()
                {
                    ConnectionClient = connectionClient,
                    ConnectionContext = connectionContext,
                    AuthenticationProvider = authentication,
                    IndividualEnrollment = individualEnrollment,
                };
            }
            catch
            {
                await UnifiedDeviceTestContext.CleanupAsync(connectionClient, connectionClient.GetCurrentConnectionContext(), individualEnrollment);
                throw;
            }
        }

        private static X509Certificate2 CreateCertificate(string registrationId, out X509Certificate2 publicCertificate)
        {
            string certificateId = Guid.NewGuid().ToString();
            string certificatePath = $"./{certificateId}.cer";
            string pfxPath = $"./{certificateId}.pfx";
            Setup.CreateTestCertificates(pfxPath, certificatePath, registrationId);
            try
            {
                publicCertificate = X509CertificateLoader.LoadCertificateFromFile(certificatePath);
                return X509CertificateLoader.LoadPkcs12FromFile(pfxPath, Setup.TestCertificatesPassword);
            }
            finally
            {
                File.Delete(certificatePath);
                File.Delete(pfxPath);
            }
        }
    }
}
