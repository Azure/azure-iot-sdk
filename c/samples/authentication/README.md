# Authentication & certificate-management samples

These samples show the certificate-management options of the SDK, from a plain
X.509 file identity to CSR-based enrollment (DPS) and runtime certificate
renewal (Classic hub). See [docs/eng/certificate-management.md](../../docs/eng/certificate-management.md)
for the full design.

## Scenario matrix

| Scenario | Provider | Sample | Notes |
|----------|----------|--------|-------|
| X.509 from files (no CSR) | `az_iot_certificate_provider_pem` | [../telemetry](../telemetry/main.c) and the other feature samples | Baseline device auth. |
| DPS CSR enrollment (issued operational cert) | managed (OpenSSL) | `dps_csr_managed` | Bootstrap X.509 → CSR in DPS register → operational cert persisted. |
| App-notified issuance (D4) | managed (OpenSSL) | `dps_csr_managed` | Uses `set_operational_cert_callback` to observe the issued chain. |
| Runtime Hub renewal (D7) | managed (OpenSSL) | `hub_renew` | `send_csr()` two-phase renewal on a connected Classic hub. |
| DPS CSR enrollment with an APP-OWNED provider | `sample_cert_provider` (samples/common) | `custom_certificate_provider` | Same flow as `dps_csr_managed`, but the provider - incl. real PKCS#10 issuance - lives in the samples tree so you can copy it. |
| BYO provider (TPM / HSM / secure element / OS keystore) | your own | `custom_provider_template` | Minimal template implementing the full vtable, incl. the `sign()` hook (D8) for non-extractable keys. |

The reusable app-owned provider `sample_cert_provider` (in `samples/common`)
issues CSRs with **platform-native crypto** - OpenSSL 3.0+ on Linux
(`sample_csr_openssl.c`) and CNG/NCrypt on Windows (`sample_csr_cng.c`) - and is
the recommended starting point for your own integration.

The HSM / PKCS#11 / non-extractable-key scenarios are deployment-specific.
Start from `custom_provider_template` and fill in the marked bodies
(`get_csr()`, `sign()`, `store_issued_certificate()`), using `sample_cert_provider`
as a complete, working reference.

## Samples

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

### `custom_certificate_provider`
The COMPLETE app-owned path: a full `az_iot_certificate_provider`
(`samples/common/sample_cert_provider.*`) that issues a real PKCS#10 CSR with
platform-native crypto (OpenSSL on Linux, CNG on Windows), persists the issued
chain, and drives the same DPS CSR enrollment as `dps_csr_managed`. Copy this to
integrate your own crypto. Requires Paho; on non-Windows also OpenSSL 3.0+.

## Environment variables

Shared (all connecting samples):

| Variable | Meaning |
|----------|---------|
| `AZ_IOT_DPS_ID_SCOPE` | DPS ID scope |
| `AZ_IOT_DPS_REGISTRATION_ID` | Registration id / device id |
| `AZ_IOT_CLIENT_CERT` | Bootstrap X.509 certificate path |
| `AZ_IOT_CLIENT_KEY` | Bootstrap X.509 private key path |
| `AZ_IOT_TRUSTED_CA` | Trusted CA path |

Managed-provider samples additionally use (optional, with defaults):

| Variable | Default | Meaning |
|----------|---------|---------|
| `AZ_IOT_OPERATIONAL_KEY` | `operational_key.pem` | Operational private key (loaded if present, else generated). |
| `AZ_IOT_OPERATIONAL_CERT` | `operational_cert.pem` | Where the issued operational chain is persisted. |

## Building

The samples build with the rest of the tree when `AZ_IOT_BUILD_SAMPLES=ON`
(default). The two connecting samples appear only when OpenSSL 3.0+ is available
(`AZ_IOT_WITH_CERT_PROVIDER_MANAGED`) and the Paho adapter is enabled
(`AZ_IOT_WITH_PAHO`). The template always builds.
