# Logging sample

Captures SDK diagnostics and optional low-level MQTTnet diagnostics through `System.Diagnostics.Trace`
while provisioning an X.509 device through DPS, connecting to its assigned IoT Hub, and sending telemetry.

## Prerequisites

- The .NET 10 SDK.
- Run [SetupSampleDevice](../../SetupSampleDevice/) as described in the [samples README](../../README.md)
  to create an X.509 enrollment and save the sample credentials. DPS must be linked to an IoT Hub.

## Run

From the `dotnet\samples` directory:

```powershell
dotnet run --project Unified\LoggingSample
```

The sample sends telemetry every two seconds until you press Ctrl+C or its 30-second timeout expires.
It writes diagnostic messages to the console and to `iot-sdk.log` in the current working directory.
**Each run overwrites that file.**

## How capture works

`Program.cs` registers both listeners before creating the connection client:

- `ConsoleTraceListener` accepts informational messages, warnings, and errors using
  `EventTypeFilter(SourceLevels.Information)`.
- `TextWriterTraceListener` accepts all severities and includes the UTC event time using
  `TraceOptions.DateTime`. Change its filter to `SourceLevels.Warning` to capture only warnings and errors.

`Trace.AutoFlush = true` flushes after every write so messages are available immediately, at the cost of
additional I/O. The sample keeps listeners attached through client disposal, then flushes and removes them
before disposing them. These are process-wide listeners, so the sample's own `Trace.TraceInformation`
messages and trace messages from other libraries are captured too.

`ConnectionClientOptions.EnableMqttLogging = true` routes the default MQTTnet client's diagnostics through
the same listeners. Set it to `false` to omit MQTTnet diagnostics while retaining SDK logs. MQTTnet
messages are emitted at trace `Information` level, so a warning-only filter excludes them even when the
original MQTTnet message describes a warning or error.

Logs may include device identifiers, hostnames, topics, and message contents. Restrict access and review or
redact logs before sharing them. This sample does not implement production log rotation or retention.
See [Capturing logs](../../../README.md#capturing-logs) for more details.
