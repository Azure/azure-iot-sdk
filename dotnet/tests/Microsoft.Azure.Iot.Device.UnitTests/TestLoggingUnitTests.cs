// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Tests;
using System.Diagnostics;
using Xunit;

namespace Microsoft.Azure.Iot.Device.UnitTests
{
    public class TestLoggingUnitTests
    {
        [Fact]
        public void TraceListener_PersistsAllSeveritiesWithTiming()
        {
            string directory = Path.Combine(Path.GetTempPath(), $"iot-test-logging-{Guid.NewGuid():N}");
            string logPath = Path.Combine(directory, "sdk.log");
            TextWriterTraceListener listener = TestLogging.CreateListener(logPath);
            Trace.Listeners.Add(listener);
            try
            {
                string marker = Guid.NewGuid().ToString();
                Trace.TraceInformation("{0}: information", marker);
                Trace.TraceWarning("{0}: warning", marker);
                Trace.TraceError("{0}: error", marker);
                Trace.Flush();

                using StreamReader reader = new(File.Open(logPath, FileMode.Open, FileAccess.Read, FileShare.ReadWrite));
                string log = reader.ReadToEnd();
                Assert.Contains($"{marker}: information", log);
                Assert.Contains($"{marker}: warning", log);
                Assert.Contains($"{marker}: error", log);
                Assert.Contains("DateTime=", log);
                Assert.Contains("ThreadId=", log);
            }
            finally
            {
                Trace.Listeners.Remove(listener);
                listener.Dispose();
                File.Delete(logPath);
                Directory.Delete(directory);
            }
        }
    }
}
