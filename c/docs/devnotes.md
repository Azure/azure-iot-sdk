<!-- Copyright (c) Microsoft. All rights reserved.
     Licensed under the MIT license. See LICENSE file in the project root for full license information. -->

# IoT/AEG client API design discussions — dev notes

_Last updated: 5/7/2026_

## DPS integration with IoTHub-Next

- What are the plans for DPS integration with IoTHub-Next? Initially DPS will not be in the picture, but need to confirm.
- **TODO:** ask about it in the Auth design discussion.
- If DPS is present, how will it communicate to the device after provisioning whether the device will connect to an AEG hub vs. a Classic hub?

## Public API

- Common connection client.
- Separate clients for messaging features (e.g., `TwinClient`, `DirectMethodClient`, etc.).

## Protocol logic depending on IoTHub version selection

- Messaging feature contracts are different between Classic and Next.
- DPS provisioning will inform what version of IoT Hub is being provisioned. **TODO:** check on that.
- All MQTT-specific logic that is common between Classic and Next IoT Hubs lives in the **connection client** (CONNECT, CONNACK, DISCONNECT, subscribe, publish).
- MQTT protocol exchange sequence is owned by the **messaging clients**.
- Provisioning and cert management (DPS + IoT Hub) also live in the connection client.
- **TODO:** design reconnection logic.
- **TODO:** confirm if cert management will be mandatory.

## MQTT client selection

### Rust MQTT
- Supports only MQTTv5 today.
- If no support for v3.1.1, it's not of good use for the C# SDK (e.g., Walmart will not want to depend on Rust MQTT if they also have to add another library for v3.1.1).
- Current production library size (with all Rust compilation options) is ~6 MB. Not feasible for embedded devices.

### C#
- One viable MQTT client: **MQTT.net** (handles MQTTv5 and v3.1.1).
- Should use the MQTT.net interface.
- Customers won't be inclined to use anything other than what the .NET Foundation offers; it has no concerns for constrained devices or platform-mandated MQTT client requirements.
- DotNetty (previously supported by the IoT Hub service team) is no longer maintained.

### C
- MQTT interface is required (platform might have specific MQTT libs it supports or allows); also concerns about constrained devices.
- **paho-mqtt-c** as default.
- **az_mqtt** to be added later when possible.

## Overall requirements

- Get a good initial review from Will Brown about this design — had concerns about classic SDKs:
  - Too clunky to make them work.
  - Not all features implemented according to how the hub works (e.g., error codes).
  - APIs looked quite different between clients.
- Client APIs should resemble each other across client languages.
- Reduce the number of packages and dependencies (compared to IoTHub-Classic SDKs).
- Zero to 100% working sample in 3 minutes.

## Progress

### Tim
Started tinkering with what the new C# SDK will look like. References:
- `azure-iot-sdk-net/samples/DirectMethodsSample/Program.cs` @ `timtay/noodling`
- `azure-iot-sdk-net/src/Microsoft.Azure.Devices.Client/ConnectionClient.cs` @ `timtay/noodling`
- `azure-iot-sdk-net/src/Microsoft.Azure.Devices.Client/Twin/TwinClient.cs` @ `timtay/noodling`
- `azure-iot-sdk-net/src/Microsoft.Azure.Devices.Client/DirectMethods/DirectMethodClient.cs` @ `timtay/noodling`

### Ewerton
Discussed MQTT client selection:
- Paho as default — **P0**.
- Add interface for Rust MQTT client — **P0**.
- `az_mqtt` in parallel when possible — **P2**.

## Requirements gathered while bootstrapping `azure-iot-sdk` (P0)

_Captured from conversation as the C99 client repo was being scaffolded. Each bullet is a hard requirement unless tagged otherwise._

### Scope (P0 surface)
- P0 deliverables: ConnectionClient, TwinClient, DirectMethodClient, TelemetryClient, DPSClient, CertManager, plus a runnable smoke sample.
- Both **Classic** and **Next (AEG)** must be supported from day 1 (single repo, single library).
- X.509 authentication is in-scope for P0. SAS / other auth modes are out-of-scope for P0.
- Embedded targets are **design-only** for P0 (no embedded build/CI yet); Linux + Windows are the P0 build/CI targets.

### Public API shape
- Two API surfaces shipped from the same codebase:
  - **API A — core**: single-threaded `do_work()` pump owned by the application; all callbacks fire from inside `do_work()`.
  - **API B — easy**: convenience wrapper built on top of API A with an internal worker thread and `*_sync` helpers.
- Public symbols use the prefix `az_iot_` (types, functions, macros). Header guards use `az_iot_*_H`. Public headers live under `inc/azure/iot/`.
- Sample code must use only `az_iot_*` public symbols — no `azure-sdk-for-c` types/functions leaking into samples.

### MQTT abstraction
- MQTT is pluggable via a vtable (`az_iot_mqtt_iface`).
- Each MQTT adapter instance is **tagged** with `(version, role)`:
  - `version ∈ { v3_1_1, v5 }`
  - `role ∈ { DPS, HUB_CLASSIC, HUB_NEXT }`
