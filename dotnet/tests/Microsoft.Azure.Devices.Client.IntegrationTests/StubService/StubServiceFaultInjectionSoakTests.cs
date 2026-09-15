using Microsoft.Azure.Devices.Client.Models;
using Microsoft.Azure.Devices.Client.Models.DirectMethods;
using Microsoft.Azure.Devices.Client.Models.Telemetry;
using Microsoft.Azure.Devices.Client.Models.Twin;
using Microsoft.Azure.Devices.Client.Retry;
using MQTTnet.Protocol;
using System.Collections.Concurrent;
using System.Diagnostics;
using System.Text;
using System.Text.Json.Nodes;
using Xunit;
using Xunit.Sdk;
using Gen2ConnectionClient = Microsoft.Azure.Devices.Client.Gen2.Connection.ConnectionClient;
using Gen2DirectMethodClient = Microsoft.Azure.Devices.Client.Gen2.DirectMethods.DirectMethodClient;
using Gen2DirectMethodProbeAck = Microsoft.Azure.Devices.Client.Gen2.DirectMethods.DirectMethodProbeAck;
using Gen2ReportedPatchRequest = Microsoft.Azure.Devices.Client.Gen2.Twin.ReportedPatchRequest;
using Gen2TelemetryClient = Microsoft.Azure.Devices.Client.Gen2.Telemetry.TelemetryClient;
using Gen2TwinClient = Microsoft.Azure.Devices.Client.Gen2.Twin.TwinClient;
using TwinResult = Microsoft.Azure.Devices.Client.Models.Twin.Result;
using UnifiedConnectionClient = Microsoft.Azure.Devices.Client.Unified.Connection.ConnectionClient;
using UnifiedDirectMethodClient = Microsoft.Azure.Devices.Client.Unified.DirectMethods.DirectMethodClient;
using UnifiedTelemetryClient = Microsoft.Azure.Devices.Client.Unified.Telemetry.TelemetryClient;
using UnifiedTwinClient = Microsoft.Azure.Devices.Client.Unified.Twin.TwinClient;

namespace Microsoft.Azure.Devices.Client.IntegrationTests.StubService
{
    /// <summary>
    /// Long running tests that keep a real device client doing ordinary work - telemetry, direct methods and twin
    /// operations - while the stub IoT hub and the stub DPS randomly drop its connection underneath it.
    /// </summary>
    /// <remarks>
    /// <para>
    /// These are soak tests rather than protocol tests. Nothing here asserts the shape of a packet; what they assert is
    /// that the device client keeps recovering. Every operation is retried until it succeeds or a generous per-operation
    /// deadline expires, so a failure means the client stopped recovering, not that a single call happened to land in a
    /// connection gap.
    /// </para>
    /// <para>
    /// The run length is controlled by the <c>IOTHUB_STUB_SOAK_SECONDS</c> environment variable and defaults to something
    /// short enough for a normal test pass. Set it higher to actually soak.
    /// </para>
    /// </remarks>
    public class StubServiceFaultInjectionSoakTests(ITestOutputHelper output)
    {
        /// <summary>
        /// The ceiling for a soak run. This is not the run length - <see cref="ResolveSoakDuration"/> is - it is only the
        /// point at which xunit gives up on a run that is no longer making progress.
        /// </summary>
        private const int SoakTestTimeoutMilliseconds = 15 * 60 * 1000;

        private const string SoakDurationVariable = "IOTHUB_STUB_SOAK_SECONDS";

        private static readonly TimeSpan s_defaultSoakDuration = TimeSpan.FromSeconds(60);

        /// <summary>
        /// How long a single operation may keep failing before the device is declared unable to recover. This has to
        /// comfortably exceed a drop plus a reconnect plus, for gen2, a re-provisioning round trip.
        /// </summary>
        private static readonly TimeSpan s_operationRecoveryBudget = TimeSpan.FromSeconds(60);

