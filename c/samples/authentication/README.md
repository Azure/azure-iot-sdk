# Authentication & certificate-management samples

These samples show the certificate-management options of the SDK, from a plain
X.509 file identity to CSR-based enrollment (DPS) and runtime certificate
renewal (Classic hub). See [docs/eng/certificate-management.md](../../docs/eng/certificate-management.md)
for the full design.

## Scenario matrix

| Scenario | Provider | Sample | Notes |
|----------|----------|--------|-------|
| X.509 from files (no CSR) | `az_iot_certificate_provider_pem` | [../telemetry_gen1](../telemetry_gen1/main.c), [../telemetry_gen2](../telemetry_gen2/main.c) and the other feature samples | Baseline device auth (via DPS). Identical on both generations -- the provider is generation-agnostic. |
| Direct hub connect, no DPS | `az_iot_certificate_provider_pem` | `direct_hub_gen1`, `direct_hub_gen2` | Caller-supplied hub FQDN + device cert/key. No DPS step means nothing announces the generation, so each states it at compile time via `opts.connection_profile`. |
| DPS CSR enrollment (issued operational cert) | managed (OpenSSL) | `dps_csr_managed` | Bootstrap X.509 → CSR in DPS register → operational cert persisted. |
| App-notified issuance (D4) | managed (OpenSSL) | `dps_csr_managed` | Uses `set_operational_cert_callback` to observe the issued chain. |
| Runtime Hub renewal (D7) | managed (OpenSSL) | `hub_renew` | `send_csr()` two-phase renewal on a connected Classic hub. |
| DPS CSR enrollment with an APP-OWNED provider | `sample_cert_provider` (samples/common) | `custom_certificate_provider` | Same flow as `dps_csr_managed`, but the provider - incl. real PKCS#10 issuance - lives in the samples tree so you can copy it. |
| Non-extractable key, engine/provider stack (D8) | your own (10 lines) | `hsm_pkcs11_gen1`, `hsm_pkcs11_gen2` | Key stays in a PKCS#11 token / TPM; the provider returns a `pkcs11:` URI + provider id and the **Paho adapter signs the TLS handshake through it**. Needs OpenSSL 3.0+ and a provider for the token. The custody code is identical in both. |
| Non-extractable key, no engine abstraction (D8) | your own | `hsm_sign_callback` | Only "sign these bytes" is available, so the provider implements the `sign()` hook and a BYO adapter drives the handshake through it. **Not a Paho path** — see below. |
| BYO provider (TPM / HSM / secure element / OS keystore) | your own | `custom_provider_template` | Minimal template implementing the full vtable, incl. the `sign()` hook (D8) for non-extractable keys. |

The reusable app-owned provider `sample_cert_provider` (in `samples/common`)
issues CSRs with **platform-native crypto** - OpenSSL 3.0+ on Linux
(`sample_csr_openssl.c`) and CNG/NCrypt on Windows (`sample_csr_cng.c`) - and is
the recommended starting point for your own integration.

### Which non-extractable path applies

Both D8 routes are implemented end to end, but they land on different adapters,
so pick by what the platform's crypto stack offers:

