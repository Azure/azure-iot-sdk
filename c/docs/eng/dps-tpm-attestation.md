# DPS TPM attestation

Status: registration step implemented (`az_iot_dps_tpm_client.h`); no sample, no e2e test yet,
and no connection client integration (see [Remaining work](#remaining-work)).

## Problem

DPS TPM attestation proves possession of the TPM's endorsement key (EK) with a challenge: DPS
encrypts a key to the EK and storage root key (SRK), and only that TPM can import it. DPS runs
this flow over HTTPS and AMQP, not MQTT. The connection client's DPS session is MQTT, so it
cannot carry it. A TPM could already hold an X.509 key (`hsm_pkcs11`) or sign SAS tokens
(`user_provided_sas_token`), but not attest to DPS.

## Flow

```mermaid
sequenceDiagram
    participant App
    participant C as az_iot_dps_tpm_client
    participant TPM
    participant DPS as DPS (HTTPS)
    participant CC as az_iot_connection_client
    App->>C: get_request()
    C-->>App: PUT /{scope}/registrations/{id}/register (EK, SRK)
    App->>DPS: send
    DPS-->>App: 401 {authenticationKey, keyName}
    App->>C: handle_response()
    C->>TPM: import_key(blob)
    Note over TPM: ActivateCredential(EK, SRK), Import, Load
    App->>C: get_request()
    C->>TPM: hmac_sha256(sr + "\n" + se)
    C-->>App: PUT register + Authorization: SharedAccessSignature
    App->>DPS: send
    DPS-->>App: 202 {operationId, status: assigning} + Retry-After
    loop until assigned
        App->>C: get_request()
        C-->>App: GET /{scope}/registrations/{id}/operations/{opId}
        App->>DPS: send
        DPS-->>App: 200 {status, registrationState}
    end
    C->>TPM: import_key(registrationState.tpm.authenticationKey)
    C-->>App: assignment (hub, device ID, connectionProfile)
    App->>CC: host, client_id, connection_profile, hub_auth.sas.on_sas_token_required
    CC->>App: token request (resource URI, key name)
    App->>TPM: hmac_sha256(string to sign)
    App->>CC: update_sas_token()
```

The authentication key is the concatenation of TPM2B_ID_OBJECT, TPM2B_ENCRYPTED_SECRET,
TPM2B_PRIVATE, TPM2B_ENCRYPTED_SECRET and TPM2B_PUBLIC, optionally followed by UINT16-sized
encrypted data. `az_iot_dps_tpm_parse_key()` splits it into the inputs of TPM2_ActivateCredential
(credential blob, secret) and TPM2_Import (duplicate, inSymSeed, objectPublic).

## Design

```mermaid
flowchart LR
    subgraph SDK["SDK (no I/O, no allocation)"]
        C[az_iot_dps_tpm_client<br/>request builder + response parser]
        S[az_iot_sas_token.h]
        CC[az_iot_connection_client]
    end
    subgraph App
        H[HTTPS stack<br/>libcurl, platform]
        T[TSS<br/>tpm2-tss, wolfTPM]
    end
    C -- request --> H
    H -- status, body, Retry-After --> C
    C -- import_key, hmac_sha256 --> T
    C --> S
    App -- assignment --> CC
    CC -- on_sas_token_required --> T
```

- **A codec, not a transport.** The SDK has no HTTP client, and adding one (TLS, proxy,
  redirects, per platform) is out of proportion to one provisioning step. The client builds each
  request into a caller buffer and consumes each response; any HTTPS stack sends them. Same shape
  as azure-sdk-for-c's provisioning client.
- **TPM through two callbacks.** `import_key` and `hmac_sha256` are all the flow needs; the
  application keeps its TSS, handles and persistence policy.
- **States.** `REGISTER` (no token) -> 401 with key -> `AUTH_REGISTER` -> 202 -> `POLL` ->
  `assigned`. 429 and 5xx keep the request for a retry after `Retry-After` (default 3 s). A 401
  after the key exchange is `AZ_IOT_ERR_AUTH`; any other 4xx, or status `failed`/`disabled`, is
  `AZ_IOT_ERR_DPS` with the DPS `errorCode`. A failure ends the registration; `init()` restarts it.
- **SAS.** Resource URI `{scope}%2fregistrations%2f{id}`, key name from the 401 (`registration`
  if absent), a fresh token per request from the caller's Unix time.
- **Hub.** The assignment maps to a direct connection. mqttv3 hubs take the TPM-signed SAS token
  through the existing `on_sas_token_required`. mqttv5 hubs take X.509 only, so the option `csr`
  is sent and `issuedCertificateChain` returned.
- **Registration ID.** `az_iot_dps_tpm_registration_id_from_ek()` gives lowercase unpadded base32
  of SHA-256(EK), the ID the earlier Azure IoT C SDK derives.

## Alternatives not taken

```mermaid
flowchart TD
    A[TPM attestation in C] --> B{Where?}
    B -->|MQTT DPS session in the connection client| X1[Not possible:<br/>DPS has no TPM flow over MQTT]
    B -->|HTTPS inside the connection client| X2[Not now: needs an HTTP adapter interface,<br/>TLS/proxy per platform, and a second DPS<br/>path through re-provisioning and retries]
    B -->|AMQP| X3[No AMQP stack in this SDK]
    B -->|Standalone HTTPS codec| OK[Chosen: small, testable,<br/>works with any HTTPS stack and TSS]
```

## Remaining work

- **Sample** with libcurl and tpm2-tss (ESAPI): EK/SRK creation, ActivateCredential, Import,
  Load, EvictControl, HMAC.
- **E2E test** against DPS with an individual TPM enrollment, using a TPM simulator in CI.
- **Connection client integration** (optional): re-provisioning after a rejected hub identity,
  which the connection client does only over its MQTT DPS session. Until then the application
  registers again.
- **Service dependencies:** certificate issuance (`csr`) under TPM attestation, for mqttv5 hubs,
  and SAS on mqttv5 hubs. Neither is documented as supported today; a TPM-attested device can
  reach an mqttv3 hub with SAS, or an mqttv5 hub only with an X.509 credential from elsewhere.
