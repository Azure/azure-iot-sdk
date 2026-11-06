// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using System.Globalization;

namespace Microsoft.Azure.Iot.Device.IntegrationTests.Longhaul
{
    /// <summary>
    /// Longhaul run configuration, read from environment variables. The longhaul tests are skipped unless
    /// <see cref="DurationVariable"/> is set, so that a plain <c>dotnet test</c> never starts a multi-hour run.
    /// </summary>
    public sealed class LonghaulSettings
    {
        public const string DurationVariable = "LONGHAUL_DURATION_MINUTES";

        public required TimeSpan Duration { get; init; }

        public required string OutputDirectory { get; init; }

        public required TimeSpan TelemetryInterval { get; init; }

        public required TimeSpan ReportedPropertiesInterval { get; init; }

        public required TimeSpan DesiredPropertiesInterval { get; init; }

        public required TimeSpan DirectMethodInterval { get; init; }

        public required TimeSpan CloudToDeviceInterval { get; init; }

        /// <summary>
        /// How long a single operation may take, end to end, before it counts as failed.
        /// </summary>
        public required TimeSpan OperationTimeout { get; init; }

        /// <summary>
        /// How often process resource usage (memory, handles, threads) is sampled.
        /// </summary>
        public required TimeSpan SampleInterval { get; init; }

        /// <summary>
        /// How often a progress line is written to the test runner's diagnostic output.
        /// </summary>
        public required TimeSpan ProgressInterval { get; init; }

        public required double MinSuccessRatePercent { get; init; }

        public required double MaxManagedMemoryGrowthMegabytes { get; init; }

        public static LonghaulSettings? FromEnvironment()
        {
            string? duration = Environment.GetEnvironmentVariable(DurationVariable);
            if (string.IsNullOrWhiteSpace(duration))
            {
                return null;
            }

            double durationMinutes = ParsePositive(DurationVariable, duration);

            return new LonghaulSettings
            {
                Duration = TimeSpan.FromMinutes(durationMinutes),
                OutputDirectory = Environment.GetEnvironmentVariable("LONGHAUL_OUTPUT_DIRECTORY") is { Length: > 0 } outputDirectory
                    ? outputDirectory
                    : Path.Combine(Directory.GetCurrentDirectory(), "longhaul-results"),
                TelemetryInterval = Seconds("LONGHAUL_TELEMETRY_INTERVAL_SECONDS", 10),
                ReportedPropertiesInterval = Seconds("LONGHAUL_REPORTED_PROPERTIES_INTERVAL_SECONDS", 30),
                DesiredPropertiesInterval = Seconds("LONGHAUL_DESIRED_PROPERTIES_INTERVAL_SECONDS", 60),
                DirectMethodInterval = Seconds("LONGHAUL_DIRECT_METHOD_INTERVAL_SECONDS", 60),
                CloudToDeviceInterval = Seconds("LONGHAUL_C2D_INTERVAL_SECONDS", 60),
                OperationTimeout = Seconds("LONGHAUL_OPERATION_TIMEOUT_SECONDS", 60),
                SampleInterval = Seconds("LONGHAUL_SAMPLE_INTERVAL_SECONDS", 60),
                ProgressInterval = Seconds("LONGHAUL_PROGRESS_INTERVAL_SECONDS", 600),
                MinSuccessRatePercent = Number("LONGHAUL_MIN_SUCCESS_PERCENT", 99),
                MaxManagedMemoryGrowthMegabytes = Number("LONGHAUL_MAX_MANAGED_MEMORY_GROWTH_MB", 64),
            };
        }

        private static TimeSpan Seconds(string name, double defaultValue) => TimeSpan.FromSeconds(Number(name, defaultValue));

        private static double Number(string name, double defaultValue)
        {
            string? value = Environment.GetEnvironmentVariable(name);
            return string.IsNullOrWhiteSpace(value) ? defaultValue : ParsePositive(name, value);
        }

        private static double ParsePositive(string name, string value)
        {
            if (!double.TryParse(value, NumberStyles.Float, CultureInfo.InvariantCulture, out double parsed) || parsed <= 0)
            {
                throw new InvalidOperationException($"Environment variable '{name}' must be a positive number; got '{value}'.");
            }

            return parsed;
        }
    }
}
