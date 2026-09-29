# C library coding conventions

Scope: `c/src` (core + feature clients) and `c/inc`. Adapters under `c/adapters`
are the boundary where libc, OpenSSL or a third-party MQTT stack is
unavoidable, and they are held to the naming and error-handling rules but not
to the no-allocation or no-libc-string rules. Tests and samples are application
code.

The short version: **this library is built out of azure-sdk-for-c primitives.
Before writing a hand-rolled C construct, look for the `az_*` or `az_iot_*`
helper, and if one does not exist yet, propose it rather than reaching for the
libc call.**

`.github/instructions/c-library.instructions.md` restates the enforceable parts
for AI-assisted editing, and two scripts make them stick:
`c/eng/check-banned-constructs.sh` (what you may call) and
`c/eng/code-style.sh` (how it is laid out). This document is the reasoning;
those are the teeth.

## 0. Formatting is clang-format's job, not yours

```bash
bash c/eng/code-style.sh fix      # reformat every tracked C source
bash c/eng/code-style.sh check    # what CI runs
```

The style is [`c/.clang-format`](../../.clang-format), adopted from
azure-sdk-for-c so this project reads like the SDK it is built on: LLVM-derived,
Allman braces, 2-space indent, 100 columns, one argument per line. Keep it in
sync when upstream changes.

One deviation, recorded next to the setting: `SortIncludes` stays **off**. LLVM
enables it, and it sorts `<cmocka.h>` above the headers cmocka requires, which
stops every unit test compiling. Include order is load-bearing in C.

**Pin the version.** clang-format's output differs between major releases. CI
uses clang-format 18; Visual Studio 2022 currently bundles 19, so do not rely on
the IDE's copy. Override with `CLANG_FORMAT=clang-format-18`.

The tree was reformatted wholesale in one commit, which is listed in
`.git-blame-ignore-revs` so `git blame` looks through it. Enable that locally
once with `git config blame.ignoreRevsFile .git-blame-ignore-revs`; GitHub
honours it automatically.

## 0.1. clang-tidy must be clean

```bash
cmake -S c -B c/build/clang-tidy -DCMAKE_EXPORT_COMPILE_COMMANDS=ON   # configure only
bash c/eng/clang-tidy.sh c/build/clang-tidy                            # what CI runs
```

Checks and the reason for each exclusion are in [`c/.clang-tidy`](../../.clang-tidy);
every finding is an error in `src`, `adapters` and `samples`. CI pins clang-tidy
18.1.8 (`pipx install clang-tidy==18.1.8`). Ignore a return value with an
explicit `(void)` cast. Suppress a false positive on its line with
`/* NOLINTNEXTLINE(<check>): <reason> */`.

## 1. Build strings with `az_iot_span_writer`, not the C library

`snprintf`, `sprintf`, `vsnprintf`, `vsprintf`, `strcpy`, `strcat`, `strncpy`,
`strncat` and `strtok` are banned in `c/src`. The `v*` variants are on the list
too: they are the same hazard reached through a `va_list`, and the logging
facade - which owns the one legitimate use - carries a waiver for it.

Use [`az_iot_span_writer`](../../src/core/internal/span_writer.h):

```c
char topic[AZ_IOT_MQTTV5_DM_TOPIC_MAX];
const char* parts[] = { "ih/", device_id, "/dev/methods" };
if (az_iot_span_writer_build_str(AZ_SPAN_FROM_BUFFER(topic), NULL, parts, 3) != AZ_IOT_OK)
{
    return AZ_IOT_ERR_INTERNAL;
}
```

or, when the parts are not all strings, the append/`end_str` sequence. Appends
never fail loudly; the first failure is latched and one check at the end
reports it.

Why, concretely:

- A `%s` argument that is NULL is undefined behaviour. The writer reports
  `AZ_IOT_ERR_INVALID_ARG`. Two real NULL-`device_id` paths were fixed by the
  conversion alone.
- Overflow leaves an **empty** string, not a half-built one, so a caller that
  ignores the result cannot publish a truncated topic.
- Truncation is a returned error, not something each call site has to notice.

