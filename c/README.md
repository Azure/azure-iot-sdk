<!-- Copyright (c) Microsoft. All rights reserved.
     Licensed under the MIT license. See LICENSE file in the project root for full license information. -->

# azure-iot-sdk

C99 client SDK for the Azure MQTTv3 hub and the MQTTv5 hub.

> Status: **early bootstrap**. See [docs/design.md](docs/design.md) for the architecture and [docs/devnotes.md](docs/devnotes.md) for original design discussion notes.

## Highlights

- **C99-strict**, no submodules. Dependencies via vcpkg manifest (primary) or CPM.cmake (fallback).
- **Single public API**: low-level, single-threaded `do_work()` pump; all callbacks fire on the caller's thread.
- **Pluggable MQTT** with version + role tagging:
  - DPS + MQTTv3 hub require **MQTT v3.1.1**.
  - MQTTv5 hub requires **MQTT v5**.
  - Adapters register factories via `az_iot_connection_client_register_mqtt_factory()`. The core picks the right `(version, role)` per session and instantiates a fresh adapter for each.
- **Default adapter**: Paho-C (v3.1.1 + v5).
- **Built on [azure-sdk-for-c](https://github.com/Azure/azure-sdk-for-c)** (pinned via `FetchContent`):
  - `az::core` for spans, JSON, logging, contexts, result codes.
  - `az::iot::hub` for MQTTv3 hub MQTT topic build/parse.
  - `az::iot::provisioning` for Azure DPS MQTT topic build/parse.
  - MQTTv5 hub protocol logic lives in this repo (no upstream library yet).

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
./build/linux-gcc-debug/samples/unified/az_iot_sample_telemetry
```

### Build hardening

GCC and Clang builds are hardened by default (`AZ_IOT_ENABLE_HARDENING=ON`); see
[cmake/az_iot_hardening.cmake](cmake/az_iot_hardening.cmake). MSVC builds are not
affected.

| Flag | Effect | When |
|---|---|---|
| `-fstack-protector-strong` | Stack canaries | Always |
| `-fstack-clash-protection` | Stack clash probing | Always |
| `-fcf-protection` | Control-flow enforcement (CET) | x86 only |
| `-Wformat -Werror=format-security` | Non-literal format strings are errors | Always |
| `-D_FORTIFY_SOURCE=2` | Checked libc calls | `Release`, `RelWithDebInfo`, `MinSizeRel` |
| PIE, `-z relro -z now` | Position-independent code, read-only relocations | Always |
| `-z noexecstack` | Non-executable stack | Always |

Each flag is probed first and skipped if the toolchain does not support it.

**Scope.** The flags apply to the SDK, its samples and tests, and the dependencies it
fetches (azure-sdk-for-c, Paho). They are not exported: a parent project that adds this
tree with `add_subdirectory()`/`FetchContent`, or consumes the installed package, keeps
its own flags.

**Turning it off.** It is all or nothing; individual flags cannot be removed through
`CMAKE_C_FLAGS`, because the SDK's flags come later on the command line.

- Command line: `cmake --preset linux-gcc-debug -DAZ_IOT_ENABLE_HARDENING=OFF`
- Parent project, before adding the SDK:
  ```cmake
  set(AZ_IOT_ENABLE_HARDENING OFF)
  add_subdirectory(azure-iot-sdk/c)
  ```
- Personal preset, in an untracked `CMakeUserPresets.json` next to `CMakePresets.json`:
  ```json
  {
    "version": 6,
    "configurePresets": [
      {
        "name": "linux-gcc-debug-relaxed",
        "inherits": "linux-gcc-debug",
        "binaryDir": "${sourceDir}/build/linux-gcc-debug-relaxed",
        "cacheVariables": {
          "AZ_IOT_ENABLE_HARDENING": "OFF",
          "AZ_IOT_WARNINGS_AS_ERRORS": "OFF"
        }
      }
    ]
  }
  ```

`AZ_IOT_WARNINGS_AS_ERRORS=OFF` drops `-Werror`/`/WX` only; `-Werror=format-security`
belongs to hardening. Package builds that inject their own hardening flags (e.g. Debian,
Yocto) can set `AZ_IOT_ENABLE_HARDENING=OFF` to avoid duplicates.

**Verifying.** `eng/check-hardening.sh <build-dir>` fails any ELF executable or shared
library without PIE (executables), RELRO, `BIND_NOW` or a non-executable stack. The stack
protector check is weaker: it covers first-party executables only (not shared libraries),
reports each one without a canary reference, and fails only if none has one; a binary with
no function that needs a canary legitimately has none. CI runs it on gcc and clang, static
and shared (`hardening-linux` in `ci-c.yml`).

## Install and consume

A top-level build installs static libraries, headers and the `azure-iot-sdk`
CMake package (`-DAZ_IOT_INSTALL=OFF` disables it):

```sh
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=<prefix>
cmake --build build/release
cmake --install build/release
```

```cmake
find_package(azure-iot-sdk CONFIG REQUIRED COMPONENTS mqttv3 adapter_paho)
target_link_libraries(app PRIVATE azure::iot::mqttv3 azure::iot::adapter_paho)
```

- Components: `core`, `mqttv3`, `mqttv5`, and each adapter built (`adapter_paho`,
  `adapter_rust_mqtt`, `su_crypto_openssl`, `su_crypto_mbedtls`,
  `certificate_provider_managed`).
- azure-sdk-for-c ships in the package (headers under `include/azure-sdk-for-c`);
  Paho installs its own `eclipse-paho-mqtt-c` package into the same prefix.
- OpenSSL, and mbedTLS for `su_crypto_mbedtls`, must be findable by the consumer.
- MSVC: the libraries use the static CRT; set
  `CMAKE_MSVC_RUNTIME_LIBRARY` to `MultiThreaded$<$<CONFIG:Debug>:Debug>`.
- `-DBUILD_SHARED_LIBS=ON` builds `core`, `mqttv3` and `mqttv5` as shared
  libraries. There is no ABI guarantee between releases: rebuild the application
  whenever the library is updated ([struct_versioning.md](docs/struct_versioning.md)).

pkg-config (not MSVC): one `azure-iot-sdk-<component>.pc` per component.
Adapters are always static, so pass `--static` when using one; OpenSSL's and
mbedTLS's `.pc` files must be on `PKG_CONFIG_PATH`. With shared libraries
outside the loader's search path, set `LD_LIBRARY_PATH` or an rpath at run time.

```sh
cc app.c $(pkg-config --static --cflags --libs azure-iot-sdk-mqttv3 azure-iot-sdk-adapter_paho)
```

Yocto: [platforms/yocto/meta-azure-iot-sdk](platforms/yocto/meta-azure-iot-sdk/README.md) (scarthgap).

[tests/install](tests/install/CMakeLists.txt) is a consumer that CI builds against the installed package,
with CMake and with pkg-config alone ([pkg-config-test.sh](tests/install/pkg-config-test.sh)).

## Project layout

```
inc/azure/iot/        public headers
src/{core,features}/ implementation
adapters/{paho,rust_mqtt}/ MQTT adapters
samples/                  examples (unified/, mqttv5/, authentication/, su/)
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

  /* Connection client — DPS runs internally */
  az_iot_connection_client_options copts = az_iot_connection_client_options_default();
  copts.dps.id_scope         = getenv("AZ_IOT_ID_SCOPE");
  copts.dps.registration_id  = getenv("AZ_IOT_REGISTRATION_ID");
  copts.certificate_provider = &certs.base;

  az_iot_connection_client conn;
  az_iot_mqttv3_telemetry_client mqttv3_tel = { 0 };
  az_iot_mqttv5_telemetry_client mqttv5_tel = { 0 };
  if (az_iot_connection_client_init(&conn, &copts) != AZ_IOT_OK
      /* Register both MQTT versions: v3.1.1 for DPS + MQTTv3, v5 for MQTTv5. */
      || az_iot_connection_client_register_mqtt_factory(
             &conn, az_iot_paho_factory_create_v3_1_1())
          != AZ_IOT_OK
      || az_iot_connection_client_register_mqtt_factory(&conn, az_iot_paho_factory_create_v5())
          != AZ_IOT_OK)
  {
    return 1;
  }
  az_iot_connection_client_add_state_observer(&conn, on_conn_state, &ctx);

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
        ? az_iot_mqttv5_telemetry_client_init(&mqttv5_tel, &conn)
        : az_iot_mqttv3_telemetry_client_init(&mqttv3_tel, &conn);
    static const uint8_t body[] = "{\"hello\":\"world\"}";
    az_iot_telemetry_message msg = { 0 };
    msg.payload = body;
    msg.payload_len = sizeof(body) - 1;

    az_iot_result send_result = init_result;
    if (init_result == AZ_IOT_OK)
    {
      send_result = ctx.connection_profile == AZ_IOT_CONNECTION_PROFILE_MQTT_V5
        ? az_iot_mqttv5_telemetry_client_send(&mqttv5_tel, &msg, on_send_done, &ctx)
        : az_iot_mqttv3_telemetry_client_send(&mqttv3_tel, &msg, on_send_done, &ctx);
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
  az_iot_mqttv3_telemetry_client_deinit(&mqttv3_tel);
  az_iot_mqttv5_telemetry_client_deinit(&mqttv5_tel);
  az_iot_connection_client_deinit(&conn);
  az_iot_certificate_provider_pem_deinit(&certs);
  return rc;
}
```

A fuller version is [samples/unified/connect_first/main.c](samples/unified/connect_first/main.c); [samples/unified/telemetry/main.c](samples/unified/telemetry/main.c) builds before `open()` and also handles a device moved to the other hub generation.

## Samples

Samples are grouped like the .NET SDK's: [samples/unified](samples/unified/) serve
whichever hub generation DPS assigns, [samples/mqttv5](samples/mqttv5/) serve MQTTv5 hubs
only. There is no MQTTv3-only group; the MQTTv3-only samples
(`unified/file_upload`, `authentication/dps_csr_managed`,
`authentication/hub_renew`) exit non-zero on an MQTT v5 hub. Layout,
configuration and the full list are in [samples/README.md](samples/README.md).
