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

## Sizes change between releases

Struct sizes, buffer sizes and capacity macros may change in any release, minor and patch
included. Size application memory with the SDK's macros and functions (for example
`AZ_IOT_SAS_BUFFER_SIZE()`, `AZ_IOT_SU_WORKSPACE_SIZE()`), never with numbers copied from a build,
a log or a document, and recompile after every SDK update.

What catches a mismatch:

| Situation | Check | Result |
|---|---|---|
| Static build | Sizes and macros are re-evaluated when the application is recompiled | Always consistent |
| Shared build | Full release version in the library name (below) | The application fails to load against any other release |
| Capacity macros defined differently for the application and the SDK, static or shared | `az_iot_connection_client_init()` compares the application's `az_iot_connection_client_layout` with the SDK's | `AZ_IOT_ERR_INVALID_ARG`, client not written, `ERROR` log naming the value |

## Shared libraries

- `az_iot_core`, `az_iot_mqttv3` and `az_iot_mqttv5` build as shared libraries.
  Adapters are always static.
- azure-sdk-for-c is always static (position-independent in a shared build) and
  a public dependency of `az_iot_core`, since its types are in our headers. On
  Linux, `az_iot_mqttv3`/`az_iot_mqttv5` resolve it from `az_iot_core`; on
  Windows, each DLL or executable calling it links its own copy. Copies are
  independent: the SDK sets no azure-sdk-for-c global state (log or precondition
  callbacks).
- The full release version (`AZ_IOT_VERSION_STRING`, pre-release label
  included) is in the soname (`libaz_iot_core.so.<version>`) and the Windows
  DLL name (`az_iot_core-<version>.dll`). An application built against one
  release fails to load with any other, instead of misreading struct layouts.
- If you repackage the libraries, keep the versioned names. Without them the loader no longer
  refuses another release: check at startup that the loaded library matches the headers the
  application was built with, and refuse to run otherwise:

  ```c
  #include <azure/iot/az_iot_version.h>

  /* AZ_IOT_VERSION_STRING: headers the application was built with.
   * az_iot_version_string(): library loaded. */
  if (strcmp(az_iot_version_string(), AZ_IOT_VERSION_STRING) != 0)
  {
    /* refuse to start */
  }
  ```
- Windows: each DLL links the static CRT (`/MT`), so heap, `FILE*` and
  environment are per module. The SDK API does not pass them across.

## Adding a field

- Append it and make zero mean the previous behaviour, so existing code that
  zero-initializes the struct keeps its behaviour once recompiled.
- Adapter-facing structs (`az_iot_mqtt_iface.h`) follow the same rule, so a
  bring-your-own adapter keeps working once recompiled.
