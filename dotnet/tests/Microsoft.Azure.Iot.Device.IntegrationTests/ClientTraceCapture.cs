// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using System.Diagnostics;
using System.Globalization;
using Microsoft.Azure.Iot.Device.IntegrationTests;
using Xunit;

[assembly: AssemblyFixture(typeof(ClientTraceCapture))]

namespace Microsoft.Azure.Iot.Device.IntegrationTests
{
    /// <summary>
    /// Writes the client's traces to the file named by E2E_CLIENT_TRACE_FILE (if set), each tagged with the test that
    /// produced it, so that a failure in CI can be diagnosed from the client's side.
    /// </summary>
    public sealed class ClientTraceCapture : IDisposable
    {
        public const string TraceFileVariable = "E2E_CLIENT_TRACE_FILE";

        private readonly TestTaggingTraceListener? _listener;

        public ClientTraceCapture()
        {
            string? path = Environment.GetEnvironmentVariable(TraceFileVariable);
            if (string.IsNullOrWhiteSpace(path))
            {
                return;
            }

            _listener = new TestTaggingTraceListener(path);
            Trace.AutoFlush = true;
            Trace.Listeners.Add(_listener);
        }

        public void Dispose()
        {
            if (_listener != null)
            {
                Trace.Listeners.Remove(_listener);
                _listener.Dispose();
            }
        }

        private sealed class TestTaggingTraceListener(string path) : TextWriterTraceListener(path)
        {
            public override void TraceEvent(TraceEventCache? eventCache, string source, TraceEventType eventType, int id, string? message)
            {
                WriteLine(string.Create(
                    CultureInfo.InvariantCulture,
                    $"{DateTime.UtcNow:HH:mm:ss.fff} [{Environment.CurrentManagedThreadId}] [{TestContext.Current.Test?.TestDisplayName ?? "-"}] {eventType}: {message}"));
            }

            public override void TraceEvent(TraceEventCache? eventCache, string source, TraceEventType eventType, int id, string? format, params object?[]? args)
            {
                TraceEvent(eventCache, source, eventType, id, args == null ? format : string.Format(CultureInfo.InvariantCulture, format ?? string.Empty, args));
            }
        }
    }
}
