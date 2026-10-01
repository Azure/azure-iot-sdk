<!-- Copyright (c) Microsoft. All rights reserved.
     Licensed under the MIT license. See LICENSE file in the project root for full license information. -->

# Building, installing and verifying the C SDK

Build options, installation, the hardening applied to every build, and the MQTT adapter
conformance suite. For a first build, see the [samples overview](../../samples/README.md#build).

## Source layout

```
inc/azure/iot/      public headers
src/                core, mqttv3 and mqttv5 feature clients, software updates
adapters/           MQTT adapters (Paho, Rust shell), OpenSSL certificate provider,
                    software updates crypto and platform adapters
samples/            samples, one README each
tests/              unit, conformance, end-to-end and install tests
platforms/yocto/    Yocto layer
docs/               customer documentation; docs/eng for engineering documentation
cmake/, eng/        build helpers and CI scripts
```

## Install and consume

Building `c/` as the top-level CMake project installs static libraries, headers and the
`azure-iot-sdk` CMake package (`-DAZ_IOT_INSTALL=OFF` disables it). From `c/`:

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
  whenever the library is updated ([struct_versioning.md](../../docs/struct_versioning.md)).

pkg-config (not MSVC): one `azure-iot-sdk-<component>.pc` per component.
Adapters are always static, so pass `--static` when using one; OpenSSL's and
mbedTLS's `.pc` files must be on `PKG_CONFIG_PATH`. With shared libraries
outside the loader's search path, set `LD_LIBRARY_PATH` or an rpath at run time.

```sh
cc app.c $(pkg-config --static --cflags --libs azure-iot-sdk-mqttv3 azure-iot-sdk-adapter_paho)
```

Yocto: [platforms/yocto/meta-azure-iot-sdk](../../platforms/yocto/meta-azure-iot-sdk/README.md) (scarthgap).

[tests/install](../../tests/install/CMakeLists.txt) is a consumer that CI builds against the installed package,
with CMake and with pkg-config alone ([pkg-config-test.sh](../../tests/install/pkg-config-test.sh)).
[tests/subproject](../../tests/subproject/CMakeLists.txt) builds the same sources with this tree added via `add_subdirectory()`.

## Build hardening

Builds are hardened by default (`AZ_IOT_ENABLE_HARDENING=ON`); see
[cmake/az_iot_hardening.cmake](../../cmake/az_iot_hardening.cmake).

GCC and Clang:

| Flag | Effect | When |
|---|---|---|
| `-fstack-protector-strong` | Stack canaries | Always |
| `-fstack-clash-protection` | Stack clash probing | Always |
| `-fcf-protection` | Control-flow enforcement (CET) | x86 only |
| `-Wformat -Werror=format-security` | Non-literal format strings are errors | Always |
| `-D_FORTIFY_SOURCE=2` | Checked libc calls | `Release`, `RelWithDebInfo`, `MinSizeRel` |
| PIE, `-z relro -z now` | Position-independent code, read-only relocations | Always |
| `-z noexecstack` | Non-executable stack | Always |

MSVC (`/GS`, `/DYNAMICBASE`, `/NXCOMPAT` and `/HIGHENTROPYVA` are already on by default):

| Flag | Effect | When |
|---|---|---|
| `/guard:cf` | Control Flow Guard | Always |
| `/CETCOMPAT` | CET shadow stack compatible | x64 only |
| `/sdl` | Security-relevant warnings are errors | First-party targets only |

Each flag is probed first and skipped if the toolchain does not support it.

**Scope.** The flags apply to the SDK, its samples and tests, and the dependencies it
fetches (azure-sdk-for-c, Paho), except `/sdl`, which the dependencies do not build
cleanly with. They are not exported: a parent project that adds this
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
and `/sdl` belong to hardening. Package builds that inject their own hardening flags (e.g. Debian,
Yocto) can set `AZ_IOT_ENABLE_HARDENING=OFF` to avoid duplicates.

**Verifying.** `eng/check-hardening.sh <build-dir>` fails any ELF executable or shared
library without PIE (executables), RELRO, `BIND_NOW` or a non-executable stack. The stack
protector check is weaker: it covers first-party executables only (not shared libraries),
reports each one without a canary reference, and fails only if none has one; a binary with
no function that needs a canary legitimately has none. On Windows (from a Visual Studio
developer shell, in Git Bash) it uses `dumpbin` and fails any `.exe`/`.dll` without Control
Flow Guard, ASLR or DEP, and, on x64, high entropy VA or CET compatibility. CI runs it on
gcc and clang, static and shared (`hardening-linux` in `ci-c.yml`), and on MSVC, static
and shared (`hardening-windows`).

## MQTT adapter conformance suite

[`tests/conformance/`](../../tests/conformance/) is a reusable test suite that drives an MQTT
adapter only through the `az_iot_mqtt_iface` interface. `az_iot_conformance_paho_v3` and
`az_iot_conformance_paho_v5` run it against the bundled Paho adapter. To run it against your own
adapter, see [Bring your own MQTT client](../how_to_byo_mqtt_client.md).

It needs a reachable MQTT broker:

```sh
export AZ_IOT_MQTT_BROKER_HOST=localhost
export AZ_IOT_MQTT_BROKER_PORT=1883
ctest --preset linux-gcc-debug --output-on-failure -R conformance
```

The tests are registered only with `-DAZ_IOT_BUILD_CONFORMANCE_TESTS=ON`, which the Linux presets
set. Once registered, an unset `AZ_IOT_MQTT_BROKER_HOST` is a failure, not a skip. CI runs an
`eclipse-mosquitto:2` broker.
