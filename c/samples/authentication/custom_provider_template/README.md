<!-- Copyright (c) Microsoft. All rights reserved.
     Licensed under the MIT license. See LICENSE file in the project root for full license information. -->

# Certificate provider template

A minimal skeleton of an `az_iot_certificate_provider`, for a TPM, HSM, secure
element or OS keystore that the shipped providers do not cover. It needs no crypto library and
does not connect: `main()` calls the provider interface so the wiring compiles and runs.

## Sample features

- Implements every slot of the provider interface with placeholder bodies to replace:
-   - `load()`: return the credential for the requested role (bootstrap or operational) and
      index: 0 for the first certificate, `AZ_IOT_ERR_NOT_FOUND` past the last one.
-   - `get_csr()`: produce a PKCS#10 CSR (base64 DER) over the device key.
-   - `store_issued_certificate()`: persist the issued chain.
-   - `sign()`: sign with a non-extractable key. When you implement it, leave the PEM key fields empty in `load()`; see [`hsm_sign_callback`](../hsm_sign_callback/README.md) and [`hsm_pkcs11`](../hsm_pkcs11/README.md) for which route your adapter supports.
- For a provider that builds a real CSR, see [`custom_certificate_provider`](../custom_certificate_provider/README.md).
- Platforms: Linux and Windows.

## Service requirements

None. The sample does not connect.

## Configure

Nothing to configure: the sample reads no environment variables. The paths in `main()`
(`bootstrap_cert.pem`, `bootstrap_key.pem`, `trusted_ca.pem`) are placeholders that the run never
opens; replace them with your provider's own configuration.

## Build and run

Build prerequisites (toolchain, CMake, OpenSSL) are listed in the
[samples overview](../../README.md#prerequisites). Run from the repository's `c/` directory.

Linux:

```sh
cmake --preset linux-gcc-debug
cmake --build --preset linux-gcc-debug --target az_iot_sample_auth_custom_provider_template
./build/linux-gcc-debug/samples/authentication/az_iot_sample_auth_custom_provider_template
```

Windows (*Developer PowerShell for VS 2022*):

```powershell
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug --target az_iot_sample_auth_custom_provider_template
.\build\windows-msvc-debug\samples\authentication\Debug\az_iot_sample_auth_custom_provider_template.exe
```

Always built; no external dependency.

## How it ends

Exit code 0.

## Expected output

```
[custom] get_csr produced: PLACEHOLDER-BASE64-DER-CSR
[custom] persisting issued chain: 1 cert(s)
```

## Troubleshooting

| Symptom | Likely cause |
| --- | --- |
| Exit 1 with no output | `get_csr()` failed. In the template only an allocation failure does that; once you fill it in, check what `my_get_csr()` returns. |
| Build error in `main.c` after an SDK update | The provider interface changed; compare `s_my_vtable` with [az_iot_certificate_provider.h](../../../inc/azure/iot/az_iot_certificate_provider.h). The sample is always built to catch this. |

## Where to look in `main.c`

| What | Code |
| --- | --- |
| Provider state (the base must be first) | the provider struct at the top of `main.c` |
| The interface to fill in | `my_load()`, `my_get_csr()`, `my_store_issued_certificate()`, `my_sign()`, `my_destroy()` |

Interface reference: [az_iot_certificate_provider.h](../../../inc/azure/iot/az_iot_certificate_provider.h).