        /// <summary>
        /// The reason codes the SDK is expected to recover from on its own, per the classification in
        /// <c>MqttConnectionManager.Classify(MqttDisconnectReason)</c>: the benign close plus everything it treats as
        /// retryable.
        /// </summary>
        /// <remarks>
        /// The codes it classifies as <c>Terminal</c> are deliberately excluded. A terminal code is a correct reason for
        /// the client to stop reconnecting, so injecting one would end the soak rather than test it.
        /// </remarks>
        private static readonly MqttDisconnectReasonCode[] s_recoverableReasonCodes =
        [
            MqttDisconnectReasonCode.NormalDisconnection,
            MqttDisconnectReasonCode.UnspecifiedError,
            MqttDisconnectReasonCode.ImplementationSpecificError,
            MqttDisconnectReasonCode.ServerBusy,
            MqttDisconnectReasonCode.ServerShuttingDown,
            MqttDisconnectReasonCode.KeepAliveTimeout,
            MqttDisconnectReasonCode.MessageRateTooHigh,
            MqttDisconnectReasonCode.QuotaExceeded,
            MqttDisconnectReasonCode.ConnectionRateExceeded,
            MqttDisconnectReasonCode.MaximumConnectTime,
        ];

        /// <summary>
        /// The reason code the SDK classifies as <c>IdentityTerminal</c>: fatal to the connection, but recoverable by a
        /// device that was provisioned through DPS, because it can register again and connect to whatever it is assigned.
        /// </summary>
        /// <remarks>
        /// <c>BadAuthenticationMethod</c> is the other code the SDK classifies that way, but MQTTnet's
        /// <see cref="MqttDisconnectReasonCode"/> does not define it, so the stub cannot send it.
        /// </remarks>
        private const MqttDisconnectReasonCode IdentityReasonCode = MqttDisconnectReasonCode.NotAuthorized;

        [Fact(Timeout = SoakTestTimeoutMilliseconds)]
        public async Task Gen2_KeepsWorkingWhileTheStubsRandomlyDropTheConnection()
        {
            TimeSpan soakDuration = ResolveSoakDuration();
            CancellationToken cancellationToken = TestContext.Current.CancellationToken;
            var report = new SoakReport(output);

            await using StubServiceTestHarness harness = await StartFaultyHarnessAsync(IotHubGeneration.Gen2, report);

            Gen2ConnectionClient connection = await harness.ConnectGen2DeviceAsync();
            using var telemetryClient = new Gen2TelemetryClient(connection);
            using var twinClient = new Gen2TwinClient(connection);
            using var directMethodClient = new Gen2DirectMethodClient(connection);

            // A gen2 device has to accept the delivery probe before the hub will send it the invocation itself.
            directMethodClient.DirectMethodProbeReceivedAsync += _ => Task.FromResult(Gen2DirectMethodProbeAck.Accepted());
            directMethodClient.DirectMethodInvokedAsync += args => Task.FromResult(new DirectMethodResponse
            {
                Status = 200,
                Payload = args.Payload,
            });

            var operations = new SoakOperations(
                SendTelemetryAsync: (telemetry, token) => telemetryClient.SendTelemetryAsync(telemetry, token),
                GetTwinAsync: token => twinClient.GetTwinAsync(cancellationToken: token),
                UpdateReportedPropertiesAsync: (patch, ifMatch, token) => twinClient.UpdateReportedPropertiesAsync(
                    new Gen2ReportedPatchRequest { ReportedProperties = patch, IfMatch = ifMatch },
                    token));

            // The device only re-provisions because it was provisioned in the first place, so the count starts at one.
            int identityFaults = await RunSoakAsync(harness, operations, soakDuration, injectIdentityFaults: true, report, cancellationToken);

            Assert.True(identityFaults > 0, "No identity fault was ever delivered, so re-provisioning was never exercised.");
            report.AssertHealthy(MinimumExpectedHubDrops(soakDuration));
        }

