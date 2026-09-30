<!-- Copyright (c) Microsoft. All rights reserved.
     Licensed under the MIT license. See LICENSE file in the project root for full license information. -->

# Non-extractable key in a PKCS#11 token or TPM (`authentication/hsm_pkcs11`)

Provisions through DPS and connects with a device private key that **never leaves the hardware**.
The certificate provider hands the SDK a key *reference* (an RFC 7512 `pkcs11:` URI plus the id of
the OpenSSL provider that owns it) instead of a key, and the Paho adapter signs the TLS handshake
through the token. After connecting, the sample sends one telemetry message
(`{"custody":"hardware"}`).

## Sample features

- Hub generations: mqttv3 and mqttv5, whichever DPS assigns. Key custody does not depend on the
  MQTT version; the telemetry client is built once `CONNECTED`, for the assigned generation.
- Any PKCS#11 module works (SoftHSM2, a TPM through tpm2-pkcs11, an ATECC608, a smart card).
- A key reference that cannot be resolved fails the connect with `AZ_IOT_ERR_TLS` and a log line
  naming the URI, rather than failing inside the handshake.
- For a key that cannot be named by a URI and a stack with no OpenSSL provider, see
  [`hsm_sign_callback`](../hsm_sign_callback/README.md).
- Platforms: Linux. The code also builds on Windows, where it is untested; CI exercises it on Linux with SoftHSM2.

## Service requirements

