# Connect Samples

This guide is for first-time readers of this repository. It walks you through starting a local broker, building the samples, and running them.

## What the samples do

[az_mqtt5_sample_connect.c](az_mqtt5_sample_connect.c) (mqttv5, `az_mqtt::mqttv5`) and
[az_mqtt3_sample_connect.c](az_mqtt3_sample_connect.c) (mqttv3, `az_mqtt::mqttv3`) do the same
thing, each with its own API. Each one:

- Connects to the broker
- Subscribes to mqtt5/test/# (mqttv3: mqtt3/test/#)
- Publishes one test message
- Processes incoming events (SUBACK, PUBACK, echoed PUBLISH)
- Disconnects cleanly

## Prerequisites

- CMake 3.14+
- A C compiler toolchain (MSVC, clang, or gcc)
- Docker
- OpenSSL command-line tool (optional, only needed if you want the broker TLS listener/certs from the helper scripts)

## 1. Start a local broker

From repo root:

### Windows PowerShell

```powershell
.\tests\start_broker.ps1
```

### Linux/macOS

```bash
./tests/start_broker.sh
```

When startup succeeds you should see lines similar to:

```text
Plain TCP: localhost:1883
TLS:       localhost:8883
```

If OpenSSL is not available, the Windows script falls back to plain TCP only and prints:

```text
Plain TCP: localhost:1883
TLS:       disabled (OpenSSL not found)
```

## 2. Configure and build the samples

From repo root:

### Windows (Visual Studio generator)

```powershell
cmake -S . -B build -DAZ_MQTT_BUILD_SAMPLES=ON
cmake --build build --config Debug --target az_mqtt5_sample_connect az_mqtt3_sample_connect
```

### Linux/macOS (Ninja/Unix Makefiles)

```bash
cmake -S . -B build -DAZ_MQTT_BUILD_SAMPLES=ON
cmake --build build --target az_mqtt5_sample_connect az_mqtt3_sample_connect
```

## 3. Run a sample

Defaults are host=localhost and port=1883:

### Windows

```powershell
.\build\samples\Debug\az_mqtt5_sample_connect.exe
```

### Linux/macOS

```bash
./build/samples/az_mqtt5_sample_connect
```

You can also pass host and port explicitly:

```text
az_mqtt5_sample_connect [host] [port]
```

Example:

```text
az_mqtt5_sample_connect localhost 1883
```

## Expected successful output

A successful mqttv5 run looks similar to this (mqttv3 prints `MQTT3 Sample`, uses `mqtt3/test/...` and shows return codes instead of reason codes):

```text
MQTT5 Sample: Connecting to localhost:1883
Connecting...
[CONNACK] reason=0 session_present=0
Connected!
Subscribe sent (packet_id=1)
Publish sent (packet_id=2)
Processing events (10 iterations)...
[SUBACK] packet_id=1 reason_codes=[0x01]
[PUBACK] packet_id=2 reason=0
[PUBLISH received] topic="mqtt5/test/hello" qos=1 payload_len=45
  payload: "Hello from az_mqtt5_client! Zero allocations."
Disconnecting...
Done.
```

The exact packet ids and reason codes can vary by broker behavior.

## Stop the broker

### Windows PowerShell

```powershell
.\tests\start_broker.ps1 -Stop
```

### Linux/macOS

```bash
./tests/start_broker.sh stop
```