        [Fact(Timeout = SoakTestTimeoutMilliseconds)]
        public async Task Gen1_KeepsWorkingWhileTheStubsRandomlyDropTheConnection()
        {
            TimeSpan soakDuration = ResolveSoakDuration();
            CancellationToken cancellationToken = TestContext.Current.CancellationToken;
            var report = new SoakReport(output);

            await using StubServiceTestHarness harness = await StartFaultyHarnessAsync(IotHubGeneration.Gen1, report);

            UnifiedConnectionClient connection = await harness.ConnectGen1DeviceAsync();
            using var telemetryClient = new UnifiedTelemetryClient(connection);
            using var twinClient = new UnifiedTwinClient(connection);
            using var directMethodClient = new UnifiedDirectMethodClient(connection);

            // The classic protocol has no delivery probe, so the invocation arrives directly.
            directMethodClient.DirectMethodInvokedAsync += args => Task.FromResult(new DirectMethodResponse
            {
                Status = 200,
                Payload = args.Payload,
            });

            var operations = new SoakOperations(
                SendTelemetryAsync: (telemetry, token) => telemetryClient.SendTelemetryAsync(telemetry, token),
                GetTwinAsync: token => twinClient.GetTwinAsync(token),

                // Classic reported writes are unconditional, so the version the caller holds is not sent at all.
                UpdateReportedPropertiesAsync: (patch, _, token) => twinClient.UpdateReportedPropertiesAsync(patch, token));

            // Identity faults are a gen2 concern here: only that half of the soak asserts on re-provisioning.
            await RunSoakAsync(harness, operations, soakDuration, injectIdentityFaults: false, report, cancellationToken);

            report.AssertHealthy(MinimumExpectedHubDrops(soakDuration));
        }

        /// <summary>
        /// Start a harness whose hub and DPS are both randomly dropping the device's connection, and whose device client
        /// retries quickly enough that a soak run makes progress.
        /// </summary>
        /// <remarks>
        /// Both stubs drop connections because the device uses one MQTT client id for both endpoints, so whichever of them
        /// fires is dropping whichever connection the device currently holds. That is the point: the device has to recover
        /// from losing a hub session and from losing a registration in flight.
        /// </remarks>
        private static async Task<StubServiceTestHarness> StartFaultyHarnessAsync(IotHubGeneration generation, SoakReport report)
        {
            StubServiceTestHarness harness = await StubServiceTestHarness.StartAsync(
                generation,
                configureProvisioningService: options =>
                {
                    options.RandomConnectionDrops = RecoverableDrops(TimeSpan.FromSeconds(5), TimeSpan.FromSeconds(11));
                },
                configureHub: options =>
                {
                    options.RandomConnectionDrops = RecoverableDrops(TimeSpan.FromSeconds(3), TimeSpan.FromSeconds(7));

                    // The stub's half minute defaults are longer than this test's whole recovery budget, which would turn a
                    // single invocation that landed in a connection gap into a stall instead of a retry.
                    options.DefaultDirectMethodConnectTimeout = TimeSpan.FromSeconds(5);
                    options.DefaultDirectMethodResponseTimeout = TimeSpan.FromSeconds(5);
                },
                configureClientOptions: options =>
                {
                    // The harness default is NoRetry, which is the opposite of what a soak test needs: here the client must
                    // reconnect on its own, and quickly, because every drop is deliberate.
                    options.ConnectionRetryPolicy = new ExponentialBackoffRetryPolicy(uint.MaxValue, TimeSpan.FromSeconds(1));
                });

            harness.Stub.DeviceConnectionDropped += (_, args) => report.RecordHubDrop(args);
            harness.Dps.DeviceConnectionDropped += (_, args) => report.RecordProvisioningDrop(args);
            harness.Dps.DeviceProvisioned += (_, _) => report.RecordProvisioned();

            return harness;
        }

        private static StubConnectionDropOptions RecoverableDrops(TimeSpan minInterval, TimeSpan maxInterval) => new()
        {
            Enabled = true,
            MinInterval = minInterval,
            MaxInterval = maxInterval,
            ReasonCodes = s_recoverableReasonCodes,
            ReasonString = "stub service soak test fault injection",
        };