- Hard binding rules:
  - DPS → **MQTTv3.1.1 only**.
  - HUB_CLASSIC → **MQTTv3.1.1 only**.
  - HUB_NEXT → **MQTTv5 only**.
- ConnectionClient holds an **adapter registry** keyed by MQTT version and resolves the adapter at connect time. Adapters are registered via `az_iot_connection_client_register_mqtt_factory()`.
- Adapters are **destroyed and recreated** across the DPS → Hub transition ("two-adapter dance") rather than being reused.
- Default adapter shipped: Paho-C. Rust MQTT adapter is a build-time option.

### Mandatory dependencies
- `azure-sdk-for-c` is a **mandatory** dependency, not optional. It is how the client talks to DPS and IoTHub-Classic (uses `az::core`, `az::iot::hub`, `az::iot::provisioning`).
- `azure-sdk-for-c` is integrated via **CMake `FetchContent`**, pinned to a tag (currently `1.5.0`). It is linked **PRIVATE** so it does not leak into the public ABI/include surface.
- No git submodules. Optional deps (Paho, OpenSSL) come from **vcpkg** (manifest mode); other source deps via FetchContent (CPM is a fallback option).

### Paho-C MQTT adapter (default)
- Eclipse `paho.mqtt.c` is integrated the same way as `azure-sdk-for-c`: **FetchContent**, pinned to a tag (currently `v1.3.13`). No vcpkg requirement.
- Uses Paho's **MQTTAsync** API; one library covers both MQTTv3.1.1 and v5 (selected per session via `MQTTAsync_createOptions::MQTTVersion`).
- Two factories shipped: `az_iot_paho_factory_create_v3_1_1()` (DPS + HUB_CLASSIC) and `az_iot_paho_factory_create_v5()` (HUB_NEXT).
- Paho's callbacks fire on its internal threads; the adapter marshals them into a thread-safe FIFO and dispatches them on the caller's thread inside `process_loop()`. This preserves the API A single-thread contract.
- Built static only (`PAHO_BUILD_STATIC=TRUE`, `PAHO_BUILD_SHARED=FALSE`); no DLL artifacts.
- TLS is **not** enabled in the adapter yet (`PAHO_WITH_SSL=OFF`); X.509 plumbing arrives together with `certificate_provider` wiring in a later phase.

### Build, toolchain, CI
- Language: **C99 strict** (`-std=c99 -pedantic`, `CMAKE_C_EXTENSIONS OFF`, warnings-as-errors on by default).
- CMake **≥ 3.21**, driven by `CMakePresets.json`. Presets split into `base` + `linux-base` so Windows is not impacted by Linux-only options (e.g. `CMAKE_EXPORT_COMPILE_COMMANDS` symlinking).
- Linux portability: define `_DEFAULT_SOURCE` / `_POSIX_C_SOURCE=200809L` where needed for `azure-sdk-for-c`'s POSIX platform impl; include upstream headers as `SYSTEM` to avoid `-Werror=strict-prototypes` failures on legacy `func()` declarations.
- CI matrix (GitHub Actions): Linux GCC, Linux Clang, Windows MSVC, plus a dedicated **C99-strict** job.
- CI awareness: must compile cleanly across all `#if` variants before pushing — do not rely on CI to catch ordering / forward-declaration mistakes.

### Testing
- Test framework: **cmocka** (chosen for mocking support and alignment with `azure-sdk-for-c`). Integrated via FetchContent (`cmocka-1.1.7`), forced static (`BUILD_SHARED_LIBS=OFF` in cache with `FORCE`, save/restore around the subdir).
- Tests run via `ctest` from CMake presets on both Windows and Linux.
- An MQTT iface **conformance suite** lives in `tests/conformance/`. It is a reusable cmocka library that exercises any `az_iot_mqtt_factory` end-to-end against a real broker — it does not depend on Paho or any specific adapter. Customers can link `az_iot_conformance` and instantiate their own factory to validate that their MQTT client+adapter is plug-compatible. Two suites: `AZ_IOT_CONFORMANCE_SUITE_V3_1_1` and `AZ_IOT_CONFORMANCE_SUITE_V5`.
- Conformance tests skip themselves (CTest exit 77) unless `AZ_IOT_MQTT_BROKER_HOST` is set, so local builds without a broker stay green.
- CI runs an `eclipse-mosquitto:2` service container on the Linux jobs and points the conformance harnesses at it.

### Repository hygiene / process
- Naming: `aeg` / `AEG` is reserved for internal short-form only; **all public artifacts** (file names, types, macros, functions, CMake options, targets, env-var hints) use `az_iot` / `az_iot`.
- `devnotes.md` is the running requirements log: every time a new requirement is presented, this document must be updated.

### Hub flavor selection
- The IoT Hub flavor (Classic vs Next) is **not** a caller-facing knob. There is no `hub_version` field on any public options struct, and no `az_iot_HUB_*` enum exposed in the public API.
- DPS tells the SDK which hub the device was provisioned to and the SDK selects the appropriate MQTT version internally (v3.1.1 for Classic, v5 for Next).
- The DPS assignment callback exposes `assigned_hub` + `assigned_device_id` only; the hub flavor it learned is consumed internally and not surfaced to the application.

