// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Devices.Provisioning.Service;
using Microsoft.Azure.Iot.Device.Authentication;
using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Models.Telemetry;
using Microsoft.Azure.Iot.Device.Provisioning.Models;
using Microsoft.Azure.Iot.Device.Unified.Connection;
using Microsoft.Azure.Iot.Device.Unified.Telemetry;
using System.Diagnostics;
using System.Net.NetworkInformation;
using TpmAuthenticationSample;
using Xunit;

namespace Microsoft.Azure.Iot.Device.IntegrationTests.Unified
{
    /// <summary>
    /// Provisions with TPM attestation against the TSS.MSR TPM simulator, using the TPM authentication sample's
    /// Microsoft.TSS-based <see cref="TssTpmSecurityProvider"/>.
    /// </summary>
    /// <remarks>
    /// These run only on Windows, where the simulator runs, and only when TPM_SIMULATOR_PATH names the directory holding
    /// the simulator's Simulator.exe and Tpm.dll. The simulator accepts one connection at a time, so these tests run
    /// one after another.
    /// </remarks>
    [Collection(nameof(TpmSimulatorCollection))]
    [Trait("Category", "Tpm")]
    public class TpmProvisioningIntegrationTests
    {
        private readonly TpmSimulatorFixture _simulator;

        public TpmProvisioningIntegrationTests(TpmSimulatorFixture simulator)
        {
            _simulator = simulator;
        }

        [Fact(Timeout = Setup.TestTimeoutMilliseconds)]
        public async Task ProvisionsOverHttpWithTpmAttestationAndSendsTelemetry()
        {
            _simulator.SkipIfUnavailable();
            CancellationToken cancellationToken = TestContext.Current.CancellationToken;

            using TssTpmSecurityProvider tpm = TssTpmSecurityProvider.CreateForSimulator();
            IndividualEnrollment enrollment = await CreateEnrollmentAsync(tpm, cancellationToken);
            TpmAuthenticationProvider authentication = new(enrollment.RegistrationId, Setup.DpsIdScope, tpm);

            ConnectionClient connectionClient = new();
            try
            {
                TelemetryClient telemetryClient = new(connectionClient); // Not disposed: disposing it would dispose the connection client before cleanup disconnects it

                ConnectionContext context = await Setup.RetryAroundAuthorizationAsync(
                    () => connectionClient.ProvisionAndConnectAsync(CreateSettings(), authentication, cancellationToken: cancellationToken),
                    cancellationToken);

                Assert.Equal(enrollment.RegistrationId, context.DeviceId);
                await telemetryClient.SendTelemetryAsync(new DeviceToCloudTelemetry { Payload = new byte[10] }, cancellationToken);
            }
            finally
            {
                await UnifiedDeviceTestContext.CleanupAsync(connectionClient, connectionClient.GetCurrentConnectionContext(), enrollment);
            }
        }

        [Fact(Timeout = Setup.TestTimeoutMilliseconds)]
        public async Task ReconnectsWithTheKeyPersistedInTheTpmWithoutProvisioningAgain()
        {
            _simulator.SkipIfUnavailable();
            CancellationToken cancellationToken = TestContext.Current.CancellationToken;

            IndividualEnrollment? enrollment = null;
            ConnectionContext? context = null;
            ConnectionClient connectionClient = new();
            try
            {
                using (TssTpmSecurityProvider tpm = TssTpmSecurityProvider.CreateForSimulator())
                {
                    enrollment = await CreateEnrollmentAsync(tpm, cancellationToken);
                    TpmAuthenticationProvider authentication = new(enrollment.RegistrationId, Setup.DpsIdScope, tpm);
                    context = await Setup.RetryAroundAuthorizationAsync(
                        () => connectionClient.ProvisionAndConnectAsync(CreateSettings(), authentication, cancellationToken: cancellationToken),
                        cancellationToken);
                    await connectionClient.DisconnectAsync(null, cancellationToken);
                }

                // As if the device restarted: a new connection to the TPM, which power cycles it, and a new client
                // restored from the saved context. Provisioning is pointed at a host that does not exist, so the device
                // can only connect by signing with the key the TPM persisted.
                using TssTpmSecurityProvider restartedTpm = TssTpmSecurityProvider.CreateForSimulator();
                context.AuthenticationProvider = new TpmAuthenticationProvider(enrollment.RegistrationId, Setup.DpsIdScope, restartedTpm);
                ProvisioningSettings unreachableDps = CreateSettings();
                unreachableDps.GlobalEndpointAddress = "dps.invalid";

                connectionClient.Dispose();
                connectionClient = new ConnectionClient(null, context);
                ConnectionContext reconnected = await connectionClient.ProvisionAndConnectAsync(
                    unreachableDps, context.AuthenticationProvider, cancellationToken: cancellationToken);

                Assert.Same(context, reconnected);
                TelemetryClient telemetryClient = new(connectionClient); // Not disposed: disposing it would dispose the connection client before cleanup disconnects it
                await telemetryClient.SendTelemetryAsync(new DeviceToCloudTelemetry { Payload = new byte[10] }, cancellationToken);
            }
            finally
            {
                await UnifiedDeviceTestContext.CleanupAsync(connectionClient, context, enrollment);
            }
        }

