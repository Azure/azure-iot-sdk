<!-- Copyright (c) Microsoft. All rights reserved.
     Licensed under the MIT license. See LICENSE file in the project root for full license information. -->

# azure-iot-sdk

C99 client SDK for Azure IoT Hub Classic and IoT Hub Next (AEG).

> Status: **early bootstrap**. See [docs/design.md](docs/design.md) for the architecture and [docs/devnotes.md](docs/devnotes.md) for original design discussion notes.

## Highlights

- **C99-strict**, no submodules. Dependencies via vcpkg manifest (primary) or CPM.cmake (fallback).
- **Single public API**: low-level, single-threaded `do_work()` pump; all callbacks fire on the caller's thread.
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

Run a sample binary:

```sh
./build/linux-gcc-debug/samples/az_iot_sample_telemetry_gen1
```

## Project layout

```
inc/azure/iot/        public headers
src/{core,features}/ implementation
adapters/{paho,rust_mqtt}/ MQTT adapters
samples/                  examples
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
export AZ_IOT_MQTT_BROKER_HOST=localhost
export AZ_IOT_MQTT_BROKER_PORT=1883
ctest --preset linux-gcc-debug --output-on-failure -R conformance
```

Those env vars are required once the conformance tests are registered: configure with `-DAZ_IOT_BUILD_CONFORMANCE_TESTS=ON` (the Linux presets do it for you) and an unset `AZ_IOT_MQTT_BROKER_HOST` is then a **failure**, not a skip. Without the option the tests are simply not registered, so a build with no broker to hand stays green by not pretending to run them. CI runs an `eclipse-mosquitto:2` service container automatically.

## Quickstart — send telemetry

The SDK is a single-threaded pump: the application drives `do_work()` and every
callback fires on that thread. Provisioning through DPS is internal to the
connection client — leave `host` unset and fill in the `dps` fields.

`open()` is **non-blocking**: it starts provisioning and connecting, and the
application must pump until the state callback reports `CONNECTED` before
sending. Every call below returns `az_iot_result` and is declared `AZ_NODISCARD`,
so ignoring one is a compile warning (an error under this project's default
`AZ_IOT_WARNINGS_AS_ERRORS`).

