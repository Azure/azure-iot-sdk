// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Mqtt;
using Microsoft.Azure.Iot.Device.Provisioning.Models;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text.Json;
using Xunit;

namespace Microsoft.Azure.Iot.Device.UnitTests
{
    /// <summary>
    /// Tests for how <see cref="AbstractConnectionClient"/> handles malformed Device Provisioning Service responses.
    /// </summary>
    public class ProvisioningResponseUnitTests
    {
        private const string GlobalDeviceEndpoint = "global.azure-devices-provisioning.net";
        private const string RegisterTopicPrefix = "$dps/registrations/PUT/";
        private const string GetOperationStatusTopicPrefix = "$dps/registrations/GET/";

        private static readonly TimeSpan s_testTimeout = TimeSpan.FromSeconds(30);

        [Theory]
        [InlineData(null)]
        [InlineData("")]
        [InlineData("   ")]
        public async Task RegistrationResponseWithoutOperationIdFailsProvisioningWithoutPolling(string? operationId)
        {
            using MockConnectionMqttClient mockMqttClient = new();
            int statusPolls = 0;
            mockMqttClient.OnPublish = async publish =>
            {
                if (publish.Topic.StartsWith(RegisterTopicPrefix, StringComparison.Ordinal))
                {
                    await mockMqttClient.SimulatePublishReceivedAsync(new MqttPublish()
                    {
                        Topic = "$dps/registrations/res/202/?$rid=1",
                        Payload = JsonSerializer.SerializeToUtf8Bytes(
                            new RegistrationOperationStatus()
                            {
                                OperationId = operationId,
                                Status = ProvisioningRegistrationStatus.Assigning,
                            },
                            JsonSerializationSettings.Options),
                    });
                }
                else if (publish.Topic.StartsWith(GetOperationStatusTopicPrefix, StringComparison.Ordinal))
                {
                    Interlocked.Increment(ref statusPolls);
                }

                return new MqttPublishAck() { ReasonCode = MqttPublishAckReasonCode.Success };
            };

            using TestConnectionClient connectionClient = new(new() { MqttClient = mockMqttClient });

            Exception thrown = await Assert.ThrowsAnyAsync<Exception>(
                () => connectionClient
                    .ProvisionAndConnectAsync(
                        new ProvisioningSettings("0ne00000000")
                        {
                            RegistrationId = "someRegistrationId",
                            GlobalEndpointAddress = GlobalDeviceEndpoint,
                        },
                        CreateAuthenticationProvider(),
                        TestContext.Current.CancellationToken)
                    .WaitAsync(s_testTimeout, TestContext.Current.CancellationToken));

            Assert.IsType<InvalidOperationException>(thrown.InnerException ?? thrown);
            Assert.Equal(0, Volatile.Read(ref statusPolls));
        }

        private static X509AuthenticationProvider CreateAuthenticationProvider()
        {
            using RSA key = RSA.Create(2048);
            var certificateRequest = new CertificateRequest("CN=someRegistrationId", key, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);
            X509Certificate2 certificate = certificateRequest.CreateSelfSigned(DateTimeOffset.UtcNow.AddDays(-1), DateTimeOffset.UtcNow.AddDays(1));

            return new X509AuthenticationProvider(certificate);
        }
    }
}
