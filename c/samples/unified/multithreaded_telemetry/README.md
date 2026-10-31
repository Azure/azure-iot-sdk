<!-- Copyright (c) Microsoft. All rights reserved.
     Licensed under the MIT license. See LICENSE file in the project root for full license information. -->

# Telemetry from several threads

[`unified/telemetry`](../telemetry/README.md), with the readings produced on other threads. The
SDK is single-threaded: every call into it, for every client in the process, must come from one
thread or be serialized by the application ([architecture](../../../docs/architecture.md)). This
sample shows the application's side of that contract. Everything else is identical to `telemetry`,
so the difference between the two `main.c` files is exactly the feature.

## Sample features

- Hub generations: mqttv3 (MQTT 3.1.1) and mqttv5 (MQTT 5), whichever DPS assigns, including after the device is moved to a hub of the other generation.
- Two sensor threads push a reading every 2.5 seconds into [`reading_queue.c`](reading_queue.c), a bounded FIFO the application owns and locks (pthread mutex, or a critical section on Windows). The sensor threads never call the SDK.
- The main thread is the only one that calls the SDK. It runs `do_work()` and, between calls, pops one reading and sends it once the previous send has completed. The SDK callbacks run inside `do_work()` on that thread, so the state they share with the loop needs no lock.
- While the device is not connected, readings wait in the queue. When it is full, the oldest reading is dropped and counted.
- The alternative, one application mutex around every SDK call on every thread, also meets the contract. But `do_work()` holds that mutex for its whole timeout, and a callback must never try to take it again.
- The queue is tested under helgrind and DRD in CI (`tests/unit/sample_reading_queue_race_test.c`).
- Platforms: Linux and Windows.

## Service requirements, Configure

As [`unified/telemetry`](../telemetry/README.md#service-requirements): the same DPS enrollment and
the same environment variables.

## Build and run

Run from the repository's `c/` directory.

Linux:

```sh
cmake --preset linux-gcc-debug
cmake --build --preset linux-gcc-debug --target az_iot_sample_unified_multithreaded_telemetry
./build/linux-gcc-debug/samples/unified/az_iot_sample_multithreaded_telemetry
```

With az_mqtt (`-DAZ_IOT_WITH_AZ_MQTT=ON`), target `az_iot_sample_unified_multithreaded_telemetry_az_mqtt`
builds the same sample in `az_mqtt/` next to that binary ([adapters](../../README.md#build)).

Windows (*Developer PowerShell for VS 2022*):

```powershell
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug --target az_iot_sample_unified_multithreaded_telemetry
.\build\windows-msvc-debug\samples\unified\Debug\az_iot_sample_multithreaded_telemetry.exe
```

## How it ends

Runs about 60 seconds, then stops and joins the sensor threads. Exit code 0 when at least one
message was sent and the connection never settled at `FAULTED`; otherwise 1.

## Expected output

```
Sent 21 message(s) on AZ_IOT_CONNECTION_PROFILE_MQTT_V3, 0 failed, 0 reading(s) dropped.
```

The counts depend on how long connecting took.

## Where to look

| What | Code |
| --- | --- |
| The application's lock | `reading_queue.c`: `queue_lock()` / `queue_unlock()` around every field |
| Producers that never call the SDK | `main.c`: `sensor_main()` → `reading_queue_push()` |
| The one SDK thread | `main.c` main loop: `az_iot_connection_client_do_work()`, then `reading_queue_pop()` → `send_one()` |
| Shutdown order | `sensors_stop()` (stop, join), then `reading_queue_deinit()` |
