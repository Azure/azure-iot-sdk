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
| SAS to DPS, SAS to the hub (proposed) | [`dps_symmetric_key`](dps_symmetric_key/README.md) | `az_iot_sas_signer_symmetric_key` (both roles) |
| SAS to DPS, DPS-issued certificate to the hub (proposed) | [`dps_symmetric_key_csr`](dps_symmetric_key_csr/README.md) | signer (DPS) + managed (no bootstrap) |
| Skeleton of a SAS signer (proposed) | [`custom_sas_signer_template`](custom_sas_signer_template/README.md) | your own `az_iot_sas_signer` |

## Onboarding and operational credentials (proposed)

Not implemented; the `dps_symmetric_key*` samples show the proposed API and are not built.
`az_iot_connection_client_options::sas` takes one `az_iot_sas_signer` per role. A role with a signer uses SAS; a role without one uses X.509 from `certificate_provider`.

| DPS (onboarding) | Hub (operational) | Service | How |
| --- | --- | --- | --- |
| SAS | SAS, mqttv3 | Supported | `dps_symmetric_key` |
| SAS | DPS-issued X.509, mqttv3 or mqttv5 | Supported (certificate management preview) | `dps_symmetric_key_csr` |
| X.509 | DPS-issued X.509, mqttv3 or mqttv5 | Supported (certificate management preview) | `dps_csr_managed`, `custom_certificate_provider` |
| X.509 | Same X.509, mqttv3 or mqttv5 | Supported | `unified/`, `mqttv5/` samples |
| X.509 | SAS | Not provisioned by DPS | Expressible: set only `sas.operational`. |
| SAS | SAS, mqttv5 | Not supported by the service | `dps_symmetric_key`; not blocked by the SDK, a refusal is `AZ_IOT_ERR_IDENTITY_REJECTED`. |
| TPM attestation | any | Not available over MQTT | Out of scope. A TPM can hold the X.509 or SAS key. |

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
| `custom_provider_template`, `hsm_sign_callback`, `custom_sas_signer_template` | Always |
| `dps_symmetric_key`, `dps_symmetric_key_csr` | Not yet (proposed API) |
