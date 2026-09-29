<!-- Copyright (c) Microsoft. All rights reserved.
     Licensed under the MIT license. See LICENSE file in the project root for full license information. -->

# Struct versioning

## Policy

The SDK guarantees **source compatibility only**. There is no ABI guarantee
between releases.

Public structs are allocated by the application, so their size and layout are
fixed when the application is compiled. A release may add fields to any public
struct, which changes its size. An application must be rebuilt against the
headers of the release it runs with.

| Linking | Effect |
|---|---|
| Static (default; embedded devices) | Application and library are always built together. Nothing to manage. |
| Shared (`-DBUILD_SHARED_LIBS=ON`) | Updating the library requires rebuilding the application. |

## Shared libraries

- `az_iot_core`, `az_iot_mqttv3` and `az_iot_mqttv5` build as shared libraries.
  Adapters are always static.
- azure-sdk-for-c is always static (position-independent in a shared build) and
  is linked into each shared library.
- The full release version is in the soname (`libaz_iot_core.so.0.0.1`) and the
  Windows DLL name (`az_iot_core-0.0.1.dll`). An application built against one
  release fails to load with any other, instead of misreading struct layouts.

## Adding a field

- Append it and make zero mean the previous behaviour, so existing code that
  zero-initializes the struct keeps its behaviour once rebuilt.
- Adapter-facing structs (`az_iot_mqtt_iface.h`) follow the same rule, so a
  bring-your-own adapter keeps working once rebuilt.

## Possible future guarantee

If needed, ABI stability can be added per patch series (e.g. `1.2.x`), as mbedTLS
does: soname per minor version, and an ABI check (`abidiff`) in CI rejecting
layout changes within a series.