`memcpy`, `strlen`, `strcmp`, `strncmp` and `strstr` are **not** banned - the
writer itself is built on the first two. They are the crossing point between the
`const char*` MQTT interface and the span world. Prefer `az_span` and
`az_span_find` when the data is already a span; reach for the libc call only at
that boundary.

## 2. Never call `az_span_copy` or `az_span_slice` on unvalidated input

This project builds with `AZ_NO_PRECONDITION_CHECKING=OFF` and installs no
precondition handler, so az_core's default handler **spins forever**. A copy
that does not fit hangs the device instead of returning an error.

- `az_span_copy`, `az_span_copy_u8`, `az_span_slice`, `az_span_u32toa` and the
  `az_json_*` entry points all carry preconditions.
- `az_iot_span_writer` exists partly to avoid them: it touches `az_span` only
  through `az_span_ptr` and `az_span_size`, which have none.
- Turning preconditions *off* is not the fix - it removes the check and leaves
  the unchecked `memcpy`, which is a buffer overflow rather than a hang.

If you must call one of them, bounds-check with `az_span_size()` first.

## 3. Trace through the logging facade, never to a stream

`printf`, `fprintf`, `puts` and `fputs` are banned in `c/src`. Use
`AZ_IOT_LOG_{TRACE,DEBUG,INFO,WARN,ERROR}` for a ready-made message and the
`...F` variants for a formatted one, from
[`az_iot_log.h`](../../inc/azure/iot/az_iot_log.h).

The application chooses where diagnostics go. A library that writes to `stderr`
overrides that choice, cannot be switched off, and - as this SDK did until
recently - can print things like the DPS username and the TLS key path.

Adapters use the same macros; that is why the facade is public.

Log formatting is the one place truncation is preferred over failure, which is
the opposite of `az_iot_span_writer`: a shortened diagnostic still names the
cause, while a shortened topic is a correctness bug.

## 4. No dynamic allocation in `c/src`

`malloc`, `calloc`, `realloc`, `free` and `strdup` are banned. Buffers are
caller-provided (`az_span` or a sized array) or live inside the caller-allocated
client struct.

One documented exception remains, waived in-file: the reference filesystem PEM
loader.

Read environment variables with `az_iot_env_read()` (`internal/env.h`), never
`getenv`: on Windows, a shared build with the static CRT gives each DLL its own
copy of the environment.

## 5. Follow azure-sdk-for-c naming and shapes

- No `_t` type suffixes.
- Every options struct gets `az_iot_<x>_options_default(void)` taking no
  arguments; required fields are set by the caller afterwards.
- Error type is this project's `az_iot_result`, not `az_result`.
- `AZ_NODISCARD` goes on functions where ignoring the result is *likely a bug*
  (init/open/send/parse). Do **not** put it on pump, setter, close or
  best-effort functions: the codebase calls those fire-and-forget, and on gcc a
  `(void)` cast does **not** silence `warn_unused_result`.
- Check `_deps/` for a name collision before adding any `az_iot_*` symbol. This
  has bitten twice.

## When the helper does not exist

That is the interesting case, and the answer is not "use libc this once".

1. Check whether az_core already has it. `az_span_find`,
   `az_span_is_content_equal_ignoring_case`, `az_span_atou32` and the
   `az_json_*` readers/writers cover a lot.
2. Check whether it exists but is az_core-*internal* (`_az_`-prefixed, e.g.
   `_az_span_token`, `_az_span_url_encode`). `c/src` uses **zero** `_az_*`
   symbols and should keep it that way: they are unstable, and the ESP32 sample
   compiles az_core from source so a patched dependency would not reach it.
   Reimplement in our own layer instead.
3. Otherwise **propose adding it** to `c/src/core/` next to `span_writer`, with
   unit tests for its boundary and overflow cases. Say so in the PR description
   so the shape gets reviewed before it has callers.

Adding a small, tested helper is nearly always better than one more hand-rolled
buffer walk. `az_iot_span_writer` itself came out of this exact question.

## Waiving a rule

Add a comment to the file, naming the symbol and the reason:

```c
/* az-iot-allow: fprintf -- the built-in stderr sink itself */
```

The waiver is per file and per symbol. It is deliberately verbose to write,
because each one is a decision a reviewer should see. Run the check locally with:

```bash
bash c/eng/check-banned-constructs.sh
```
