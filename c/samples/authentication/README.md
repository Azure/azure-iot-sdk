<!-- Copyright (c) Microsoft. All rights reserved.
     Licensed under the MIT license. See LICENSE file in the project root for full license information. -->

# Authentication samples

Certificate options of the SDK, from a plain X.509 file identity to certificates issued by DPS,
renewal over IoT Hub, and private keys that never leave the hardware. Each sample has its own
README. Design background: [certificate-management.md](../../docs/eng/certificate-management.md).

| Scenario | Sample | Provider |
| --- | --- | --- |
| X.509 certificate and key from files | every [`unified/`](../unified/) and [`mqttv5/`](../mqttv5/) sample | `az_iot_certificate_provider_pem` |
| DPS issues an operational certificate from a CSR | [`dps_csr_managed`](dps_csr_managed/README.md) | managed (OpenSSL) |
| Same, with the CSR built by application code | [`custom_certificate_provider`](custom_certificate_provider/README.md) | `sample_cert_provider` (in `samples/common`) |
| Renew the operational certificate over an mqttv3 hub | [`hub_renew`](hub_renew/README.md) | managed (OpenSSL) |
| Key in a PKCS#11 token or TPM, named by a URI | [`hsm_pkcs11`](hsm_pkcs11/README.md) | your own; returns a key reference |
| Key reachable only through "sign these bytes" | [`hsm_sign_callback`](hsm_sign_callback/README.md) | your own; implements `sign()` |
| Skeleton of a certificate provider | [`custom_provider_template`](custom_provider_template/README.md) | your own |
| SAS keys to DPS and to the hub | [`dps_sas_key`](dps_sas_key/README.md) | none |
| SAS key to DPS, DPS-issued certificate to the hub | [`dps_sas_key_issued_cert`](dps_sas_key_issued_cert/README.md) | managed (OpenSSL), no bootstrap certificate |
| SAS tokens from the application, e.g. a TPM, HSM or token service (proposed) | [`user_provided_sas_token`](user_provided_sas_token/README.md) | none |

## Choosing how each role authenticates

Credential fallback, the secondary key, renewal and `user_provided_token` are proposed and not
implemented yet; see [connecting.md](../../docs/connecting.md#authentication).
`user_provided_sas_token` shows the proposed callback API and is not built.

Each role (DPS, hub) uses whichever of these sources are configured, tried in this order:

| Source | The device holds | SDK needs |
| --- | --- | --- |
| X.509 | Certificates and keys, through `certificate_provider` (one or more per role) | — |
| Primary, secondary key | Symmetric keys in `dps_auth` / `hub_auth` | `crypto`, and a Unix time (`time()` unless `unix_time` is set) |
| User-provided token | A `user_provided_token` callback | — |

The SDK moves on only when the service rejects a credential, without a retry delay, and keeps
using the one that worked. Configuring only X.509, or only SAS, uses that alone.

`trusted_ca` sets server trust for every connection, whatever the kind.

| DPS | Hub | Service | Sample |
| --- | --- | --- | --- |
| SAS | SAS, mqttv3 | Supported | `dps_sas_key`, `user_provided_sas_token` |
| SAS | DPS-issued X.509, mqttv3 or mqttv5 | Supported (certificate management preview) | `dps_sas_key_issued_cert` |
| X.509 | DPS-issued X.509, mqttv3 or mqttv5 | Supported (certificate management preview) | `dps_csr_managed` |
| X.509 | X.509 | Supported | `unified/`, `mqttv5/` samples |
| X.509 | SAS | Not provisioned by DPS | Expressible; no sample |
| SAS | SAS, mqttv5 | Not supported by the service yet | Designed for; the token request carries the hub generation. Until then, a rejection is `AZ_IOT_ERR_IDENTITY_REJECTED` |
| TPM attestation | any | Not available over MQTT | Out of scope. A TPM can hold the X.509 key (`hsm_pkcs11`) or sign SAS tokens (`user_provided_sas_token`) |

## Which non-extractable key route applies

- **The key can be named** (a PKCS#11 URI, a TPM 2.0 object) and an OpenSSL 3.x provider for it is
  installed: return `client_key_uri` + `crypto_engine_id`, as in `hsm_pkcs11`. The Paho adapter
  supports this.
- **The key cannot be named** (a bare secure element, a vendor TLS stack, a bare-metal port):
  implement the provider's `sign()` hook, as in `hsm_sign_callback`. The Paho adapter does not
  support this route; it needs your own MQTT adapter
  ([Bring your own MQTT client](../../docs/how_to_byo_mqtt_client.md)).

## Building

The samples build with the rest of the tree when `AZ_IOT_BUILD_SAMPLES=ON` (the default):

| Sample | Built when |
| --- | --- |
| `dps_csr_managed`, `hub_renew` | OpenSSL 3.0+ and the Paho adapter (`AZ_IOT_WITH_CERT_PROVIDER_MANAGED`, `AZ_IOT_WITH_PAHO`) |
| `custom_certificate_provider` | The Paho adapter, and OpenSSL 3.0+ outside Windows |
| `hsm_pkcs11` | The Paho adapter |
| `custom_provider_template`, `hsm_sign_callback` | Always |
| `dps_sas_key` | The Paho adapter and the OpenSSL crypto backend (`AZ_IOT_WITH_CRYPTO_OPENSSL`) |
| `dps_sas_key_issued_cert` | As `dps_sas_key`, plus the managed provider (`AZ_IOT_WITH_CERT_PROVIDER_MANAGED`) |
| `user_provided_sas_token` | Not yet (proposed API) |
