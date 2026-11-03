// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.MQTTnetAdapter;
using System.Diagnostics;
using System.Runtime.InteropServices;

namespace Microsoft.Azure.Iot.Device.IntegrationTests.Longhaul
{
    /// <summary>
    /// Runs a set of device operations, each on its own fixed cadence, against one connected device for the configured
    /// duration, while recording per-operation outcomes and latencies, connection drops and recoveries, and process
    /// resource usage. The result is analyzed into a <see cref="LonghaulReport"/>.
    /// </summary>
    internal sealed class LonghaulRun
    {
        private const double BytesPerMegabyte = 1024 * 1024;

        private readonly LonghaulSettings _settings;
        private readonly string _scenario;
        private readonly string _apiSet;
        private readonly string _hub;
        private readonly Action<string> _log;
        private readonly List<ScheduledOperation> _operations = new();
        private readonly List<ResourceSample> _samples = new();
        private readonly CancellationTokenSource _faulted = new();

        private readonly object _connectionLock = new();
        private bool _recordingConnection;
        private long _runStartTimestamp;
        private int _connects;
        private int _disconnects;
        private readonly Dictionary<string, int> _disconnectReasons = new();
        private DateTimeOffset? _disconnectedSince;
        private TimeSpan _totalDisconnected;
        private TimeSpan _longestOutage;
        private int _reprovisioningStarts;
        private readonly List<string> _faultMessages = new();

        public LonghaulRun(LonghaulSettings settings, string scenario, string apiSet, string hub, Action<string> log)
        {
            _settings = settings;
            _scenario = scenario;
            _apiSet = apiSet;
            _hub = hub;
            _log = log;
        }

        /// <summary>
        /// Adds an operation that runs every <paramref name="interval"/>. <paramref name="executeAsync"/> receives a
        /// sequence number unique to this operation and must throw if the operation did not succeed end to end.
        /// </summary>
        public void AddOperation(string name, TimeSpan interval, Func<long, CancellationToken, Task> executeAsync)
            => _operations.Add(new ScheduledOperation(name, interval, executeAsync));

        /// <summary>
        /// Records connection drops, recoveries and faults of this device's connection while the run is in progress.
        /// </summary>
        public void Observe(AbstractConnectionClient connectionClient, MqttNetClient mqttClient)
        {
            mqttClient.ConnectedAsync += _ =>
            {
                lock (_connectionLock)
                {
                    if (_recordingConnection)
                    {
                        _connects++;
                        EndOutageLocked();
                    }
                }

                return Task.CompletedTask;
            };

            mqttClient.DisconnectedAsync += args =>
            {
                lock (_connectionLock)
                {
                    // The MQTT client may report the end of one connection more than once, and also reports failed
                    // connect attempts, so only the transition from connected to disconnected counts as a drop. A
                    // delayed callback can also arrive after the client has already reconnected; that is not a drop.
                    if (_recordingConnection && _disconnectedSince == null && !mqttClient.IsConnected())
                    {
                        _disconnects++;
                        _disconnectedSince = DateTimeOffset.UtcNow;
                        string reason = args.Exception == null
                            ? args.Reason.ToString()
                            : $"{args.Reason} ({args.Exception.GetType().Name})";
                        _disconnectReasons[reason] = _disconnectReasons.GetValueOrDefault(reason) + 1;
                    }
                }

                return Task.CompletedTask;
            };

            connectionClient.ReprovisioningStartedAsync += args =>
            {
                lock (_connectionLock)
                {
                    _reprovisioningStarts++;
                }

                _log($"[{_scenario}] Re-provisioning started: {args.Exception.Message}");
                return Task.CompletedTask;
            };

            connectionClient.ConnectionFaultedAsync += args =>
            {
                lock (_connectionLock)
                {
                    _faultMessages.Add($"{args.Exception.GetType().Name}: {args.Exception.Message}");
                }

                // The client has given up on the connection, so every remaining operation would fail. End the run now
                // rather than spend the rest of the duration recording the same failure.
                _log($"[{_scenario}] Connection faulted unrecoverably; ending the run early. {args.Exception}");
                _faulted.Cancel();
                return Task.CompletedTask;
            };
        }

        public async Task<LonghaulReport> RunAsync(CancellationToken cancellationToken)
        {
            DateTimeOffset startedUtc = DateTimeOffset.UtcNow;
            _runStartTimestamp = Stopwatch.GetTimestamp();
            lock (_connectionLock)
            {
                _recordingConnection = true;
            }

            _log($"[{_scenario}] Starting a {_settings.Duration.TotalMinutes:0.#} minute longhaul run of {string.Join(", ", _operations.Select(o => o.Name))}.");

            using CancellationTokenSource run = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken, _faulted.Token);
            run.CancelAfter(_settings.Duration);