### ConnectionClient lifecycle (Phase 2.1)
- States: `IDLE -> CONNECTING -> CONNECTED -> DISCONNECTING -> IDLE`, plus `RECONNECTING` (Phase 2.2) and `FAULTED` (CONNACK / inbound ERROR).
- Single-threaded contract: every state transition and the user state-callback fires from inside `az_iot_connection_client_do_work()`. `on_mqtt_event()` is called from the adapter's `process_loop()` (which `do_work()` drives), and any state change requiring teardown of the active adapter is *deferred* out of the callback to avoid destroying the adapter while it is still on the call stack.
- Adapter registry validates the MQTT version on registration: each factory must declare a valid `az_iot_mqtt_version`. The SDK internally maps services to required versions (DPS/Classic → v3.1.1, Hub-Next → v5) and selects the matching registered factory at connection time.
- Without DPS, direct-host opens default to `HUB_CLASSIC` (v3.1.1). DPS overrides this via the **internal-only** `az_iot_connection_client__set_session_role()` (header `src/core/internal/connection_client_internal.h`, NOT part of the public ABI) before driving the post-provisioning open.
- Reconnect (backoff + jitter), certificate_provider / X.509 plumbing, and the `protocol_profile` dispatch table for feature clients are deferred to Phase 2.2 / 2.3.

### Reconnect (Phase 2.2)
- Reconnect is **opt-in**: enabled when `opts.reconnect.initial_delay_ms > 0`. Zero-policy means a peer drop or CONNACK failure terminates the session (`IDLE` for clean disconnect, `FAULTED` for failure).
- When enabled:
  - Unexpected `EVT_DISCONNECTED`, failed `EVT_CONNECTED`, and inbound `EVT_ERROR` schedule a reconnect attempt instead of terminating.
  - User-initiated `close()` is honoured regardless: if we're in `RECONNECTING`, the schedule is cancelled and we go straight to `IDLE`.
  - Backoff: `delay = min(max_delay_ms, initial_delay_ms << (attempt - 1))`, then jitter ±`jitter_pct`%. The shift saturates at 30 to avoid UB; `max_delay_ms == 0` is treated as "cap = initial".
  - `max_attempts == 0` means infinite; otherwise the (`max_attempts + 1`)-th unsuccessful attempt-end transitions to `FAULTED`.
- Implementation:
  - Pure-policy delay calculator in `src/core/reconnect.c` (xorshift64 jitter, no I/O); unit-tested in `tests/unit/reconnect_policy_test.c`.
  - Monotonic-time helper `az_iot_time_mono_ms()` (Win32 `GetTickCount64`, POSIX `clock_gettime(CLOCK_MONOTONIC)`); `_POSIX_C_SOURCE=200809L` is set on `az_iot_core` for Linux.
  - `ConnectionClient` records inbound events as a deferred action (FAULT / RECONNECT / IDLE) inside `on_mqtt_event`, then `apply_deferred()` in `do_work()` performs the destructive transition (teardown + state change). This keeps adapter destruction off the inbound-callback stack.
  - The jitter PRNG seed is auto-initialised from the monotonic clock; tests use the internal `az_iot_connection_client__seed_rng()` to make timing deterministic.

### Protocol profile + inbound dispatch (Phase 2.3)
- `src/core/internal/protocol_profile.h` exposes `az_iot_protocol_profile`: hub flavor, MQTT version, topic prefixes (twin response / twin desired / methods request / D2C template), default request/response timeout. The Classic profile (Phase 2.3) is implemented; the Next profile is intentionally NULL until Phase 3 lands its feature-client implementations. DPS sessions reuse the Classic profile so its mqtt_version/timeout defaults are still consultable.
- `src/core/internal/dispatch.h` is a small (`az_iot_MAX_INBOUND_HANDLERS` = 8) topic-prefix → handler registry with longest-prefix-wins routing and `unregister_by_ctx` for clean feature-client teardown. The table is heap-allocated (lazy on first registration) so sessions that never register a handler pay nothing.
- `ConnectionClient` owns one dispatch table and routes every `EVT_MESSAGE` through it (unmatched topics drop silently, matching MQTT-broker behaviour for unsubscribed wildcards). `*_ACK` events stay absorbed pending the Phase 3 correlation table.
- New internal entry points (`internal/connection_client_internal.h`):
  - `__profile()` returns the active profile pointer for the current session role.
  - `__register_inbound_handler()` / `__unregister_inbound_handlers()` — feature clients use these together with the prefixes from `__profile()` to subscribe + route.
- Build hygiene: GCC's quoted-include resolution is strict (relative to the including file's directory). Internal headers refer to siblings as `"internal/foo.h"`, so the core target now exposes `src/core` on its PRIVATE include path. Without this, headers inside `src/core/internal/` couldn't include sibling headers when consumed from a different translation unit.