        /// <summary>
        /// Drive telemetry, twin and direct method round trips in a loop for the soak duration while the stubs drop the
        /// connection, optionally forcing a re-provisioning with an identity fault along the way.
        /// </summary>
        /// <returns>The number of identity faults that were actually delivered to a connected device.</returns>
        private static async Task<int> RunSoakAsync(
            StubServiceTestHarness harness,
            SoakOperations operations,
            TimeSpan soakDuration,
            bool injectIdentityFaults,
            SoakReport report,
            CancellationToken cancellationToken)
        {
            var receivedTelemetry = new ConcurrentDictionary<string, byte>();
            harness.Stub.TelemetryReceived += (_, args) =>
            {
                if (args.MessageId != null)
                {
                    receivedTelemetry[args.MessageId] = 0;
                }
            };

            using var soakCancellation = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
            Task<int> identityFaults = injectIdentityFaults
                ? InjectIdentityFaultsAsync(harness, soakDuration, report, soakCancellation.Token)
                : Task.FromResult(0);

            try
            {
                var soakTimer = Stopwatch.StartNew();

                for (int cycle = 1; soakTimer.Elapsed < soakDuration; cycle++)
                {
                    await RunOperationCycleAsync(harness, operations, receivedTelemetry, cycle, report, cancellationToken);
                    report.RecordCycle();

                    await Task.Delay(TimeSpan.FromMilliseconds(250), cancellationToken);
                }
            }
            finally
            {
                await soakCancellation.CancelAsync();
            }

            return await identityFaults;
        }

        /// <summary>
        /// Run one round of the three operations a device actually performs, each retried until it succeeds or the
        /// recovery budget expires.
        /// </summary>
        private static async Task RunOperationCycleAsync(
            StubServiceTestHarness harness,
            SoakOperations operations,
            ConcurrentDictionary<string, byte> receivedTelemetry,
            int cycle,
            SoakReport report,
            CancellationToken cancellationToken)
        {
            string messageId = $"soak-{cycle}";

            // Telemetry is only sent once the hub has actually recorded it, because a publish that raced a drop can be
            // accepted locally and still never reach the stub.
            await RunWithRecoveryAsync(
                "telemetry",
                async token =>
                {
                    await operations.SendTelemetryAsync(
                        new DeviceToCloudTelemetry
                        {
                            Payload = Encoding.UTF8.GetBytes($"{{\"cycle\":{cycle}}}"),
                            MessageId = messageId,
                            ContentType = "application/json",
                        },
                        token);

                    return await WaitUntilAsync(() => receivedTelemetry.ContainsKey(messageId), TimeSpan.FromSeconds(5), token);
                },
                delivered => delivered,
                report,
                cancellationToken);

            DeviceTwin twin = await RunWithRecoveryAsync(
                "twin get",
                operations.GetTwinAsync,
                _ => true,
                report,
                cancellationToken);

            // The gen2 reported write is conditional on the version this device believes is authoritative, so a stale read
            // is retried with a fresh one rather than being reported as a soak failure.
            ulong reportedVersion = twin.ReportedVersion ?? 0;
            await RunWithRecoveryAsync(
                "reported properties",
                async token =>
                {
                    ReportedPatchResponse response = await operations.UpdateReportedPropertiesAsync(
                        new JsonObject { ["soakCycle"] = cycle },
                        reportedVersion,
                        token);

                    if (response.Result == TwinResult.VersionMismatch)
                    {
                        reportedVersion = response.Version;
                    }

                    return response;
                },
                response => response.Result == TwinResult.Ok,
                report,
                cancellationToken);

            // Service-initiated, so this is the direction that depends on the device having re-subscribed - and, on gen2,
            // on it having birthed again - after the last drop.
            StubDirectMethodResult methodResult = await RunWithRecoveryAsync(
                "direct method",
                token => harness.Stub.InvokeDirectMethodAsync(
                    harness.DeviceId,
                    "soakEcho",
                    Encoding.UTF8.GetBytes(messageId),
                    cancellationToken: token),
                result => result.Outcome == StubDirectMethodOutcome.Completed && result.Status == 200,
                report,
                cancellationToken);

            Assert.Equal(messageId, Encoding.UTF8.GetString(methodResult.Payload));
        }

