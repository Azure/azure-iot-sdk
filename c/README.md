<!-- Copyright (c) Microsoft. All rights reserved.
     Licensed under the MIT license. See LICENSE file in the project root for full license information. -->

# azure-iot-sdk

C99 client SDK for Azure IoT Hub Classic and IoT Hub Next (AEG).

> Status: **early bootstrap**. See [docs/design.md](docs/design.md) for the architecture and [docs/devnotes.md](docs/devnotes.md) for original design discussion notes.

## Highlights

- **C99-strict**, no submodules. Dependencies via vcpkg manifest (primary) or CPM.cmake (fallback).
- **Two layered public APIs**: API A (low-level, single-threaded `do_work()` pump) and API B (convenience wrapper with internal worker thread).
- **Pluggable MQTT** with version + role tagging:
  - DPS + IoTHub-Classic require **MQTT v3.1.1**.
  - IoTHub-Next requires **MQTT v5**.
  - Adapters register factories via `az_iot_connection_client_register_mqtt_factory()`. The core picks the right `(version, role)` per session and instantiates a fresh adapter for each.
- **Default adapter**: Paho-C (v3.1.1 + v5).
- **Built on [azure-sdk-for-c](https://github.com/Azure/azure-sdk-for-c)** (pinned via `FetchContent`):
  - `az::core` for spans, JSON, logging, contexts, result codes.
  - `az::iot::hub` for IoTHub-Classic MQTT topic build/parse.
  - `az::iot::provisioning` for Azure DPS MQTT topic build/parse.
  - IoTHub-Next protocol logic lives in this repo (no upstream library yet).

## Build (Phase 0)

Configure and build with one of the provided presets:

```sh
cmake --preset linux-gcc-debug
cmake --build --preset linux-gcc-debug
ctest --preset linux-gcc-debug
```

```pwsh
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug --config Debug
ctest --preset windows-msvc-debug -C Debug
```

Run the smoke binary:

```sh
./build/linux-gcc-debug/samples/az_iot_smoke
```

## Project layout

```
inc/azure/iot/        public headers (API A)
src/{core,features}/ implementation
adapters/{paho,rust_mqtt}/ MQTT adapters
samples/                  examples + smoke
tests/                    ctest suites
tests/conformance/        reusable MQTT iface conformance suite
docs/                     design + dev notes
cmake/                    helpers (warnings, options, CPM placeholder)
```

## MQTT adapter conformance suite

`tests/conformance/` ships a reusable cmocka-based test suite that operates strictly through the public `az_iot_mqtt_iface` vtable. It is the contract any MQTT client+adapter must satisfy to be usable with `azure-iot-sdk`. Two CMake targets ship today:

- `az_iot_conformance_paho_v3` — validates the bundled Paho adapter as MQTTv3.1.1.
- `az_iot_conformance_paho_v5` — validates the bundled Paho adapter as MQTTv5.

To validate your own adapter, link against `az_iot_conformance` and call `az_iot_conformance_run(suite, factory)` from a small harness exe (see [tests/conformance/paho_v3_main.c](tests/conformance/paho_v3_main.c)). A full walk-through is in [docs/how_to_byo_mqtt_client.md](docs/how_to_byo_mqtt_client.md).

The suite needs a reachable MQTT broker; configure via:

```sh
export az_iot_MQTT_BROKER_HOST=localhost
export az_iot_MQTT_BROKER_PORT=1883
ctest --preset linux-gcc-debug --output-on-failure -R conformance
```

Without those env vars the conformance tests report `Skipped` (CTest exit 77) so the rest of the suite stays green for developers without a broker handy. CI runs an `eclipse-mosquitto:2` service container automatically.

## Quickstart — send telemetry in ~30 lines

The easiest path uses **API B** (`az_iot_easy_client`), which spins one
worker thread internally and exposes blocking `*_sync` calls.

```c
#include "azure/iot/easy/az_iot_easy_client.h"
#include "azure/iot/az_iot_certificate_provider_pem.h"
#include "azure/iot/az_iot_telemetry_client.h"
#include "azure/iot/adapters/az_iot_adapter_paho.h"

az_iot_certificate_provider_pem_options_t pem = {
    .trusted_ca_pem_path  = getenv("AZ_IOT_TRUSTED_CA"),  /* optional */
    .client_cert_pem_path = getenv("AZ_IOT_CLIENT_CERT"),
    .client_key_pem_path  = getenv("AZ_IOT_CLIENT_KEY"),
};
az_iot_certificate_provider_pem_t cm;
az_iot_certificate_provider_pem_init(&cm, &pem);

az_iot_easy_options_t opts = {
    .host = getenv("AZ_IOT_HOST"), .port = 8883,
    .client_id = getenv("AZ_IOT_DEVICE_ID"), .certificate_provider = &cm.base,
    .reconnect = { .initial_delay_ms = 500, .max_delay_ms = 10000, .jitter_pct = 25 },
};
az_iot_easy_client_t* easy = NULL;
az_iot_easy_client_create(&opts, &easy);

az_iot_mqtt_factory_t* paho = az_iot_paho_factory_create_v3_1_1();
az_iot_easy_client_register_mqtt_factory(easy, paho);

az_iot_easy_client_open_sync(easy, 30000);

const char* body = "{\"hello\":\"world\"}";
az_iot_telemetry_message_t msg = {
    .payload = (const uint8_t*)body, .payload_len = strlen(body),
    .content_type = "application/json",
};
az_iot_easy_client_send_telemetry_sync(easy, &msg, 5000);

az_iot_easy_client_close_sync(easy, 5000);
az_iot_easy_client_destroy(easy);
az_iot_paho_factory_destroy(paho);
az_iot_certificate_provider_pem_deinit(&cm);
```

The full source is in [samples/telemetry_quickstart/main.c](samples/telemetry_quickstart/main.c).

To run it, set the env vars and invoke the built binary:

```sh
export AEG_HOST=<my-hub>.azure-devices.net
export AEG_DEVICE_ID=my-device
export AEG_CLIENT_CERT=/path/to/device.crt.pem
export AEG_CLIENT_KEY=/path/to/device.key.pem
export AEG_TRUSTED_CA=/path/to/ca.pem        # optional

./build/linux-gcc-debug/samples/az_iot_sample_telemetry_quickstart
```

If any required env var is unset, the sample prints a usage hint and exits 0
so it can be safely included in a default build matrix without a broker.

## Samples

| Sample | API | What it shows |
| --- | --- | --- |
| [telemetry_quickstart](samples/telemetry_quickstart/) | B (Easy) | Cert mgr + Easy client + 5 QoS-0 sends. The 3-minute path. |
| [twin_get_patch](samples/twin_get_patch/) | A (core) | Manual `do_work()` pump, `twin_get` + `patch_reported`. |
| [direct_method_responder](samples/direct_method_responder/) | A (core) | Subscribe for direct methods, echo payload back via `az_iot_direct_method_respond`. |
| [smoke](samples/smoke/) | — | Phase 0 link-only smoke binary. |

## API A vs API B

There are two layered public API surfaces. They are **fully interoperable** — API B is a thin wrapper over A — and both ship in the same package.

|  | **API A** (core) | **API B** (easy) |
| --- | --- | --- |
| Header root | `azure/iot/...` | `azure/iot/easy/...` |
| Library target | `az_iot_core` | `az_iot_easy` |
| Threading | Single-threaded; **caller** drives `do_work(timeout_ms)`. | One internal worker thread per Easy client. |
| Calls | Non-blocking, completion via callback. | Blocking `*_sync(timeout_ms)`. |
| Callbacks fire on | The thread that called `do_work()`. | The internal worker thread (with internal lock held). |
| Allocations | Bounded; no hidden heap on the hot path. | Same, plus one mutex + condvar + thread per client. |
| Best for | Embedded, RTOS-friendly designs, single-threaded apps, custom event loops. | Desktop/server apps that want a simple synchronous send/receive API. |
| DPS support | Yes (internal to `az_iot_connection_client`; set `opts.dps` fields). | Removed. |

See [docs/api_a_vs_b.md](docs/api_a_vs_b.md) for the full comparison.