        private static ProvisioningSettings CreateSettings() => new(Setup.DpsIdScope)
        {
            Transport = ProvisioningTransport.Http,
        };

        private static async Task<IndividualEnrollment> CreateEnrollmentAsync(TssTpmSecurityProvider tpm, CancellationToken cancellationToken)
        {
            string endorsementKey = Convert.ToBase64String(await tpm.GetEndorsementKeyAsync(cancellationToken));
            IndividualEnrollment enrollment = new(Guid.NewGuid().ToString(), new TpmAttestation(endorsementKey));

            using ProvisioningServiceClient provisioningServiceClient = Setup.GetDpsHubServiceClient();
            await provisioningServiceClient.CreateOrUpdateIndividualEnrollmentAsync(enrollment, cancellationToken);
            return enrollment;
        }
    }

    /// <summary>
    /// Runs the TPM tests one after another, and after every other test, because the TPM simulator accepts only one
    /// connection at a time.
    /// </summary>
    [CollectionDefinition(nameof(TpmSimulatorCollection), DisableParallelization = true)]
    public class TpmSimulatorCollection : ICollectionFixture<TpmSimulatorFixture>
    {
    }

    /// <summary>
    /// Starts the TSS.MSR TPM simulator from TPM_SIMULATOR_PATH, unless one is already listening, and stops it afterwards.
    /// </summary>
    public sealed class TpmSimulatorFixture : IAsyncLifetime
    {
        private const string SimulatorPathVariable = "TPM_SIMULATOR_PATH";

        private Process? _simulator;
        private string? _skipReason;

        public void SkipIfUnavailable()
        {
            if (_skipReason != null)
            {
                Assert.Skip(_skipReason);
            }
        }

        public async ValueTask InitializeAsync()
        {
            if (!OperatingSystem.IsWindows())
            {
                _skipReason = "The TPM simulator only runs on Windows.";
                return;
            }

            string? simulatorPath = Environment.GetEnvironmentVariable(SimulatorPathVariable);
            if (string.IsNullOrWhiteSpace(simulatorPath))
            {
                _skipReason = $"{SimulatorPathVariable} is not set to the directory holding the TPM simulator.";
                return;
            }

            if (IsSimulatorListening())
            {
                return;
            }

            string simulatorExecutable = Path.Combine(simulatorPath, "Simulator.exe");
            if (!File.Exists(simulatorExecutable))
            {
                throw new FileNotFoundException($"{SimulatorPathVariable} does not hold the TPM simulator.", simulatorExecutable);
            }

            // The simulator keeps its non-volatile state in its working directory.
            _simulator = Process.Start(new ProcessStartInfo(simulatorExecutable)
            {
                WorkingDirectory = simulatorPath,
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
            }) ?? throw new InvalidOperationException("Failed to start the TPM simulator.");

            // Drain its console output so that it never blocks on a full pipe.
            _simulator.OutputDataReceived += (_, _) => { };
            _simulator.ErrorDataReceived += (_, _) => { };
            _simulator.BeginOutputReadLine();
            _simulator.BeginErrorReadLine();

            Stopwatch waited = Stopwatch.StartNew();
            while (!IsSimulatorListening())
            {
                if (_simulator.HasExited)
                {
                    throw new InvalidOperationException($"The TPM simulator exited with code {_simulator.ExitCode}.");
                }

                if (waited.Elapsed > TimeSpan.FromSeconds(30))
                {
                    throw new TimeoutException("The TPM simulator did not start listening in time.");
                }

                await Task.Delay(TimeSpan.FromMilliseconds(250));
            }
        }

        public ValueTask DisposeAsync()
        {
            if (_simulator != null)
            {
                try
                {
                    _simulator.Kill(entireProcessTree: true);
                    _simulator.WaitForExit(TimeSpan.FromSeconds(10));
                }
                catch (InvalidOperationException)
                {
                    // It already exited.
                }

                _simulator.Dispose();
            }

            return ValueTask.CompletedTask;
        }

        private static bool IsSimulatorListening()
        {
            return IPGlobalProperties.GetIPGlobalProperties()
                .GetActiveTcpListeners()
                .Any(endpoint => endpoint.Port == TssTpmSecurityProvider.DefaultSimulatorPort);
        }
    }
}
