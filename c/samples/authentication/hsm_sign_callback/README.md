<!-- Copyright (c) Microsoft. All rights reserved.
     Licensed under the MIT license. See LICENSE file in the project root for full license information. -->

# Non-extractable key through a sign() callback (`authentication/hsm_sign_callback`)

For a secure element whose key cannot be named by a URI and whose TLS stack has no OpenSSL
provider for it: the only operation available is "sign these bytes". The certificate provider
implements `sign()`, and the MQTT adapter's TLS layer drives the handshake signature through it.

## Sample features

- Shows the whole path: a provider implementing `sign()`, the SDK passing it to the adapter (provider interface version 2 or later), and the adapter reading `tls.sign` / `tls.sign_ctx` from its connect options and calling it. That last step is the line an integrator writes in their own adapter.
- **Not a Paho path.** Paho takes its client key as a file path and has no key callback, so the Paho adapter refuses a `sign()`-only credential with `AZ_IOT_ERR_NOT_SUPPORTED`. With Paho, use [`hsm_pkcs11`](../hsm_pkcs11/README.md).
- Uses a stand-in adapter that performs the signature and then reports it has no transport, so the sample runs anywhere: no broker, no hardware, no network.
- To make it real, replace `sign()` with a call into your secure element, and the stand-in with your own adapter ([Bring your own MQTT client](../../../docs/how_to_byo_mqtt_client.md)).
- Platforms: Linux and Windows.

## Service requirements

None. The sample does not connect.

## Build and run

Build prerequisites (toolchain, CMake, OpenSSL) are listed in the
[samples overview](../../README.md#prerequisites). Run from the repository's `c/` directory.

Linux:

```sh
cmake --preset linux-gcc-debug
cmake --build --preset linux-gcc-debug --target az_iot_sample_auth_hsm_sign_callback
./build/linux-gcc-debug/samples/authentication/az_iot_sample_auth_hsm_sign_callback
```

Windows (*Developer PowerShell for VS 2022*):

```powershell
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug --target az_iot_sample_auth_hsm_sign_callback
.\build\windows-msvc-debug\samples\authentication\Debug\az_iot_sample_auth_hsm_sign_callback.exe
```

Always built; no external dependency.

## How it ends

Exit code 0 when the adapter called `sign()` exactly once; otherwise 1. `open()` returning `AZ_IOT_ERR_NOT_SUPPORTED` is expected: the stand-in has no transport.

## Expected output

```
[hsm_sign_callback] adapter received a sign() hook (ctx=0x7ffd...); a real adapter installs it in its TLS stack here
[hsm_sign_callback] sign() produced 32 bytes without the key ever leaving the element
[hsm_sign_callback] open() returned AZ_IOT_ERR_NOT_SUPPORTED after 1 sign() call(s)
```

## Where to look in `main.c`

| What | Code |
| --- | --- |
| Provider with a sign() hook | `sign_load()`, `sign_sign()`, `sign_release()`, `sign_deinit()` |
| Where an adapter consumes the hook | `standin_connect()` |
| Result of the run | `on_conn_state()` and the end of `main()` |