- **The key can be named** (a PKCS#11 URI, a TPM 2.0 object) and an OpenSSL 3.x
  provider for it is installed: use `client_key_uri` + `crypto_engine_id`, i.e.
  `hsm_pkcs11_gen1` / `hsm_pkcs11_gen2`. The shipping **Paho adapter honours
  this**: it resolves the URI
  through the provider and the handshake signs inside the hardware. A URI it
  cannot resolve fails the connect with `AZ_IOT_ERR_TLS` and a message naming
  it, rather than dying inside the handshake.
- **The key cannot be named** — a bare secure element, a vendor TLS stack, a
  bare-metal port: implement the vtable's `sign()` hook, i.e.
  `hsm_sign_callback`. The SDK carries the hook to the adapter, but **Paho
  cannot use it**: Paho takes its client key as a file path and exposes neither
  the `SSL_CTX` nor a key callback (upstream has none either), so it refuses a
  `sign()`-only credential with `AZ_IOT_ERR_NOT_SUPPORTED` instead of
  connecting without a client key. This route needs a BYO adapter; see
  [how_to_byo_mqtt_client.md](../../docs/how_to_byo_mqtt_client.md).

The CSR side of an HSM integration is still deployment-specific: start from
`custom_provider_template` and fill in the marked bodies (`get_csr()`,
`store_issued_certificate()`), using `sample_cert_provider` as a complete,
working reference.

## Samples

### `direct_hub_gen1` / `direct_hub_gen2`
Direct (no DPS) connection to an IoT Hub given a hub FQDN and X.509 device
credentials, then sends one telemetry message. Because there is no DPS step,
nothing on the wire announces the hub generation, so each sample states it at
compile time with `opts.connection_profile` — `AZ_IOT_CONNECTION_PROFILE_CLASSIC`
(MQTT v3.1.1) in `_gen1`, `AZ_IOT_CONNECTION_PROFILE_MQTT_V5` for an IoT Hub Next /
Event Grid (AEG) endpoint in `_gen2`. That is what a direct-connect deployment
actually looks like: the hub it was handed does not change generation underneath
it. When the generation genuinely is not known until runtime, that is DPS
territory — see [../connection_profile_fallback](../connection_profile_fallback/main.c).
Fill in the `SAMPLE_*` constants at the top of `main.c` or set the env vars
below. Requires the Paho adapter.

### `custom_provider_template`
Self-contained, no external dependencies, does not connect. A copy-paste
starting point for a custom `az_iot_certificate_provider`. Its `main()`
exercises the vtable so the wiring compiles and round-trips.

### `dps_csr_managed`
DPS enrollment that obtains an operational certificate via CSR using the
OpenSSL-backed managed provider, then connects to the assigned hub with the
issued identity. Requires the managed provider (OpenSSL 3.0+) and the Paho
adapter.

### `hub_renew`
Runtime operational-certificate renewal against a connected Classic hub: builds
a fresh CSR from the managed provider, calls `az_iot_connection_client_send_csr()`,
and persists the renewed chain. Requires the managed provider and Paho.

### `hsm_pkcs11_gen1` / `hsm_pkcs11_gen2`
Provisions through DPS and connects with a device key that never leaves a
PKCS#11 token (D8). The provider returns `client_key_uri` + `crypto_engine_id`
and no key material at all; the certificate stays an ordinary PEM file, because
a certificate is public. Requires the Paho adapter, OpenSSL 3.0+, and an
OpenSSL 3.x provider for the token (`pkcs11-provider` for PKCS#11,
`tpm2-openssl` for TPM 2.0) that OpenSSL can find.

Key custody is generation-agnostic — the token signs a TLS handshake and neither
MQTT version is visible to it. The two samples differ only in which telemetry
client they build and which MQTT adapters they register (`_gen2` registers
v3.1.1 as well, because the DPS leg speaks it), which leaves the custody code
visible as the part that does not change.

The provider must register a **decoder for its own key-reference PEM**, because
that is what OpenSSL — and therefore Paho — uses to resolve the file back to the
key inside the token. `pkcs11-provider` does so from **0.5**; older builds
(Ubuntu 24.04 packages 0.3) are detected at connect time and refused with a
message saying so, rather than failing inside the handshake.

`c/eng/setup-pkcs11-provider.sh` builds a suitable provider and
`c/eng/setup-softhsm.sh` provisions a SoftHSM2 token from an existing device key
and prints the environment this sample expects. Together they are how CI runs
the custody tests.

### `hsm_sign_callback`
The `sign()` hook (D8) for a stack with no engine or provider abstraction.
Self-contained: it brings a stand-in adapter so it runs anywhere with no
hardware, no broker and no network, and shows the exact line an integrator
writes in their own adapter to consume `tls.sign` / `tls.sign_ctx`. Replace two
things to make it real: the hook body, and the stand-in adapter.

### `custom_certificate_provider`
The COMPLETE app-owned path: a full `az_iot_certificate_provider`
(`samples/common/sample_cert_provider.*`) that issues a real PKCS#10 CSR with
platform-native crypto (OpenSSL on Linux, CNG on Windows), persists the issued
chain, and drives the same DPS CSR enrollment as `dps_csr_managed`. Copy this to
integrate your own crypto. Requires Paho; on non-Windows also OpenSSL 3.0+.

## Environment variables

The `direct_hub_gen1` / `direct_hub_gen2` samples (no DPS) use:

| Variable | Meaning |
|----------|---------|
| `AZ_IOT_HUB_HOSTNAME` | Direct hub FQDN |
| `AZ_IOT_DEVICE_ID` | Device id / MQTT client id |
| `AZ_IOT_CLIENT_CERT` | Device X.509 certificate path |
| `AZ_IOT_CLIENT_KEY` | Device X.509 private key path |
| `AZ_IOT_TRUSTED_CA` | Trusted CA path (optional; system store if unset) |

Shared (all DPS-based connecting samples):

| Variable | Meaning |
|----------|---------|
| `AZ_IOT_DPS_ID_SCOPE` | DPS ID scope |
| `AZ_IOT_DPS_REGISTRATION_ID` | Registration id / device id |
| `AZ_IOT_CLIENT_CERT` | Bootstrap X.509 certificate path |
| `AZ_IOT_CLIENT_KEY` | Bootstrap X.509 private key path |
| `AZ_IOT_TRUSTED_CA` | Trusted CA path |

The `hsm_pkcs11_*` samples replace `AZ_IOT_CLIENT_KEY` with the key reference:

| Variable | Meaning |
|----------|---------|
| `AZ_IOT_CLIENT_KEY_URI` | Key reference, e.g. `pkcs11:token=aziot;object=device-key;type=private` |
| `AZ_IOT_CRYPTO_ENGINE_ID` | OpenSSL provider id: `pkcs11`, `tpm2` |

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

`c/eng/setup-softhsm.sh` generates a configuration of this shape and exports
`OPENSSL_CONF`, so the repo's own SoftHSM2 setup needs nothing further. What it
writes differs in the paths, which it fills in rather than hard-codes: the
`pkcs11-module-path` it detected, and a `module = <dir>/pkcs11.so` line only
when `OPENSSL_MODULES` names a directory that actually holds one. Compare
yours for the two `pkcs11-module-*` settings and `activate`, not line for line.

Managed-provider samples additionally use (optional, with defaults):

| Variable | Default | Meaning |
|----------|---------|---------|
| `AZ_IOT_OPERATIONAL_KEY` | `operational_key.pem` | Operational private key (loaded if present, else generated). |
| `AZ_IOT_OPERATIONAL_CERT` | `operational_cert.pem` | Where the issued operational chain is persisted. |

## Building

The samples build with the rest of the tree when `AZ_IOT_BUILD_SAMPLES=ON`
(default). The two connecting samples appear only when OpenSSL 3.0+ is available
(`AZ_IOT_WITH_CERT_PROVIDER_MANAGED`) and the Paho adapter is enabled
(`AZ_IOT_WITH_PAHO`). The `hsm_pkcs11_*` samples need only the Paho adapter to
build; the provider they drive is a run-time requirement. The template and
`hsm_sign_callback` always build.