        /// <summary>
        /// Periodically drop the device's hub connection with an identity terminal reason code, and confirm that the device
        /// recovers by registering with the stub DPS again rather than by simply reconnecting.
        /// </summary>
        /// <remarks>
        /// This is the one fault in the soak that a reconnect cannot fix. The SDK classifies these codes as
        /// <c>ErrorRetryability.IdentityTerminal</c>, and a device that reached its hub through DPS answers them by
        /// provisioning again with the credentials it still holds.
        /// </remarks>
        private static async Task<int> InjectIdentityFaultsAsync(
            StubServiceTestHarness harness,
            TimeSpan soakDuration,
            SoakReport report,
            CancellationToken cancellationToken)
        {
            // Long enough apart that each fault's recovery finishes before the next one, because the SDK collapses a second
            // identity fault into a re-provisioning attempt that is already running.
            TimeSpan interval = Clamp(soakDuration / 4, TimeSpan.FromSeconds(10), TimeSpan.FromSeconds(30));
            int delivered = 0;

            try
            {
                while (!cancellationToken.IsCancellationRequested)
                {
                    await Task.Delay(interval, cancellationToken);

                    int provisionedBefore = report.ProvisionedCount;

                    if (!await harness.Stub.DropDeviceConnectionAsync(
                        harness.DeviceId,
                        IdentityReasonCode,
                        "soak test identity fault",
                        cancellationToken))
                    {
                        // The device was between connections, so there was nothing to fault. The next pass will catch it.
                        report.Note($"identity fault '{IdentityReasonCode}' found no connection to drop");
                        continue;
                    }

                    delivered++;

                    bool reprovisioned = await WaitUntilAsync(
                        () => report.ProvisionedCount > provisionedBefore,
                        s_operationRecoveryBudget,
                        cancellationToken);

                    Assert.True(
                        reprovisioned,
                        $"The device did not re-provision within {s_operationRecoveryBudget} of being disconnected with '{IdentityReasonCode}'.");

                    report.RecordIdentityFault(IdentityReasonCode);
                }
            }
            catch (OperationCanceledException)
            {
                // The soak finished, which is the only thing that stops this loop.
            }

            return delivered;
        }

        private static async Task<T> RunWithRecoveryAsync<T>(
            string operationName,
            Func<CancellationToken, Task<T>> operation,
            Func<T, bool> succeeded,
            SoakReport report,
            CancellationToken cancellationToken)
        {
            var budget = Stopwatch.StartNew();
            var attempts = new List<string>();

            while (true)
            {
                cancellationToken.ThrowIfCancellationRequested();

                try
                {
                    T result = await operation(cancellationToken);

                    if (succeeded(result))
                    {
                        report.RecordOperation(operationName, attempts.Count);
                        return result;
                    }

                    attempts.Add(result is StubDirectMethodResult method ? method.Outcome.ToString() : result?.ToString() ?? "null");
                }
                catch (Exception e) when (e is not OperationCanceledException)
                {
                    attempts.Add(e.GetType().Name);
                }

                if (budget.Elapsed >= s_operationRecoveryBudget)
                {
                    throw new XunitException(
                        $"The device never completed a '{operationName}' operation within {s_operationRecoveryBudget}, so it "
                        + $"stopped recovering from the injected faults. Attempts: {string.Join(", ", attempts)}. {report}");
                }

                await Task.Delay(TimeSpan.FromMilliseconds(250), cancellationToken);
            }
        }

        /// <summary>
        /// Poll a condition that is satisfied by another task rather than signalled, which is what the stub's events and the
        /// SDK's own recovery both are from this test's point of view.
        /// </summary>
        private static async Task<bool> WaitUntilAsync(Func<bool> condition, TimeSpan timeout, CancellationToken cancellationToken)
        {
            var elapsed = Stopwatch.StartNew();

            while (!condition())
            {
                if (elapsed.Elapsed >= timeout)
                {
                    return false;
                }

                await Task.Delay(TimeSpan.FromMilliseconds(50), cancellationToken);
            }

            return true;
        }

        /// <summary>
        /// The number of hub drops a run of this length has to have injected for its success to mean anything.
        /// </summary>
        /// <remarks>
        /// The hub drops on a 3 to 7 second interval, so a run injects roughly one drop every five seconds. This scales
        /// with the configured duration rather than being a fixed count, because a deliberately short run - the default,
        /// or one shortened through <see cref="SoakDurationVariable"/> - would otherwise fail for being short rather than
        /// for the device having stopped recovering.
        /// </remarks>
        private static int MinimumExpectedHubDrops(TimeSpan soakDuration) =>
            Math.Max(1, (int)(soakDuration.TotalSeconds / 15));

