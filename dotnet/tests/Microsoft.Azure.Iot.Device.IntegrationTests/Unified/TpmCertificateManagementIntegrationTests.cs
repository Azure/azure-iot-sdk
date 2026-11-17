// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Devices.Provisioning.Service;
using Microsoft.Azure.Iot.Device.Authentication;
using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Models.Telemetry;
using Microsoft.Azure.Iot.Device.Unified.Connection;
using Microsoft.Azure.Iot.Device.Unified.Telemetry;
using System.Globalization;
using System.Net;
using System.Net.Http.Headers;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using System.Text.Json.Nodes;
using TpmAuthenticationSample;
using Xunit;

namespace Microsoft.Azure.Iot.Device.IntegrationTests.Unified
{
    /// <summary>
    /// Provisions with TPM attestation against the TSS.MSR TPM simulator while requesting a client certificate, then
    /// authenticates with IoT Hub using the issued certificate instead of the TPM.
    /// </summary>
    /// <remarks>
    /// Like <see cref="TpmProvisioningIntegrationTests"/>, these run only on Windows with TPM_SIMULATOR_PATH set. They
    /// also need an IoT Hub and DPS set up for certificate management, and the ADR_CERT_MGMT_* names of the certificate
    /// policy to link the TPM enrollment to, so they run with the other certificate management tests.
    /// </remarks>
    [Collection(nameof(TpmSimulatorCollection))]
    [Trait("Category", "CertificateManagement")]
    public class TpmCertificateManagementIntegrationTests
    {
        // The DPS service API version that can link an enrollment to an ADR certificate policy.
        private const string EnrollmentApiVersion = "2026-11-01";

        private readonly TpmSimulatorFixture _simulator;

        public TpmCertificateManagementIntegrationTests(TpmSimulatorFixture simulator)
        {
            _simulator = simulator;
        }

        [Fact(Timeout = Setup.TestTimeoutMilliseconds)]
        public async Task ProvisionsWithTpmAttestationThenConnectsWithTheIssuedCertificate()
        {
            _simulator.SkipIfUnavailable();
            CancellationToken cancellationToken = TestContext.Current.CancellationToken;

            using TssTpmProvider tpm = TssTpmProvider.CreateForSimulator();
            using RSA issuedCertificatePrivateKey = RSA.Create(2048);
            IndividualEnrollment enrollment = await CreateEnrollmentLinkedToCertificatePolicyAsync(tpm, cancellationToken);
            TpmAuthenticationProvider authentication = new(enrollment.RegistrationId, Setup.DpsIdScope, tpm, issuedCertificatePrivateKey);

            ProvisioningSettings settings = new(Setup.DpsIdScope)
            {
                CertificateSigningRequest = new(
                    issuedCertificatePrivateKey,
                    CertificateUtilities.GenerateCsrWithPrivateKey(enrollment.RegistrationId, issuedCertificatePrivateKey)),
            };

            ConnectionClient connectionClient = new();
            try
            {
                TelemetryClient telemetryClient = new(connectionClient); // Not disposed: disposing it would dispose the connection client before cleanup disconnects it

                ConnectionContext context = await Setup.RetryAroundAuthorizationAsync(
                    () => connectionClient.ProvisionAndConnectAsync(settings, authentication, cancellationToken: cancellationToken),
                    cancellationToken);

                Assert.Equal(enrollment.RegistrationId, context.DeviceId);
                Assert.NotNull(context.IssuedClientCertificates);
                Assert.NotEmpty(context.IssuedClientCertificates);

                // IoT Hub is authenticated with the issued certificate rather than a TPM-signed SAS token.
                AuthenticationCredentials hubCredentials = await authentication.GetPrimaryOperationalAuthenticationCredentialsAsync(
                    context.IotHubHostName,
                    context.DeviceId,
                    context.IssuedClientCertificates,
                    cancellationToken);
                using X509Certificate2 issuedCertificate = X509CertificateLoader.LoadCertificate(Convert.FromBase64String(context.IssuedClientCertificates[0]));
                Assert.NotNull(hubCredentials.ClientCertificate);
                Assert.Equal(issuedCertificate.Thumbprint, hubCredentials.ClientCertificate.Thumbprint);
                Assert.True(hubCredentials.ClientCertificate.HasPrivateKey);
                Assert.Empty(hubCredentials.Password);

                await telemetryClient.SendTelemetryAsync(new DeviceToCloudTelemetry { Payload = new byte[10] }, cancellationToken);
            }
            finally
            {
                await UnifiedDeviceTestContext.CleanupAsync(connectionClient, connectionClient.GetCurrentConnectionContext(), enrollment);
            }
        }

