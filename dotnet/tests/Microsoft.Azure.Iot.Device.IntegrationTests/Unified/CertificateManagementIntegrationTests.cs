// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Authentication;
using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Models.CertificateManagement;
using Microsoft.Azure.Iot.Device.Models.Telemetry;
using Microsoft.Azure.Iot.Device.Provisioning;
using Microsoft.Azure.Iot.Device.Unified.Connection;
using Microsoft.Azure.Iot.Device.Unified.FileUpload;
using Microsoft.Azure.Iot.Device.Unified.Telemetry;
using System.Diagnostics;
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
        [Fact(Timeout = 4 * Setup.TestTimeoutMilliseconds + 10000)]
        [Trait("Bug", "40197368")]
        public async Task CanForceRenewX509CertificateThroughDpsAfterDisposingClient()
        {
            string registrationId = Environment.GetEnvironmentVariable("IOT_DPS_GROUP_X509_REGISTRATION_ID")
                ?? throw new InvalidOperationException("Missing IOT_DPS_GROUP_X509_REGISTRATION_ID environment variable.");
            string certificatePem = DecodeBase64EnvironmentVariable("IOT_DPS_GROUP_X509_CERTIFICATE");
            string privateKeyPem = DecodeBase64EnvironmentVariable("IOT_DPS_GROUP_X509_KEY");
            using X509Certificate2 bootCertificate = CreateX509CertificateFromKeyAndCert(certificatePem, privateKeyPem);

            ITestOutputHelper output = TestContext.Current.TestOutputHelper
                ?? throw new InvalidOperationException("Test output is unavailable.");
            using StringWriter traceOutput = new();
            using TextWriterTraceListener listener = new(TextWriter.Synchronized(traceOutput))
            {
                TraceOutputOptions = TraceOptions.DateTime,
            };
            Trace.Listeners.Add(listener);

            ConnectionContext? previousContext = null;
            string? previousThumbprint = null;
            try
            {
                // Repeat the disposal/renewal sequence because the reported DPS-to-Hub race is timing-sensitive.
                const int renewalCount = 3;
                for (int cycle = 0; cycle <= renewalCount; cycle++)
                {
                    using CancellationTokenSource cts = CancellationTokenSource.CreateLinkedTokenSource(
                        TestContext.Current.CancellationToken);
                    cts.CancelAfter(Setup.TestTimeoutMilliseconds);
                    using RSA csrKey = RSA.Create(2048);
                    X509AuthenticationProvider authentication = new(bootCertificate, csrKey);
                    ProvisioningSettings settings = new(DpsIdScope)
                    {
                        CertificateSigningRequest = new(
                            csrKey, CertificateUtilities.GenerateCsrWithPrivateKey(registrationId, csrKey)),
                    };
                    using ConnectionClient client = new(new ConnectionClientOptions
                    {
                        EnableMqttLogging = true,
                        ConnectionAttemptTimeout = TimeSpan.FromSeconds(5),
                    });
                    Stopwatch elapsed = Stopwatch.StartNew();
                    output.WriteLine($"Cycle {cycle}/{renewalCount}: provisioning with a fresh client and CSR; forceProvisioning={cycle > 0}.");

                    try
                    {
                        // Do not retry the whole operation: that could hide a failed post-renewal Hub handoff.
                        ConnectionContext context = await client.ProvisionAndConnectAsync(
                            settings, authentication, forceProvisioning: cycle > 0, cancellationToken: cts.Token)
                            .WaitAsync(cts.Token);

                        Assert.NotNull(context.IssuedClientCertificates);
                        Assert.NotEmpty(context.IssuedClientCertificates);
                        Assert.Same(context, client.GetCurrentConnectionContext());
                        using X509Certificate2 issuedCertificate = X509CertificateLoader.LoadCertificate(
                            Convert.FromBase64String(context.IssuedClientCertificates[0]));
                        if (previousContext != null)
                        {
                            Assert.NotSame(previousContext, context);
                            Assert.Equal(previousContext.DeviceId, context.DeviceId);
                            Assert.NotEqual(previousThumbprint, issuedCertificate.Thumbprint);
                        }

                        AuthenticationCredentials credentials = await authentication.GetOperationalAuthenticationCredentialsAsync(
                            context.IotHubHostName, context.DeviceId, context.IssuedClientCertificates);
                        Assert.NotNull(credentials.ClientCertificate);
                        Assert.True(credentials.ClientCertificate.HasPrivateKey);
                        Assert.Equal(issuedCertificate.Thumbprint, credentials.ClientCertificate.Thumbprint);

                        using TelemetryClient telemetry = new(client);
                        await telemetry.SendTelemetryAsync(new DeviceToCloudTelemetry
                        {
                            Payload = Encoding.UTF8.GetBytes($"Certificate renewal repro cycle {cycle}"),
                            MessageId = Guid.NewGuid().ToString(),
                        }, cts.Token).WaitAsync(cts.Token);

                        output.WriteLine($"Cycle {cycle}: Hub connection and telemetry succeeded after {elapsed.Elapsed}.");
                        previousContext = context;
                        previousThumbprint = issuedCertificate.Thumbprint;
                    }
                    finally
                    {
                        ConnectionContext? context = client.GetCurrentConnectionContext();
                        output.WriteLine(
                            $"Cycle {cycle}: elapsed={elapsed.Elapsed}; assigned Hub={context?.IotHubHostName}; " +
                            $"issued certificate count={context?.IssuedClientCertificates?.Count ?? 0}. " +
                            "Disposing the client without an explicit DisconnectAsync.");
                    }
                }
            }
            finally
            {
                Trace.Flush();
                Trace.Listeners.Remove(listener);
                output.WriteLine(traceOutput.ToString());
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