            List<Task> loops = _operations.Select((operation, index) => RunOperationAsync(operation, TimeSpan.FromSeconds(index), run.Token)).ToList();
            loops.Add(SampleResourcesAsync(run.Token));
            loops.Add(ReportProgressAsync(run.Token));
            await Task.WhenAll(loops);

            TimeSpan elapsed = Stopwatch.GetElapsedTime(_runStartTimestamp);
            lock (_connectionLock)
            {
                _recordingConnection = false;
                EndOutageLocked();
            }

            // A final sample so that the end of the run is always represented, even if it ended between samples.
            TakeSample();

            cancellationToken.ThrowIfCancellationRequested();

            LonghaulReport report = BuildReport(startedUtc, elapsed);
            _log($"[{_scenario}] Finished after {elapsed.TotalMinutes:0.#} minutes: {(report.Passed ? "passed" : "FAILED")}.");
            return report;
        }

        private async Task RunOperationAsync(ScheduledOperation operation, TimeSpan initialOffset, CancellationToken runToken)
        {
            // Staggered so that operations sharing a cadence do not all start in the same instant.
            long nextDue = _runStartTimestamp + (long)(initialOffset.TotalSeconds * Stopwatch.Frequency);
            long intervalTicks = (long)(operation.Interval.TotalSeconds * Stopwatch.Frequency);
            long sequence = 0;

            while (!runToken.IsCancellationRequested)
            {
                TimeSpan wait = TimeSpan.FromSeconds((double)(nextDue - Stopwatch.GetTimestamp()) / Stopwatch.Frequency);
                if (wait > TimeSpan.Zero)
                {
                    try
                    {
                        await Task.Delay(wait, runToken);
                    }
                    catch (OperationCanceledException)
                    {
                        return;
                    }
                }

                sequence++;
                long started = Stopwatch.GetTimestamp();
                using CancellationTokenSource attempt = CancellationTokenSource.CreateLinkedTokenSource(runToken);
                attempt.CancelAfter(_settings.OperationTimeout);
                try
                {
                    await operation.ExecuteAsync(sequence, attempt.Token);
                    operation.Stats.RecordSuccess(Stopwatch.GetElapsedTime(started));
                }
                catch (OperationCanceledException) when (runToken.IsCancellationRequested)
                {
                    // The run ended while this operation was in flight; it neither succeeded nor failed.
                    return;
                }
                catch (Exception e)
                {
                    string error = attempt.IsCancellationRequested
                        ? $"Timeout ({_settings.OperationTimeout.TotalSeconds:0}s)"
                        : e.GetType().Name;
                    operation.Stats.RecordFailure(error);
                    _log($"[{_scenario}] {operation.Name} #{sequence} failed after {Stopwatch.GetElapsedTime(started).TotalSeconds:0.0}s: {error}: {e.Message}");
                }

                // If an operation overran its slot, start the next one immediately once, then return to the cadence
                // from now instead of trying to catch up on every missed slot.
                nextDue += intervalTicks;
                long now = Stopwatch.GetTimestamp();
                if (nextDue < now)
                {
                    nextDue = now;
                }
            }
        }

        private async Task SampleResourcesAsync(CancellationToken runToken)
        {
            using PeriodicTimer timer = new(_settings.SampleInterval);
            TakeSample();
            try
            {
                while (await timer.WaitForNextTickAsync(runToken))
                {
                    TakeSample();
                }
            }
            catch (OperationCanceledException)
            {
            }
        }

        private async Task ReportProgressAsync(CancellationToken runToken)
        {
            using PeriodicTimer timer = new(_settings.ProgressInterval);
            try
            {
                while (await timer.WaitForNextTickAsync(runToken))
                {
                    TimeSpan elapsed = Stopwatch.GetElapsedTime(_runStartTimestamp);
                    string operations = string.Join(", ", _operations.Select(o =>
                    {
                        (int attempts, int successes) = o.Stats.Counts;
                        return $"{o.Name} {successes}/{attempts}";
                    }));
                    int disconnects;
                    lock (_connectionLock)
                    {
                        disconnects = _disconnects;
                    }

                    ResourceSample? latest;
                    lock (_samples)
                    {
                        latest = _samples.LastOrDefault();
                    }

                    _log($"[{_scenario}] {elapsed.TotalMinutes:0}/{_settings.Duration.TotalMinutes:0} min: {operations}; " +
                        $"disconnects {disconnects}; managed heap {latest?.ManagedMegabytes:0.0} MB");
                }
            }
            catch (OperationCanceledException)
            {
            }
        }

