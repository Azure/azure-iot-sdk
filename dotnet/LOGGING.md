# Capturing logs

The SDK writes diagnostic messages through `System.Diagnostics.Trace`, including connection, provisioning,
retry, and feature-client diagnostics. Register a `TraceListener` before creating clients to send these messages
to a console, file, or another destination. No additional logging package is required.

For example, capture informational messages, warnings, and errors in a file:

```csharp
using System.Diagnostics;
using Microsoft.Azure.Iot.Device.Unified.Connection;
using Microsoft.Azure.Iot.Device.Unified.Models;

using var listener = new TextWriterTraceListener("iot-sdk.log")
{
    Filter = new EventTypeFilter(SourceLevels.Information),
    TraceOutputOptions = TraceOptions.DateTime,
};
Trace.Listeners.Add(listener);
Trace.AutoFlush = true;

try
{
    using var connectionClient = new ConnectionClient(new ConnectionClientOptions
    {
        EnableMqttLogging = true,
    });

    // Provision, connect, and use feature clients here.
}
finally
{
    Trace.Flush();
    Trace.Listeners.Remove(listener);
}
```

Add a `ConsoleTraceListener` to capture messages in the console instead of, or in addition to, a file.
Use `SourceLevels.Warning` to capture only warnings and errors, or `SourceLevels.All` to accept every severity.
`Trace.Listeners` and `Trace.AutoFlush` are process-wide settings: listeners can also receive trace messages
from your application and other libraries, not just this SDK. Keep listeners alive until clients have been
disconnected and disposed, then flush, remove, and dispose the listeners you added.

`ConnectionClientOptions.EnableMqttLogging` defaults to `false`. Setting it to `true` adds low-level MQTTnet
diagnostics to the same trace output when using the SDK's default MQTT client; it does not enable or disable
the SDK's own diagnostics. MQTTnet messages are currently emitted at trace `Information` level, regardless of
their original MQTTnet severity. If you supply your own `MqttClient`, configure its logging separately.

Treat captured logs as potentially sensitive, especially with MQTT logging enabled: they can contain device
identifiers, hostnames, topics, and message contents. Restrict access, limit retention, and review or redact
logs before sharing them. For production applications, use an appropriate rotation and retention policy.
`Trace` calls require the `TRACE` compilation symbol, which standard .NET SDK projects define in both Debug
and Release builds; retain it if you customize compilation constants.

See the dedicated [logging sample](samples/Unified/LoggingSample/) for a runnable example that captures
console and file logs while provisioning a device and sending telemetry.