- An Azure IoT Hub Device Provisioning Service (DPS) instance linked to an IoT Hub.
- An X.509 enrollment for the device certificate whose private key is in the token. For an mqttv3
  hub, [Quickstart: Provision an X.509 certificate simulated device](https://learn.microsoft.com/azure/iot-dps/quick-create-simulated-device-x509) walks
  through the enrollment.

## Device requirements

- OpenSSL 3.0+.
- An OpenSSL 3.x provider for the token that OpenSSL can find (installed in its modules directory,
  or `OPENSSL_MODULES` pointing at it): `pkcs11-provider` for PKCS#11, `tpm2-openssl` for TPM 2.0.
- The provider must register a **decoder for its own key-reference PEM**: that is how OpenSSL, and
  therefore Paho, resolves the reference back to the key. `pkcs11-provider` does from **0.5**;
  older builds (Ubuntu 24.04 packages 0.3) are detected at connect time and refused with a message
  saying so.
- The Paho adapter built with key custody (`AZ_IOT_PAHO_KEY_CUSTODY`, on by default with OpenSSL
  3.0+; configure prints `non-extractable key custody enabled`).

For testing without hardware, the repository's CI uses two helper scripts:
[`c/eng/setup-pkcs11-provider.sh`](../../../eng/setup-pkcs11-provider.sh) builds a suitable
`pkcs11-provider`, and [`c/eng/setup-softhsm.sh`](../../../eng/setup-softhsm.sh) imports an
existing device key into a SoftHSM2 token, deletes the key file, and prints the environment this
sample expects. They are test helpers, not a production setup.

## Configure

| Variable | Required | Meaning |
| --- | --- | --- |
| `AZ_IOT_DPS_ID_SCOPE` | yes | DPS ID scope. |
| `AZ_IOT_DPS_REGISTRATION_ID` | yes | Registration ID; must equal the device certificate's common name. |
| `AZ_IOT_CLIENT_CERT` | yes | Device certificate (PEM file). A certificate is public, so it stays a file. |
| `AZ_IOT_CLIENT_KEY_URI` | yes | Key reference, e.g. `pkcs11:token=aziot;object=device-key;type=private`. |
| `AZ_IOT_CRYPTO_ENGINE_ID` | no | OpenSSL provider id that owns the key, e.g. `tpm2`. Default `pkcs11`. |
| `AZ_IOT_TRUSTED_CA` | no | CA bundle (PEM file) for the DPS and IoT Hub server certificates. Default: the system store. |
| `AZ_IOT_DPS_GLOBAL_ENDPOINT` | no | Provisioning endpoint. Default `global.azure-devices-provisioning.net`. |
| `AZ_IOT_PAHO_TRACE` | no | Set to enable Paho MQTT trace and verbose OpenSSL TLS errors. |

Unset required variables make the sample print what it needs and exit 0 without connecting.

**If the token needs a PIN**, name a file the provider reads it from rather than
putting the PIN in the URI:

```
AZ_IOT_CLIENT_KEY_URI='pkcs11:token=aziot;object=device-key;type=private?pin-source=file:/etc/az-iot/token-pin'
```

Paho takes the private key as a file path, so for a provider that does not encode
its own key reference the adapter has to write the URI into a reference file for
OpenSSL to resolve later. An inline `?pin-value=<PIN>` would therefore be copied
to disk by the one code path whose purpose is that the key never lands there, so
the adapter refuses it. `pin-source` names where the PIN lives instead, which
keeps the reference loadable without putting the secret in it.

**Activate the provider from OpenSSL configuration.** This is not optional --
without it a TLS 1.2 client-authentication handshake fails outright:

```
error:40800054:pkcs11:p11prov_GetOperationState:...:Error returned by C_GetOperationState
```

An OpenSSL 3.x PKCS#11 provider offers digest implementations as well as key
operations, so a provider loaded at run time can end up servicing the TLS
handshake transcript hash. TLS 1.2 duplicates that digest context, the provider
implements the duplication with `C_GetOperationState`, and most tokens do not
support that on a digest session. TLS 1.3 does not duplicate the context, so the
same credential can work against one endpoint and fail against another purely on
negotiated version -- which is what makes this worth stating plainly.

Point `OPENSSL_CONF` at a configuration that brings the provider up alongside
the default one. Measured against a live IoT Hub over TLS 1.2 with a token-held
key: without this the connect fails with the error above, with it the client
reaches CONNECTED.

```ini
openssl_conf = az_iot_init

[az_iot_init]
providers = az_iot_providers

[az_iot_providers]
default = az_iot_default_sect
pkcs11 = az_iot_pkcs11_sect

[az_iot_default_sect]
activate = 1

[az_iot_pkcs11_sect]
pkcs11-module-path = /usr/lib/softhsm/libsofthsm2.so   # your PKCS#11 module
# Precautionary. Inert on a token that advertises no digests (SoftHSM2 does
# not); it matters on tokens that do, where it keeps hashing in the default
# provider and leaves only signing in the token.
pkcs11-module-block-operations = digest
# Required with SoftHSM2: without it the process reaches CONNECTED and then
# crashes when OpenSSL tears the provider down.
pkcs11-module-quirks = no-deinit
activate = 1
```

`activate = 1` is the load-bearing line: a provider brought up from
configuration completes the handshake, one the application loads later by name
does not, and the settings above apply only to the former.

[`c/eng/setup-softhsm.sh`](../../../eng/setup-softhsm.sh) generates a configuration of this shape and exports
`OPENSSL_CONF`, so the repo's own SoftHSM2 setup needs nothing further. What it
writes differs in the paths, which it fills in rather than hard-codes: the
`pkcs11-module-path` it detected, and a `module = <dir>/pkcs11.so` line only
when `OPENSSL_MODULES` names a directory that actually holds one. Compare
yours for the two `pkcs11-module-*` settings and `activate`, not line for line.

## Build and run

Build prerequisites (toolchain, CMake, OpenSSL) are listed in the
[samples overview](../../README.md#prerequisites). Run from the repository's `c/` directory.

Linux:

```sh
cmake --preset linux-gcc-debug
cmake --build --preset linux-gcc-debug --target az_iot_sample_auth_hsm_pkcs11
./build/linux-gcc-debug/samples/authentication/az_iot_sample_auth_hsm_pkcs11
```

The target builds whenever the Paho adapter is enabled; the token and its OpenSSL provider are
needed only at run time.

## How it ends

Exit code 0 when the telemetry message was sent; 1 on any failure. Exit code 0 without connecting
when not configured.

## Expected output

```
[hsm_pkcs11] provisioning '<registration-id>'; key stays in the token (pkcs11:... via 'pkcs11')
[hsm_pkcs11] connected; the handshake signed inside the token
[hsm_pkcs11] telemetry sent
```

## Troubleshooting

| Symptom | Likely cause |
| --- | --- |
| `[hsm_pkcs11] not configured; skipping.` | A required variable is unset. |
| `p11prov_GetOperationState ... C_GetOperationState` | The provider is not activated from OpenSSL configuration; see above. |
| `AZ_IOT_ERR_TLS` naming the key URI | The URI does not resolve: wrong token or object, provider not found, or provider older than 0.5. |
| `failed to connect (state=..., reason=...)` | See the SDK log lines before it; DPS and network causes are the same as for the other samples, e.g. `dps register: errorCode=...`. |

## Where to look in `main.c`

| What | Code |
| --- | --- |
| Configuration | `hsm_config_load()` |
| The provider: a key reference instead of a key | `hsm_load()` sets `client_key_uri` and `crypto_engine_id` |
| Build the telemetry client for the assigned generation | after `CONNECTED` in `main()` |
| Send | `az_iot_mqttv3_telemetry_client_send()` / `az_iot_mqttv5_telemetry_client_send()`, `on_send_done()` |

Design background: [certificate-management.md](../../../docs/eng/certificate-management.md#device-certificate-storage-methods).
