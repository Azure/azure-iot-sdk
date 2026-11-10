# Fuzzing

The fuzz targets feed generated input to the code that parses what the hub and DPS send to a
device. They run two ways:

- **In CTest**, in every build with `AZ_IOT_BUILD_TESTS=ON`: each target replays its seed corpus
  (`<target>_corpus` tests), under whatever compiler and sanitizer the build uses.
- **Under libFuzzer**, with `AZ_IOT_BUILD_FUZZERS=ON`: continuous fuzzing, for local runs and the
  scheduled fuzzing service.

## Targets

Sources are in [tests/fuzz](../../tests/fuzz).

| Target | What it fuzzes | Input |
| --- | --- | --- |
| `fuzz_parsers` | JSON string decoding, base64/base64url decoding, the certificate helpers (base64 check, JSON completeness, certificate chain collection) | byte 0 selects the parser; the rest is its input |
| `fuzz_connection_inbound` | the connection client with the twin, C2D, direct method and file upload clients, over the in-memory mock MQTT adapter: inbound messages on every SDK topic, SUBACKs and refusals, disconnects and reconnects, DPS provisioning, the file upload HTTP response | a script of service events; layout in the source's header comment |

Both targets make an azure-sdk-for-c precondition failure abort, so it is reported as a crash
instead of a hang.

## Build and run locally

libFuzzer needs clang (or MSVC on Windows). With clang:

```sh
CC=clang cmake -S c -B build/fuzz -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DAZ_IOT_BUILD_TESTS=ON -DAZ_IOT_BUILD_FUZZERS=ON \
  -DAZ_IOT_WITH_PAHO=OFF -DAZ_IOT_BUILD_SAMPLES=OFF -DAZ_IOT_ENABLE_HARDENING=OFF \
  "-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
cmake --build build/fuzz --target fuzz_parsers fuzz_connection_inbound

mkdir -p /tmp/corpus && cp c/tests/fuzz/corpus/fuzz_connection_inbound/* /tmp/corpus/
build/fuzz/tests/fuzz/fuzz_connection_inbound -max_total_time=300 \
  -dict=c/tests/fuzz/fuzz_connection_inbound.dict /tmp/corpus
```

`AZ_IOT_BUILD_FUZZERS` instruments every target for coverage, so build only the fuzz targets.
Hardening is off because `_FORTIFY_SOURCE` hides overflows from AddressSanitizer.

## Fixing a finding

1. Reproduce it: run the target with the crash input as its only argument.
2. Fix the SDK.
3. Add the input to `tests/fuzz/corpus/<target>/` with a descriptive name, so CTest replays it
   from then on.

## Adding a target

Add `tests/fuzz/<target>.c` defining `LLVMFuzzerTestOneInput()`, list it in
[tests/fuzz/CMakeLists.txt](../../tests/fuzz/CMakeLists.txt), and add seed inputs to
`tests/fuzz/corpus/<target>/`. The CTest replay and the libFuzzer build pick it up from the list.