        /// <summary>
        /// Creates a TPM individual enrollment linked to the certificate policy, through the DPS service REST API because
        /// the DPS service client cannot express the certificate policy reference.
        /// </summary>
        private static async Task<IndividualEnrollment> CreateEnrollmentLinkedToCertificatePolicyAsync(TssTpmProvider tpm, CancellationToken cancellationToken)
        {
            string namespaceName = GetRequiredCertificatePolicyName("ADR_CERT_MGMT_NAMESPACE_NAME");
            string certificateAuthorityName = GetRequiredCertificatePolicyName("ADR_CERT_MGMT_CERTIFICATE_AUTHORITY_NAME");
            string certificatePolicyName = GetRequiredCertificatePolicyName("ADR_CERT_MGMT_POLICY_NAME");

            string registrationId = Guid.NewGuid().ToString();
            string endorsementKey = Convert.ToBase64String(await tpm.GetEndorsementKeyAsync(cancellationToken));
            JsonObject body = new()
            {
                ["registrationId"] = registrationId,
                ["attestation"] = new JsonObject
                {
                    ["type"] = "tpm",
                    ["tpm"] = new JsonObject { ["endorsementKey"] = endorsementKey },
                },
                ["provisioningStatus"] = "enabled",
                ["namespaceName"] = namespaceName,
                ["certificateAuthorityName"] = certificateAuthorityName,
                ["certificatePolicyName"] = certificatePolicyName,
            };

            Dictionary<string, string> connectionString = Setup.DpsConnectionString
                .Split(';', StringSplitOptions.RemoveEmptyEntries)
                .Select(part => part.Split('=', 2))
                .ToDictionary(part => part[0], part => part[1], StringComparer.OrdinalIgnoreCase);
            string hostName = connectionString["HostName"];

            using HttpClient httpClient = new();
            using HttpRequestMessage request = new(HttpMethod.Put, $"https://{hostName}/enrollments/{Uri.EscapeDataString(registrationId)}?api-version={EnrollmentApiVersion}")
            {
                Content = new StringContent(body.ToJsonString(), Encoding.UTF8, "application/json"),
            };
            request.Headers.TryAddWithoutValidation(
                "Authorization",
                BuildServiceSasToken(hostName, connectionString["SharedAccessKeyName"], connectionString["SharedAccessKey"]));
            request.Headers.Accept.Add(new MediaTypeWithQualityHeaderValue("application/json"));

            using HttpResponseMessage response = await httpClient.SendAsync(request, cancellationToken);
            if (!response.IsSuccessStatusCode)
            {
                string error = await response.Content.ReadAsStringAsync(cancellationToken);
                throw new InvalidOperationException($"Creating the TPM enrollment failed with {(int)response.StatusCode}: {error}");
            }

            return new IndividualEnrollment(registrationId, new TpmAttestation(endorsementKey));
        }

        private static string GetRequiredCertificatePolicyName(string variable)
        {
            string? value = Environment.GetEnvironmentVariable(variable);
            if (string.IsNullOrWhiteSpace(value))
            {
                Assert.Skip($"{variable} is not set; a TPM enrollment must be linked to a certificate policy to be issued a certificate.");
            }

            return value;
        }

        private static string BuildServiceSasToken(string hostName, string keyName, string key)
        {
            string expiresOn = DateTimeOffset.UtcNow.AddHours(1).ToUnixTimeSeconds().ToString(CultureInfo.InvariantCulture);
            string audience = WebUtility.UrlEncode(hostName);
            using HMACSHA256 hmac = new(Convert.FromBase64String(key));
            string signature = Convert.ToBase64String(hmac.ComputeHash(Encoding.UTF8.GetBytes($"{audience}\n{expiresOn}")));
            return $"SharedAccessSignature sr={audience}&sig={WebUtility.UrlEncode(signature)}&se={expiresOn}&skn={WebUtility.UrlEncode(keyName)}";
        }
    }
}
