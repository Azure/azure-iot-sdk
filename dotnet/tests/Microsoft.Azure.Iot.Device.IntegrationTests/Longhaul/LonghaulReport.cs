// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using System.Globalization;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace Microsoft.Azure.Iot.Device.IntegrationTests.Longhaul
{
    public sealed record LonghaulReport(
        string Scenario,
        string ApiSet,
        string Hub,
        string OperatingSystem,
        string Runtime,
        DateTimeOffset StartedUtc,
        double PlannedDurationMinutes,
        double ActualDurationMinutes,
        IReadOnlyList<OperationReport> Operations,
        ConnectionReport Connection,
        ResourceReport Resources,
        ThresholdReport Thresholds,
        IReadOnlyList<string> Violations)
    {
        private static readonly JsonSerializerOptions s_jsonOptions = new(JsonSerializerDefaults.Web)
        {
            WriteIndented = true,
            DefaultIgnoreCondition = JsonIgnoreCondition.Never,
        };

        public bool Passed => Violations.Count == 0;

        public string ToJson() => JsonSerializer.Serialize(this, s_jsonOptions);

        /// <summary>
        /// Writes <c>{Scenario}.json</c> (the full report, including every resource sample) and <c>{Scenario}.md</c>
        /// (a human-readable analysis) into <paramref name="directory"/>.
        /// </summary>
        public void WriteTo(string directory)
        {
            Directory.CreateDirectory(directory);
            File.WriteAllText(Path.Combine(directory, $"{Scenario}.json"), ToJson());
            File.WriteAllText(Path.Combine(directory, $"{Scenario}.md"), ToMarkdown());
        }

        public string ToMarkdown()
        {
            StringBuilder md = new();
            md.AppendLine(Invariant($"## Longhaul: {Scenario} {(Passed ? "✅ passed" : "❌ failed")}"));
            md.AppendLine();
            md.AppendLine(Invariant($"API set **{ApiSet}** on an **{Hub}** IoT hub; {OperatingSystem}, {Runtime}."));
            md.AppendLine(Invariant($"Started {StartedUtc:yyyy-MM-dd HH:mm} UTC; ran {ActualDurationMinutes:0.#} of {PlannedDurationMinutes:0.#} planned minutes."));
            md.AppendLine();

            if (Violations.Count > 0)
            {
                md.AppendLine("### Threshold violations");
                md.AppendLine();
                foreach (string violation in Violations)
                {
                    md.AppendLine(Invariant($"- {violation}"));
                }

                md.AppendLine();
            }

            md.AppendLine("### Operations");
            md.AppendLine();
            md.AppendLine("| Operation | Attempts | Succeeded | Failed | Success % | p50 ms | p95 ms | p99 ms | Max ms | Errors |");
            md.AppendLine("|---|---:|---:|---:|---:|---:|---:|---:|---:|---|");
            foreach (OperationReport operation in Operations)
            {
                string errors = operation.Errors.Count == 0
                    ? ""
                    : string.Join("<br>", operation.Errors.OrderByDescending(e => e.Value).Select(e => Invariant($"{e.Key} ×{e.Value}")));
                LatencyReport latency = operation.LatencyMilliseconds;
                md.AppendLine(
                    Invariant($"| {operation.Name} | {operation.Attempts} | {operation.Successes} | {operation.Failures} | {operation.SuccessRatePercent:0.###} | ") +
                    Invariant($"{latency.P50:0} | {latency.P95:0} | {latency.P99:0} | {latency.Max:0} | {errors} |"));
            }

            md.AppendLine();
            md.AppendLine("### Connection");
            md.AppendLine();
            md.AppendLine("| Disconnects | Reconnects | Total time disconnected (s) | Longest outage (s) | Re-provisionings | Unrecoverable faults |");
            md.AppendLine("|---:|---:|---:|---:|---:|---:|");
            md.AppendLine(
                Invariant($"| {Connection.Disconnects} | {Connection.Connects} | {Connection.TotalDisconnectedSeconds:0.#} | {Connection.LongestOutageSeconds:0.#} | ") +
                Invariant($"{Connection.ReprovisioningStarts} | {Connection.UnrecoverableFaults} |"));
            if (Connection.DisconnectReasons.Count > 0)
            {
                md.AppendLine();
                md.AppendLine("Disconnect reasons: " + string.Join(", ", Connection.DisconnectReasons.OrderByDescending(r => r.Value).Select(r => Invariant($"{r.Key} ×{r.Value}"))));
            }

            foreach (string fault in Connection.FaultMessages)
            {
                md.AppendLine();
                md.AppendLine(Invariant($"Fault: `{fault}`"));
            }

            md.AppendLine();
            md.AppendLine("### Resources");
            md.AppendLine();
            md.AppendLine("| Metric | Start | End | Peak | Change |");
            md.AppendLine("|---|---:|---:|---:|---:|");
            AppendResourceRow(md, "Managed heap (MB, after full GC)", Resources.Samples.Select(s => s.ManagedMegabytes).ToList(), "0.0");
            AppendResourceRow(md, "Working set (MB)", Resources.Samples.Select(s => s.WorkingSetMegabytes).ToList(), "0.0");
            AppendResourceRow(md, "Handles", Resources.Samples.Select(s => (double)s.HandleCount).ToList(), "0");
            AppendResourceRow(md, "Threads", Resources.Samples.Select(s => (double)s.ThreadCount).ToList(), "0");
            md.AppendLine();
            md.AppendLine(Resources.ManagedMemoryGrowthMegabytes is double growth && Resources.ManagedMemorySlopeMegabytesPerHour is double slope
                ? Invariant($"Managed heap growth after a {Resources.WarmupMinutes:0.#} minute warm-up: **{growth:+0.0;-0.0;0.0} MB** (trend {slope:+0.00;-0.00;0.00} MB/hour).")
                : Invariant($"Too few samples after the {Resources.WarmupMinutes:0.#} minute warm-up to analyze managed heap growth."));
            md.AppendLine();
            return md.ToString();
        }

        private static void AppendResourceRow(StringBuilder md, string name, IReadOnlyList<double> values, string format)
        {
            if (values.Count == 0)
            {
                md.AppendLine(Invariant($"| {name} | | | | |"));
                return;
            }

            string Format(double value) => value.ToString(format, CultureInfo.InvariantCulture);
            double change = values[^1] - values[0];
            md.AppendLine(Invariant($"| {name} | {Format(values[0])} | {Format(values[^1])} | {Format(values.Max())} | {(change >= 0 ? "+" : "")}{Format(change)} |"));
        }

        private static string Invariant(FormattableString value) => value.ToString(CultureInfo.InvariantCulture);
    }

    public sealed record OperationReport(
        string Name,
        int Attempts,
        int Successes,
        int Failures,
        double SuccessRatePercent,
        LatencyReport LatencyMilliseconds,
        IReadOnlyDictionary<string, int> Errors);

    public sealed record LatencyReport(double Mean, double P50, double P95, double P99, double Max);

    public sealed record ConnectionReport(
        int Connects,
        int Disconnects,
        IReadOnlyDictionary<string, int> DisconnectReasons,
        int ReprovisioningStarts,
        int UnrecoverableFaults,
        IReadOnlyList<string> FaultMessages,
        double TotalDisconnectedSeconds,
        double LongestOutageSeconds);

    public sealed record ResourceSample(
        double ElapsedMinutes,
        double ManagedMegabytes,
        double WorkingSetMegabytes,
        int HandleCount,
        int ThreadCount,
        bool Connected);

    public sealed record ResourceReport(
        double WarmupMinutes,
        double? ManagedMemoryGrowthMegabytes,
        double? ManagedMemorySlopeMegabytesPerHour,
        IReadOnlyList<ResourceSample> Samples);

    public sealed record ThresholdReport(double MinSuccessRatePercent, double MaxManagedMemoryGrowthMegabytes);

    internal static class LonghaulAnalysis
    {
        // Growth is measured between the median of this many samples just after warm-up and the median of this many
        // samples at the end of the run; medians keep one unusually high or low sample from skewing the comparison.
        private const int SamplesPerWindow = 5;

        public static LatencyReport SummarizeLatencies(IReadOnlyCollection<double> latencies)
        {
            if (latencies.Count == 0)
            {
                return new LatencyReport(0, 0, 0, 0, 0);
            }

            double[] sorted = latencies.Order().ToArray();
            return new LatencyReport(
                Math.Round(sorted.Average(), 1),
                Math.Round(Percentile(sorted, 50), 1),
                Math.Round(Percentile(sorted, 95), 1),
                Math.Round(Percentile(sorted, 99), 1),
                Math.Round(sorted[^1], 1));
        }

        /// <summary>
        /// Nearest-rank percentile of an ascending array.
        /// </summary>
        public static double Percentile(double[] sorted, double percentile)
        {
            int rank = (int)Math.Ceiling(percentile / 100 * sorted.Length);
            return sorted[Math.Clamp(rank - 1, 0, sorted.Length - 1)];
        }

        /// <summary>
        /// The process warms up (JIT, connection pools, caches) during the first part of a run, so memory growth is
        /// measured from the end of a warm-up of 10% of the run, at least 5 and at most 30 minutes.
        /// </summary>
        public static ResourceReport AnalyzeResources(IReadOnlyList<ResourceSample> samples, TimeSpan plannedDuration)
        {
            double warmupMinutes = Math.Clamp(plannedDuration.TotalMinutes * 0.1, 5, 30);
            List<ResourceSample> steady = samples.Where(s => s.ElapsedMinutes >= warmupMinutes).ToList();
            if (steady.Count < 2 * SamplesPerWindow)
            {
                return new ResourceReport(warmupMinutes, null, null, samples);
            }

            double baseline = Median(steady.Take(SamplesPerWindow).Select(s => s.ManagedMegabytes));
            double final = Median(steady.TakeLast(SamplesPerWindow).Select(s => s.ManagedMegabytes));
            double slopePerMinute = LeastSquaresSlope(steady.Select(s => (s.ElapsedMinutes, s.ManagedMegabytes)).ToList());

            return new ResourceReport(warmupMinutes, Math.Round(final - baseline, 2), Math.Round(slopePerMinute * 60, 3), samples);
        }

        public static List<string> FindViolations(
            IReadOnlyList<OperationReport> operations,
            ConnectionReport connection,
            ResourceReport resources,
            ThresholdReport thresholds)
        {
            List<string> violations = new();
            foreach (OperationReport operation in operations)
            {
                if (operation.Attempts == 0)
                {
                    violations.Add($"{operation.Name} never ran.");
                }
                else if (operation.SuccessRatePercent < thresholds.MinSuccessRatePercent)
                {
                    violations.Add(string.Create(CultureInfo.InvariantCulture,
                        $"{operation.Name} succeeded {operation.SuccessRatePercent:0.###}% of the time ({operation.Failures} of {operation.Attempts} failed); the minimum is {thresholds.MinSuccessRatePercent:0.###}%."));
                }
            }

            if (connection.UnrecoverableFaults > 0)
            {
                violations.Add($"The connection faulted unrecoverably {connection.UnrecoverableFaults} time(s), so the run ended early.");
            }

            if (resources.ManagedMemoryGrowthMegabytes is double growth && growth > thresholds.MaxManagedMemoryGrowthMegabytes)
            {
                violations.Add(string.Create(CultureInfo.InvariantCulture,
                    $"The managed heap grew {growth:0.0} MB after warm-up; the maximum is {thresholds.MaxManagedMemoryGrowthMegabytes:0.0} MB."));
            }

            return violations;
        }

        private static double Median(IEnumerable<double> values)
        {
            double[] sorted = values.Order().ToArray();
            int middle = sorted.Length / 2;
            return sorted.Length % 2 == 1 ? sorted[middle] : (sorted[middle - 1] + sorted[middle]) / 2;
        }

        private static double LeastSquaresSlope(IReadOnlyList<(double X, double Y)> points)
        {
            double meanX = points.Average(p => p.X);
            double meanY = points.Average(p => p.Y);
            double numerator = points.Sum(p => (p.X - meanX) * (p.Y - meanY));
            double denominator = points.Sum(p => (p.X - meanX) * (p.X - meanX));
            return denominator == 0 ? 0 : numerator / denominator;
        }
    }
}