        private static TimeSpan ResolveSoakDuration()        {
            string? configured = Environment.GetEnvironmentVariable(SoakDurationVariable);

            return int.TryParse(configured, out int seconds) && seconds > 0
                ? TimeSpan.FromSeconds(seconds)
                : s_defaultSoakDuration;
        }

        private static TimeSpan Clamp(TimeSpan value, TimeSpan min, TimeSpan max) =>
            value < min ? min : value > max ? max : value;

        /// <summary>
        /// The three device-initiated operations a soak run drives, bound to whichever generation's clients the caller
        /// built. Only the twin write differs in shape between the two, because gen2 reported writes are conditional.
        /// </summary>
        private sealed record SoakOperations(
            Func<DeviceToCloudTelemetry, CancellationToken, Task> SendTelemetryAsync,
            Func<CancellationToken, Task<DeviceTwin>> GetTwinAsync,
            Func<JsonObject, ulong, CancellationToken, Task<ReportedPatchResponse>> UpdateReportedPropertiesAsync);

        /// <summary>
        /// Everything a soak run observed. This is both the test's assertions and, when one fails, its diagnosis: a soak
        /// failure is impossible to interpret without knowing how many faults were injected and how far the run got.
        /// </summary>
        private sealed class SoakReport(ITestOutputHelper output)
        {
            private readonly List<string> _notes = new();
            private readonly Dictionary<string, int> _retriesByOperation = new();
            private int _cycles;
            private int _hubDrops;
            private int _provisioningDrops;
            private int _provisioned;
            private int _identityFaults;

            public int ProvisionedCount => Volatile.Read(ref _provisioned);

            public void RecordCycle() => Interlocked.Increment(ref _cycles);

            public void RecordProvisioned() => Interlocked.Increment(ref _provisioned);

            public void RecordHubDrop(StubDeviceConnectionDroppedEventArgs args)
            {
                Interlocked.Increment(ref _hubDrops);
                Note($"hub dropped the connection with '{args.ReasonCode}'");
            }

            public void RecordProvisioningDrop(StubDeviceConnectionDroppedEventArgs args)
            {
                Interlocked.Increment(ref _provisioningDrops);
                Note($"DPS dropped the connection with '{args.ReasonCode}'");
            }

            public void RecordIdentityFault(MqttDisconnectReasonCode reasonCode)
            {
                Interlocked.Increment(ref _identityFaults);
                Note($"the device re-provisioned after an identity fault of '{reasonCode}'");
            }

            public void RecordOperation(string operationName, int retries)
            {
                if (retries == 0)
                {
                    return;
                }

                lock (_retriesByOperation)
                {
                    _retriesByOperation[operationName] = _retriesByOperation.GetValueOrDefault(operationName) + retries;
                }
            }

            public void Note(string note)
            {
                lock (_notes)
                {
                    _notes.Add($"[{DateTimeOffset.UtcNow:HH:mm:ss.fff}] {note}");
                }
            }

            /// <summary>
            /// Assert that the run was a real soak rather than a quiet one that happened to pass.
            /// </summary>
            /// <param name="minimumHubDrops">
            /// How many hub drops the run has to have injected for its success to mean anything.
            /// </param>
            public void AssertHealthy(int minimumHubDrops)
            {
                output.WriteLine(ToString());

                lock (_notes)
                {
                    foreach (string note in _notes)
                    {
                        output.WriteLine(note);
                    }
                }

                Assert.True(_cycles > 0, "The soak never completed an operation cycle.");
                Assert.True(
                    _hubDrops >= minimumHubDrops,
                    $"Only {_hubDrops} connection drops were injected, which is too few for this run to have proven anything.");
            }

            public override string ToString()
            {
                lock (_retriesByOperation)
                {
                    string retries = _retriesByOperation.Count == 0
                        ? "none"
                        : string.Join(", ", _retriesByOperation.Select(entry => $"{entry.Key}={entry.Value}"));

                    return $"Soak summary: {_cycles} cycles, {_hubDrops} hub drops, {_provisioningDrops} DPS drops, "
                        + $"{_identityFaults} identity faults, {ProvisionedCount} registrations, retries: {retries}.";
                }
            }
        }
    }
}
