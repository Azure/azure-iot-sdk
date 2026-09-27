<!-- Copyright (c) Microsoft. All rights reserved.
     Licensed under the MIT license. See LICENSE file in the project root for full license information. -->

# Struct versioning

How public structs evolve without breaking applications built against an older
header. Helpers: [`az_iot_abi.h`](../inc/azure/iot/az_iot_abi.h).

## Problem

Every public struct is allocated by the application, so its `sizeof` and layout
are fixed when the application is compiled. If a library built from a newer
header copies, clears or reads that struct with its own `sizeof`, it reads or
writes past the application's object. Appending fields does not prevent this;
it only keeps positional initializers compiling.

## ABI profiles

Selected by CMake `AZ_IOT_ABI_PROFILE`; the build exports it to consumers of
`az_iot_core` as `AZ_IOT_ABI_SHARED`. Builds that bypass CMake (e.g. the ESP-IDF
component) get `EMBEDDED`.

| | `EMBEDDED` | `SHARED` |
|---|---|---|
| Typical target | MCU firmware, static link, whole-image OTA | Gateways/Linux, shared library updated independently |
| Default when | `BUILD_SHARED_LIBS=OFF` | `BUILD_SHARED_LIBS=ON` (`EMBEDDED` is then rejected) |
| Client storage reserve | 0 | [Reserves](#reserves) |
| Size macros (`AZ_IOT_MAX_*`, buffer sizes) | Application may override | Fixed by the library build |
| Caller-filled and SDK-stamped structs | Size-stamped | Size-stamped |

Both profiles keep the same source API.

## Rules by struct kind

| Kind | Examples | Rule |
|---|---|---|
| A. Caller fills, SDK reads | options, reconnection policy, telemetry message | `uint32_t _internal_size` first, set by a `*_INIT` macro or inline `*_default()`. The SDK reads only `min(_internal_size, sizeof)` bytes and defaults the rest. `0` (a raw `{0}`), or a stamp smaller than the first release's struct, is rejected. A stamp larger than the library's struct is rejected. |
| B. SDK fills, caller reads | state/SU events, hub profile | The SDK stamps `_internal_size`. Callers check `AZ_IOT_STRUCT_HAS_FIELD(p, T, field)` against the last field they read, never `sizeof(T)`. |
| C. Caller-allocated client state | connection, SU, feature clients, PEM provider | Opaque storage of `sizeof(impl) + <X>_RESERVE`. `init` receives `sizeof(*client)` and a fingerprint. It fails if the storage is smaller than the library needs or the fingerprint differs; larger storage is accepted. |
| D. Adapter interfaces | `az_iot_mqtt_iface`, connect/TLS/proxy options, certificate provider vtable, SU hooks | The writer stamps `_internal_size`. A function-table slot beyond the stamp means "not supported". A new field must mean "not requested" when zero. |
| E. Array elements and nested value types | properties, dispatch entries, nested option sub-structs | Frozen. New members go at the end of the enclosing struct, or the parent carries the element size. |

Adding a field: append it, make zero mean "previous behaviour", and read it only
behind `AZ_IOT_STRUCT_HAS_FIELD`. Never reorder, resize or remove a member.

## Fingerprint

`AZ_IOT_ABI_FINGERPRINT` combines `AZ_IOT_ABI_VERSION`, the profile and the
pointer size. Kind C clients extend it with the size macros that shape their
storage, so a library built with different macro values is rejected at `init`
instead of being misread. `az_iot_abi_fingerprint()` returns the library's value.

## Reserves

`SHARED` only (bytes, per instance):

| Client | Reserve |
|---|---|
| connection | 768 |
| software updates | 1792 |
| mqttv5 direct method | 512 |
| mqttv3 direct method | 192 |
| mqttv5 twin | 256 |
| mqttv3 twin | 128 |
| mqttv3 file upload | 128 |
| mqttv3/mqttv5 telemetry, mqttv3 C2D, PEM provider | 64 each |

Within one `SOVERSION`, a client's internal state may grow by at most its
reserve over the first release of that `SOVERSION`. Exhausting it requires an
`AZ_IOT_ABI_VERSION` and `SOVERSION` bump.

Sizing: the observed growth of each client from 2026-06 to 2026-09 at the
routine-change rate, with larger redesigns deliberately excluded (they bump the
ABI). These values are provisional. Re-baseline them from the final struct
sizes right before the first GA release (mqttv3, DPS and software updates) and
the first mqttv5 public preview.

## Status

- Done: profiles, fingerprint, reserve values, `AZ_IOT_STRUCT_HAS_FIELD`.
- Stamped today: hub profile, connection error detail, connection state event,
  SU event.
- Pending: remaining kind B structs; kind A, C and D conversion; shared-library
  export, `SOVERSION` and ABI checks in CI.
