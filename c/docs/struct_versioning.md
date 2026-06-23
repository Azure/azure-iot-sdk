<!-- Copyright (c) Microsoft. All rights reserved.
     Licensed under the MIT license. See LICENSE file in the project root for full license information. -->

# Struct Versioning: ABI Compatibility Across Library Versions

## The Problem

`az_iot_telemetry_message_t` is **stack-allocated by the user** (e.g. `az_iot_telemetry_message_t msg = {0};`). Its layout is baked into the user's compiled `.o` at compile time.

### What breaks when v2 appends `some_new_prop`:

1. **`sizeof` mismatch** — The user app was compiled against v1 headers, so `sizeof(az_iot_telemetry_message_t)` is smaller than what the v2 library expects. The library reads past the end of the user's struct instance → **undefined behavior** (reads garbage or adjacent stack data for `some_new_prop`).

2. **`{0}` doesn't zero the new field from the library's perspective** — Even if sizes happened to align (padding luck), the user never wrote to `some_new_prop`, so the library sees uninitialized memory it interprets as a valid value.

3. **`memcpy`/assignment of the struct** inside the library copies the wrong number of bytes if the lib uses its own `sizeof`.

4. **Arrays of the struct** have completely wrong stride; element N is at the wrong offset.

---

## Bullet-proof mitigations

| Technique | How it works |
|-----------|-------------|
| **Opaque allocation + init function** | Don't let the user declare the struct on the stack. Provide `az_iot_telemetry_message_create()`/`_init()` that returns a lib-allocated (or lib-sized) struct. User only holds a pointer. The library owns `sizeof`. |
| **Versioned options pattern** | Add a `uint32_t _reserved` or `uint32_t version` as the first field. The init function stamps the struct size or version tag. The library checks this before reading any field added after v1. New fields default to safe values when version < current. |
| **Builder API (no user-visible struct)** | Replace the struct with a builder handle:<br>`az_iot_telemetry_message_builder_t* b;`<br>`msg_builder_set_payload(b, ...);`<br>`msg_builder_set_content_type(b, ...);`<br>New fields are simply new setters — old apps never call them, defaults apply. |
| **Embed struct size at call site** | Macro wraps the send call to pass `sizeof(msg)` as a hidden parameter. Library reads only up to that many bytes and defaults the rest:<br>`#define az_iot_telemetry_client_send(tc, msg, cb, ctx) \`<br>`  _az_iot_telemetry_client_send_v(tc, msg, sizeof(*(msg)), cb, ctx)` |
| **Static assert on ABI version** | Ship a compile-time constant `az_iot_ABI_VERSION` in the header. The library exports a symbol with the same name. A static-assert or link-time check (`_ABI_V2` symbol) ensures header ↔ .a agreement. Catches mismatch at build time rather than runtime. |
| **Never extend; deprecate and replace** | Freeze `az_iot_telemetry_message_t` forever. If v2 needs more fields, introduce `az_iot_telemetry_message2_t` and a new `_send2()` entry point. Old apps keep working against the old struct/API. |

---

## Recommended combination for this codebase

Since the struct is currently passed as `const*` into `_send()`, the cheapest safe evolution path is:

1. Add a **`uint32_t _internal_size;`** field at the top of the struct now (v1).
2. Provide an **initializer macro** (replaces raw `= {0}`):
   ```c
   #define az_iot_TELEMETRY_MESSAGE_INIT \
       { ._internal_size = sizeof(az_iot_telemetry_message_t) }
   ```
3. Inside `_send()`, compare `msg->_internal_size` against the library's own `sizeof`. If smaller → the user compiled against an older header → ignore/default any fields beyond that size.

This is the pattern used by Win32 (`cbSize`), Vulkan (`sType`/`pNext`), and the Azure SDK for C (`_internal` fields). It requires no heap allocation, no builder ceremony, and catches the mismatch at runtime with a clean error rather than UB.
