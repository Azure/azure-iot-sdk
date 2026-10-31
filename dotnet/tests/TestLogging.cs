// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using System.Diagnostics;
using System.Reflection;
using System.Runtime.CompilerServices;

namespace Microsoft.Azure.Iot.Device.Tests
{
    internal static class TestLogging
    {
        internal static bool IsEnabled =>
            string.Equals(Environment.GetEnvironmentVariable("AZ_IOT_TEST_FULL_LOGGING"), "true", StringComparison.OrdinalIgnoreCase);

        [ModuleInitializer]
        internal static void Initialize()
        {
            if (!IsEnabled)
            {
                return;
            }

            string directory = Environment.GetEnvironmentVariable("AZ_IOT_TEST_LOG_DIRECTORY")
                ?? Path.Combine(Directory.GetCurrentDirectory(), "TestResults", "sdk-logs");
            string logPath = Path.Combine(directory, $"{Assembly.GetExecutingAssembly().GetName().Name}-{Environment.ProcessId}.log");
            Trace.Listeners.Add(CreateListener(logPath));
            Trace.AutoFlush = true;
            AppDomain.CurrentDomain.ProcessExit += (_, _) => Trace.Flush();
            Trace.TraceInformation("Full SDK trace logging enabled. Log file: {0}", logPath);
        }

        internal static TextWriterTraceListener CreateListener(string logPath)
        {
            Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(logPath))!);
            return new TextWriterTraceListener(new StreamWriter(logPath) { AutoFlush = true })
            {
                Filter = new EventTypeFilter(SourceLevels.All),
                TraceOutputOptions = TraceOptions.DateTime | TraceOptions.ThreadId,
            };
        }
    }
}