```c
#include <stdbool.h>
#include <stdlib.h>

#include "azure/iot/az_iot.h"
#include "azure/iot/adapters/az_iot_adapter_paho.h"

typedef struct
{
  az_iot_connection_state conn_state;
  az_iot_connection_profile connection_profile;
  bool profile_valid;
  bool send_done;
  az_iot_result send_status;
} app_ctx;

static void on_conn_state(const az_iot_connection_state_event* event, void* user_ctx)
{
  (void)event->reason;
  app_ctx* ctx = (app_ctx*)user_ctx;
  ctx->conn_state = event->state;
  if (event->state == AZ_IOT_CONN_STATE_CONNECTED && event->profile != NULL)
  {
    ctx->connection_profile = event->profile->connection_profile;
    ctx->profile_valid = true;
  }
}

static void on_send_done(az_iot_result status, void* user_ctx)
{
  app_ctx* ctx = (app_ctx*)user_ctx;
  ctx->send_status = status;
  ctx->send_done = true;
}

int main(void)
{
  app_ctx ctx = { 0 };

  /* Certificate provider (X.509) */
  az_iot_certificate_provider_pem_options pem = az_iot_certificate_provider_pem_options_default();
  pem.trusted_ca_pem_path  = getenv("AZ_IOT_TRUSTED_CA");  /* optional */
  pem.client_cert_pem_path = getenv("AZ_IOT_CLIENT_CERT");
  pem.client_key_pem_path  = getenv("AZ_IOT_CLIENT_KEY");

  az_iot_certificate_provider_pem certs;
  if (az_iot_certificate_provider_pem_init(&certs, &pem) != AZ_IOT_OK)
  {
    return 1;
  }

  /* Connection client — DPS runs internally when host == NULL */
  az_iot_connection_client_options copts = az_iot_connection_client_options_default();
  copts.dps.id_scope         = getenv("AZ_IOT_ID_SCOPE");
  copts.dps.registration_id  = getenv("AZ_IOT_REGISTRATION_ID");
  copts.certificate_provider = &certs.base;

  az_iot_connection_client conn;
  az_iot_gen1_telemetry_client gen1_tel = { 0 };
  az_iot_gen2_telemetry_client gen2_tel = { 0 };
  if (az_iot_connection_client_init(&conn, &copts) != AZ_IOT_OK
      /* Register both MQTT versions: v3.1.1 for DPS + Classic, v5 for Next. */
      || az_iot_connection_client_register_mqtt_factory(
             &conn, az_iot_paho_factory_create_v3_1_1())
          != AZ_IOT_OK
      || az_iot_connection_client_register_mqtt_factory(&conn, az_iot_paho_factory_create_v5())
          != AZ_IOT_OK)
  {
    return 1;
  }
  az_iot_connection_client_set_state_callback(&conn, on_conn_state, &ctx);

  if (az_iot_connection_client_open(&conn) != AZ_IOT_OK)
  {
    return 1;
  }

  /* Pump until connected. open() is non-blocking, so sending before this
   * completes would fail with AZ_IOT_ERR_NOT_CONNECTED. Bounded so a hub that
   * never answers cannot spin forever. */
  for (int i = 0; i < 1200 && ctx.conn_state != AZ_IOT_CONN_STATE_CONNECTED; ++i)
  {
    (void)az_iot_connection_client_do_work(&conn, 50);
    if (ctx.conn_state == AZ_IOT_CONN_STATE_FAULTED)
    {
      break;
    }
  }

  int rc = 1;
  if (ctx.conn_state == AZ_IOT_CONN_STATE_CONNECTED && ctx.profile_valid)
  {
    az_iot_result init_result = ctx.connection_profile == AZ_IOT_CONNECTION_PROFILE_MQTT_V5
        ? az_iot_gen2_telemetry_client_init(&gen2_tel, &conn)
        : az_iot_gen1_telemetry_client_init(&gen1_tel, &conn);
    static const uint8_t body[] = "{\"hello\":\"world\"}";
    az_iot_telemetry_message msg = { 0 };
    msg.payload = body;
    msg.payload_len = sizeof(body) - 1;

    az_iot_result send_result = init_result;
    if (init_result == AZ_IOT_OK)
    {
      send_result = ctx.connection_profile == AZ_IOT_CONNECTION_PROFILE_MQTT_V5
        ? az_iot_gen2_telemetry_client_send(&gen2_tel, &msg, on_send_done, &ctx)
        : az_iot_gen1_telemetry_client_send(&gen1_tel, &msg, on_send_done, &ctx);
    }
    if (init_result == AZ_IOT_OK && send_result == AZ_IOT_OK)
    {
      for (int i = 0; i < 600 && !ctx.send_done; ++i)
      {
        (void)az_iot_connection_client_do_work(&conn, 50);
      }
      rc = (ctx.send_done && ctx.send_status == AZ_IOT_OK) ? 0 : 1;
    }
  }

  az_iot_connection_client_close(&conn);
  az_iot_gen1_telemetry_client_destroy(&gen1_tel);
  az_iot_gen2_telemetry_client_destroy(&gen2_tel);
  az_iot_connection_client_destroy(&conn);
  az_iot_certificate_provider_pem_destroy(&certs);
  return rc;
}
```

The full source is in [samples/telemetry_gen1/main.c](samples/telemetry_gen1/main.c). Each sample
is a no-op when its required env vars are unset, so a default build matrix without
cloud resources stays green.

## Samples

| Sample | What it shows |
| --- | --- |
| [telemetry_gen1](samples/telemetry_gen1/) | DPS provisioning + `do_work()` pump + a telemetry send to a Classic hub. The starting point. |
| [telemetry_gen2](samples/telemetry_gen2/) | The same send to an AEG hub over MQTT v5, where properties are user properties rather than topic segments. |
| [twin_get_patch](samples/twin_get_patch/) | `twin_get` + `patch_reported`, and desired-property delivery. |
| [direct_method_responder](samples/direct_method_responder/) | Subscribe for direct methods, echo the payload back via `az_iot_direct_method_respond`. |
| [direct_method_slow_responder](samples/direct_method_slow_responder/) | Answer a direct method after its handler returned, for work that does not fit in a callback. |
| [c2d_receiver_gen1](samples/c2d_receiver_gen1/) | Receive cloud-to-device messages on a Classic hub, where properties are decoded out of the topic. |
| [c2d_receiver_gen2](samples/c2d_receiver_gen2/) | The same on an AEG hub, where the presence handshake already carries the subscription and properties need no decoding. |
| [file_upload](samples/file_upload/) | SAS-URI request, blob PUT via libcurl, completion notification. |
| [authentication](samples/authentication/) | Certificate providers, CSR enrollment, operational certificates. |
| [adu](samples/adu/) | Device Update agent: manifest verify, download, install, report. |
