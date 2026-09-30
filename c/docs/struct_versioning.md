<!-- Copyright (c) Microsoft. All rights reserved.
     Licensed under the MIT license. See LICENSE file in the project root for full license information. -->

# Struct versioning

## Policy

The SDK guarantees **source compatibility only**. There is no ABI guarantee
between releases.

Application and SDK each compile the layout of every public struct from the
headers they were built with, whichever side allocates it (e.g. client and
options structs by the application, state events by the SDK). A release may add
fields to any public struct, which changes its size and layout. After any SDK update, recompile every
application object against the new headers; relinking objects built against
older headers is not supported.

| Linking | Effect |
|---|---|
| Static (default; embedded devices) | A deployed binary keeps the SDK it was linked with until it is rebuilt. |
| Shared (`-DBUILD_SHARED_LIBS=ON`) | A replaced library is picked up at load time, so the application must be rebuilt with it. |

## Shared libraries

- `az_iot_core`, `az_iot_mqttv3` and `az_iot_mqttv5` build as shared libraries.
  Adapters are always static.
- azure-sdk-for-c is always static (position-independent in a shared build) and
  a public dependency of `az_iot_core`, since its types are in our headers. On
  Linux, `az_iot_mqttv3`/`az_iot_mqttv5` resolve it from `az_iot_core`; on
  Windows, each DLL or executable calling it links its own copy. Copies are
  independent: the SDK sets no azure-sdk-for-c global state (log or precondition
  callbacks).
- The full release version, pre-release label included, is in the soname
  (`libaz_iot_core.so.1.0.0-preview`) and the Windows DLL name
  (`az_iot_core-1.0.0-preview.dll`). An application built against one release
  fails to load with any other, instead of misreading struct layouts.
- Windows: each DLL links the static CRT (`/MT`), so heap, `FILE*` and
  environment are per module. The SDK API does not pass them across.

## Adding a field

- Append it and make zero mean the previous behaviour, so existing code that
  zero-initializes the struct keeps its behaviour once recompiled.
- Adapter-facing structs (`az_iot_mqtt_iface.h`) follow the same rule, so a
  bring-your-own adapter keeps working once recompiled.