        private void TakeSample()
        {
            // Forcing a full collection makes the managed heap size reflect live objects only, which is what leak
            // detection needs; at one sample a minute the cost is negligible.
            double managed = GC.GetTotalMemory(forceFullCollection: true) / BytesPerMegabyte;
            using Process process = Process.GetCurrentProcess();
            bool connected;
            lock (_connectionLock)
            {
                connected = _disconnectedSince == null;
            }

            ResourceSample sample = new(
                Math.Round(Stopwatch.GetElapsedTime(_runStartTimestamp).TotalMinutes, 2),
                Math.Round(managed, 2),
                Math.Round(process.WorkingSet64 / BytesPerMegabyte, 2),
                process.HandleCount,
                process.Threads.Count,
                connected);

            lock (_samples)
            {
                _samples.Add(sample);
            }
        }

        private void EndOutageLocked()
        {
            if (_disconnectedSince is DateTimeOffset since)
            {
                TimeSpan outage = DateTimeOffset.UtcNow - since;
                _totalDisconnected += outage;
                if (outage > _longestOutage)
                {
                    _longestOutage = outage;
                }

                _disconnectedSince = null;
            }
        }

        private LonghaulReport BuildReport(DateTimeOffset startedUtc, TimeSpan elapsed)
        {
            List<OperationReport> operations = _operations.Select(o => o.Stats.ToReport(o.Name)).ToList();

            ConnectionReport connection;
            lock (_connectionLock)
            {
                connection = new ConnectionReport(
                    _connects,
                    _disconnects,
                    new Dictionary<string, int>(_disconnectReasons),
                    _reprovisioningStarts,
                    _faultMessages.Count,
                    _faultMessages.ToList(),
                    Math.Round(_totalDisconnected.TotalSeconds, 1),
                    Math.Round(_longestOutage.TotalSeconds, 1));
            }

            List<ResourceSample> samples;
            lock (_samples)
            {
                samples = _samples.ToList();
            }

            ResourceReport resources = LonghaulAnalysis.AnalyzeResources(samples, _settings.Duration);
            ThresholdReport thresholds = new(_settings.MinSuccessRatePercent, _settings.MaxManagedMemoryGrowthMegabytes);

            return new LonghaulReport(
                _scenario,
                _apiSet,
                _hub,
                RuntimeInformation.OSDescription,
                RuntimeInformation.FrameworkDescription,
                startedUtc,
                Math.Round(_settings.Duration.TotalMinutes, 2),
                Math.Round(elapsed.TotalMinutes, 2),
                operations,
                connection,
                resources,
                thresholds,
                LonghaulAnalysis.FindViolations(operations, connection, resources, thresholds));
        }

        private sealed class ScheduledOperation(string name, TimeSpan interval, Func<long, CancellationToken, Task> executeAsync)
        {
            public string Name { get; } = name;

            public TimeSpan Interval { get; } = interval;

            public Func<long, CancellationToken, Task> ExecuteAsync { get; } = executeAsync;

            public OperationStats Stats { get; } = new();
        }
    }

    internal sealed class OperationStats
    {
        private readonly object _lock = new();
        private readonly List<double> _successLatenciesMilliseconds = new();
        private readonly Dictionary<string, int> _errors = new();
        private int _attempts;

        public (int Attempts, int Successes) Counts
        {
            get
            {
                lock (_lock)
                {
                    return (_attempts, _successLatenciesMilliseconds.Count);
                }
            }
        }

        public void RecordSuccess(TimeSpan latency)
        {
            lock (_lock)
            {
                _attempts++;
                _successLatenciesMilliseconds.Add(latency.TotalMilliseconds);
            }
        }

        public void RecordFailure(string error)
        {
            lock (_lock)
            {
                _attempts++;
                _errors[error] = _errors.GetValueOrDefault(error) + 1;
            }
        }

        public OperationReport ToReport(string name)
        {
            lock (_lock)
            {
                int successes = _successLatenciesMilliseconds.Count;
                return new OperationReport(
                    name,
                    _attempts,
                    successes,
                    _attempts - successes,
                    _attempts == 0 ? 0 : Math.Round(100.0 * successes / _attempts, 3),
                    LonghaulAnalysis.SummarizeLatencies(_successLatenciesMilliseconds),
                    new Dictionary<string, int>(_errors));
            }
        }
    }
}
