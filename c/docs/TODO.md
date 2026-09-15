<!-- Copyright (c) Microsoft. All rights reserved.
     Licensed under the MIT license. See LICENSE file in the project root for full license information. -->

# TODO — Dual Hub Support (Classic + Next)

Canonical pending-work tracker for IoT Hub Next (AEG) integration.

---

## Phase 1: DPS Bypass Mock Switch

- [x] Add `AZ_IOT_HUB_NEXT_MOCK_ENDPOINT` env var check in `connection_client_open()`
- [x] Add `AZ_IOT_DEVICE_ID` env var to `sample_utils`
- [x] Register both v3.1.1 and v5 factories in samples

## Phase 2: Protocol Profile for Next

- [x] Extend `az_iot_protocol_profile` with Next-specific fields
- [x] Implement `s_profile_next` in `protocol_profile.c`
- [x] ~~Unit test: `profile_for_hub_next_role_is_next`~~ — moot: the whole
  `protocol_profile` module was deleted in P4 once each generation's feature
  clients owned their own topics.

## Phase 3: MQTT v5 Properties in Adapter Interface

- [x] Add `az_iot_mqtt_user_property` type to `az_iot_mqtt_iface.h`
- [x] Extend `az_iot_mqtt_message` with typed user_properties array
- [x] Extend inbound `az_iot_mqtt_event` (done — piggybacks on message)
- [ ] Update Paho v5 adapter to set v5 User Properties on outbound PUBLISH
- [ ] Update Paho v5 adapter to extract User Properties from inbound MESSAGE events
- [ ] Wire `session_present` from CONNACK into `EVT_CONNECTED` event

## Phase 4: Session Lifecycle

- [ ] Create `src/core/session_client.c` + internal header
- [ ] Generate UUID for sessionId (platform helper or inline)
- [ ] Set LWT in CONNECT options for HUB_NEXT
- [ ] Publish session open on `session_present=0`
- [ ] Extend adapter vtable connect options: `clean_start`, `session_expiry_interval`, LWT fields
- [ ] Send DISCONNECT with reason code 0x04 on close (HUB_NEXT only)

## Phase 5: Feature Clients Dual-Mode

### Telemetry
- [x] Add `telemetry_send_next()` path in `telemetry_client.c`
- [x] Test telemetry against mock hub (E2E verified)

### Direct Method
- [x] Flavor-aware init: Next dispatches methods from the presence wildcard; Classic subscribes to `$iothub/methods/POST/#`
- [x] Flavor-aware respond: publishes with correlation_data (Next) or topic-encoded `$rid` (Classic)
- [x] E2E verified against mock Hub-Next (auto-trigger loops 4 methods continuously)
- [x] gen2: implement the AEG probe / exec / abandon phases (`common/Protos/directmethods.proto`)
- [x] ~~Add nanopb (protobuf) dependency via FetchContent~~ **Rejected.** The six direct-method
      messages are two varints, three length-delimited fields and a two-arm oneof, so the wire
      format actually in use is a few hundred bytes of code (`src/gen2/direct_method_codec.c`,
      pinned by `tests/unit/gen2_direct_method_codec_test.c` against frames written out from the
      `.proto` by hand). Generating it would put protoc in the path of both the CMake build and the
      ESP-IDF component build, which composes its sources by listing files, and would pull in
      `google/protobuf/timestamp.proto` solely for `Exec.exec_start` -- the one field the protocol
      declares observability-only and this SDK never reads. `presence_encode_birth()` in
      `core/connection_client.c` already encodes `presence.proto` the same way. Revisit if a
      feature arrives with messages large or variable enough that hand-rolling stops being
      auditable.
- [ ] gen2: re-check the ready-wait sweep once feature clients have a periodic tick. The sweep runs
      on inbound method messages today, so a token whose exec never arrives is only reclaimed when
      the next probe or exec shows up.

### Twin
- [x] Flavor-aware delivery (Next: presence wildcard + twin dispatch handlers; Classic: twin response/desired subscriptions)
- [x] GET and PATCH reported use correlation_data for Next path
- [x] Desired push handler for Next path
- [x] E2E verified against mock Hub-Next
- [x] Split into `az_iot_gen1_twin_client` / `az_iot_gen2_twin_client`; gen2 binds its inbound
      topics at connect instead of resolving the device id inside `init()`
