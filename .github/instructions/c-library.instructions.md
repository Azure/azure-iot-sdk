---
applyTo: "c/src/**,c/inc/**,c/adapters/**"
description: "Conventions for the Azure IoT C library. In c/src the CI gate bans libc string building, console I/O and dynamic allocation in favour of az_iot_span_writer, AZ_IOT_LOG_* and caller-provided buffers; c/adapters is exempt from those bans but follows the same naming, formatting and error-handling rules. Propose an azure-sdk-for-c-shaped helper before hand-rolling a libc call."
---

# Azure IoT C library conventions

This library is built out of azure-sdk-for-c primitives. Before writing a
hand-rolled C construct, look for the `az_*` / `az_iot_*` helper. If one does
not exist, **propose adding it** rather than reaching for the libc call.

Full reasoning: `c/docs/eng/coding-conventions.md`. CI enforces the banned list
via `c/eng/check-banned-constructs.sh` and formatting via
`c/eng/code-style.sh check`.

## Which rules apply where

| Area | Banned constructs | Naming, formatting, `az_iot_result`, portability |
|---|---|---|
| `c/src` (core + features) | **enforced by CI** | yes |
| `c/inc` (public headers) | n/a - declarations only | yes |
| `c/adapters` | **exempt** - this is the boundary where libc, OpenSSL and a third-party MQTT stack are unavoidable | yes |

An adapter may allocate and may call libc directly. It still traces through
`AZ_IOT_LOG_*` rather than `stderr`, because the application owns where
diagnostics go - that is why the logging facade is public.

## Formatting

Do not hand-align code. `c/.clang-format` is the style, adopted from
azure-sdk-for-c: LLVM-derived, Allman braces, 2-space indent, 100 columns, one
argument per line. After editing, run:

```bash
bash c/eng/code-style.sh fix
```

CI checks every tracked C source, so leave the tree formatted. Use
clang-format 18 (`CLANG_FORMAT=clang-format-18`); other major versions produce
different output and will fight the gate.

## Banned in `c/src` - CI fails on these

| Do not use | Use instead |
|---|---|
| `snprintf`, `sprintf`, `vsnprintf`, `vsprintf`, `strcpy`, `strcat`, `strncpy`, `strncat`, `strtok` | `az_iot_span_writer` (`src/core/internal/span_writer.h`) |
| `printf`, `fprintf`, `puts`, `fputs` | `AZ_IOT_LOG_*` / `AZ_IOT_LOG_*F` (`inc/azure/iot/az_iot_log.h`) |
| `malloc`, `calloc`, `realloc`, `free`, `strdup` | caller-provided `az_span`/array, or storage inside the caller-allocated client struct |

`memcpy`, `strlen`, `strcmp`, `strncmp`, `strstr` are allowed - the span writer
is built on them - but they belong at the `const char*` boundary. Prefer
`az_span` and `az_span_find` when the data is already a span.

Waive a rule only with a per-file, per-symbol comment naming the reason:

```c
/* az-iot-allow: fprintf -- the built-in stderr sink itself */
```

## Building strings

```c
char topic[AZ_IOT_C2D_TOPIC_MAX];
const char* parts[] = { "ih/", device_id, "/dev/c2d" };
if (az_iot_span_writer_build_str(AZ_SPAN_FROM_BUFFER(topic), NULL, parts, 3) != AZ_IOT_OK)
{
    return AZ_IOT_ERR_INTERNAL;
}
```

Appends never fail loudly: the first failure is latched and one check at the end
reports it. Overflow leaves an empty string, not a half-built one.

## Do not call precondition-bearing az_core functions on unvalidated input

`AZ_NO_PRECONDITION_CHECKING` is OFF and no handler is installed, so az_core's
default handler **spins forever**. `az_span_copy`, `az_span_copy_u8`,
`az_span_slice`, `az_span_u32toa` and `az_json_*` all carry preconditions - an
oversized input hangs the device instead of returning an error. Bounds-check
with `az_span_size()` first, or use `az_iot_span_writer`, which reaches
`az_span` only through `az_span_ptr`/`az_span_size`.

Do **not** "fix" this by disabling preconditions: that turns the hang into a
buffer overflow.

## Style

- No `_t` type suffixes.
- Options structs get `az_iot_<x>_options_default(void)`, no arguments.
- Error type is `az_iot_result`, not `az_result`.
- `AZ_NODISCARD` only where ignoring the result is likely a bug (init/open/send/parse).
  Not on pump, setter, close or best-effort calls - the codebase calls those
  fire-and-forget, and on gcc a `(void)` cast does not silence `warn_unused_result`.
- Check `_deps/` for collisions before adding any `az_iot_*` symbol.

## Portability traps that pass on MSVC and fail on Linux CI

- az_core functions are `warn_unused_result`; a `(void)` cast does not silence
  gcc. Consume the value.
- A forward `typedef struct X X;` plus a full `typedef struct X { } X;` is a C99
  typedef redefinition: `-Wpedantic` error on gcc/clang, silent on MSVC. Make the
  full definition a plain `struct X { };`.
- `/*` inside a block comment (e.g. writing a topic glob) is `-Werror=comment` on
  gcc, silent on MSVC.
- Windows stack memory is garbage where Linux is often zeroed; initialise
  caller-provided storage.

## Tests

Any new helper needs unit tests for its boundary and overflow cases. Behaviour
changes need a test that proves them - especially anything that alters bytes on
the wire.
