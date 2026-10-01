<!-- Copyright (c) Microsoft. All rights reserved.
     Licensed under the MIT license. See LICENSE file in the project root for full license information. -->

# SAS signer template

> **Proposed API.** `az_iot_sas_signer` is not consumed by the connection client yet.

A minimal skeleton of an `az_iot_sas_signer`, for a symmetric key kept in a TPM, HSM or secure
element. It needs no crypto library and does not connect: `main()` calls the signer so the wiring
compiles and runs.

## Sample features

- `my_sign()`: replace with HMAC-SHA256 over the given bytes, computed by your key store.
- Set the signer as `sas.onboarding` (DPS) and/or `sas.operational` (hub) in `az_iot_connection_client_options`.
- Platforms: Linux and Windows.

## Service requirements

None. The sample does not connect.

## Build and run

Build prerequisites (toolchain, CMake, OpenSSL) are listed in the
[samples overview](../../README.md#prerequisites). Run from the repository's `c/` directory.

Linux:

```sh
cmake --preset linux-gcc-debug
cmake --build --preset linux-gcc-debug --target az_iot_sample_auth_custom_sas_signer_template
./build/linux-gcc-debug/samples/authentication/az_iot_sample_auth_custom_sas_signer_template
```

Windows (*Developer PowerShell for VS 2022*):

```powershell
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug --target az_iot_sample_auth_custom_sas_signer_template
.\build\windows-msvc-debug\samples\authentication\Debug\az_iot_sample_auth_custom_sas_signer_template.exe
```

Always built; no external dependency.

## How it ends

Exit code 0.

## Expected output

```
[sas_signer] sign: AZ_IOT_OK, 32 bytes
```

## Where to look in `main.c`

| What | Code |
| --- | --- |
| Signer state (the base must be first) | `my_signer` |
| The interface to fill in | `my_sign()`, `my_deinit()` |

Interface reference: [az_iot_sas_signer.h](../../../inc/azure/iot/az_iot_sas_signer.h).