- [x] Desired-property subscriber registry collapsed to a single `set_desired_handler()`; its only
      consumer (ADU) was re-layered off the twin channel
- [ ] gen2: carry a desired-properties version. The service does not send one on
      `ih/{device_id}/dev/twin/desired` yet, so the handler always reports 0 and an application
      cannot tell a replay from a fresh patch the way it can on Classic's `$version`.

### C2D
- [x] `az_iot_c2d_client` feature client (header + implementation)
- [x] Classic: `devices/{reg_id}/messages/devicebound/#` subscription with prefix-based dispatch
- [x] Next: C2D delivery through the `ih/{device_id}/dev/#` presence wildcard
- [x] `c2d_receiver` sample using the feature client API
- [x] E2E verified against mock Hub-Next (auto-trigger fires rotating payloads every 5s)
- [ ] E2E verified against Classic IoT Hub
- [ ] Design C2D strict-settlement state machine (accept/reject/abandon)

## Phase 6: Connection Client Integration

- [x] Early mock-next detection in `connection_client_init()` (sets session_role + resolves device_id)
- [ ] Wire session lifecycle after HUB_NEXT CONNACK
- [ ] Skip `az_iot_hub_client_get_user_name()` for HUB_NEXT (no Classic username format)

## Phase 7: Infrastructure

- [x] Enable `PAHO_WITH_SSL=ON` in CMake
- [ ] Conformance suite v5 pass against Mosquitto
- [ ] CI: add Next-mock integration test job
- [x] Mock service auto-triggers: DM (loops forever), C2D (rotating payloads), Twin desired

---

## ADU: Root Key Package Runtime Rotation (Option B — deferred)

**Status:** Not started. Option A (compiled-in Microsoft production root keys via
`az_iot_adu_microsoft_root_keys()`, defined in `src/features/adu/adu_root_keys_microsoft.c`)
ships now and anchors trust for every Microsoft-signed update manifest out of the box.

Option B adds *rotation* on top of those immutable anchors. The hardcoded keys are
the permanent trust anchor; the Root Key Package fetched at runtime is the rotation
mechanism, and is itself signed (N-of-M threshold) by the hardcoded keys. The
package is referenced by an **unprotected (unsigned)** URL — keys from it must NEVER be
trusted directly; they are only trusted because the compiled-in anchor keys vouch for
them. Skipping anchor validation would be a remote-code-execution backdoor.

**Where the URL comes from:** ADUv1 carried it as the top-level twin property
`rootKeyPackageUrl`. That channel is cut; under ADUv2 it arrives as
`serviceConfiguration.rootKeyDownloadUrl` in the update-check response
(see [eng/aduv2-spec.md](eng/aduv2-spec.md)). Either way it is unsigned input.

Work items:
- [ ] Surface `serviceConfiguration.rootKeyDownloadUrl` from the update-check response to the app.
- [ ] Add a platform hook (mirroring the existing `download_fn` shape) for the app to
      fetch the root key package bytes — the SDK ships no HTTP client by design.
- [ ] Parse the root key package (kid list, key blobs, package signatures, isKeyTampered/
      disabled flags).
- [ ] Validate the package against the compiled-in anchor keys: verify N-of-M threshold
      signatures over the package payload before trusting any key inside it.
- [ ] Enforce threshold/continuity rules (a rotation must remain verifiable by the
      currently trusted set; reject downgrade/replay).
- [ ] Apply the validated package to the in-memory root key store (respecting
      `AZ_IOT_ADU_MAX_ROOT_KEYS`), honoring disabled/revoked entries.
- [ ] Unit tests: valid package applies; package with too-few valid signatures rejected;
      package signed by an untrusted/disabled key rejected; downgrade/replay rejected.
- [ ] Update `docs/eng/adu-client-plan.md` (§ "Root Key Package runtime rotation",
      currently 🔜 Deferred) once implemented.

**Dependencies:** Option A anchor keys (done). Needs the ADUv2 channel for the package URL.
Distinct from ADUv2 Day-0 recovery.

---

## Code Quality

- [ ] Make sure all errors are checked in the code (make a list first, then after
      reviewed, implement).

---

_Last updated: 2026-06-04_
