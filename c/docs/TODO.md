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
- [x] Update Paho v5 adapter to set v5 User Properties on outbound PUBLISH
- [x] Update Paho v5 adapter to extract User Properties from inbound MESSAGE events
- [x] Wire `session_present` from CONNACK into `EVT_CONNECTED` event — both versions. The v5 path
      came first; the v3.1.1 path now reports it too. That bit was previously recorded here as not
      needed, on the grounds that only the v5 presence/birth path consumes it. That reasoning no
      longer holds: a Classic session connects with Clean Session 0 (Phase 4 below, and
      `docs/connection.md` section 3.2), so whether the broker resumed the session or silently
      started a fresh one is reported by the CONNACK and by nothing else. Nothing in the SDK
      branches on the value — it is surfaced for the application, which cannot otherwise see it.
- [x] Prove all three end to end. Conformance cases drive the real Paho v5 adapter against a
      broker and against injected broker packets: `v5_message_properties_survive_a_roundtrip`,
      `v5_properties_from_the_server_are_surfaced_intact`,
      `v5_session_present_from_connack_is_reported`, plus
      `v3_1_1_publish_ignores_the_v5_only_fields` to keep v5 bytes off a 3.1.1 session
      (`c/tests/conformance/az_iot_conformance.c`).
      `session_present_reports_whether_the_broker_resumed_the_session` covers the same flag from
      the other direction and in BOTH suites: it drives a real resumption against the broker
      (subscribe, disconnect, reconnect non-clean) instead of an injected CONNACK.

### Not carried by the Paho adapter yet

- [ ] CONNECT User Properties. `az_iot_mqtt_connect_options.user_properties` is accepted by
      the interface and is silently ignored by the Paho v5 adapter, which puts only Session
      Expiry on the CONNECT. Nothing in the SDK sets it today; it is listed here rather than
      ticked so that stays a known gap and not a discovery.
- [ ] PUBLISH `response_topic` and `topic_alias`. Both exist on `az_iot_mqtt_message` and
      neither is serialized. Same reasoning: unused today, not implemented, not ticked.

## Phase 4: Session Lifecycle

- [ ] ~~Create `src/core/session_client.c` + internal header~~ — **not needed, deliberately not
  done.** The connection client already owns the connect/reconnect state machine, the presence
  (birth) handshake and the subscription gate, and the per-role session terms are three assignments
  on the connect options it already builds (`resolve_session_options()`). A separate file would have
  to reach into that state to say anything, so the logic lives in `connection_client.c`.
- [ ] ~~Generate UUID for sessionId (platform helper or inline)~~ — **dropped: already served by the
  connection nonce.** `presence_gen_nonce()` produces a fresh RFC 4122 version 4 UUID per CONNECT
  attempt; it rides the CONNECT username as `correlationId` and the birth PUBLISH as MQTT 5
  Correlation Data, which is the identifier the presence protocol actually defines. A second UUID
  would identify nothing the service looks at.
- [x] Set LWT in CONNECT options for HUB_NEXT — **the platform sets no Will, by design.**
  `az_iot_connection_client_options.lwt` exposes the Will to the application and is applied to the
  hub roles (never to DPS), with the Will Delay Interval and a close that carries DISCONNECT reason
  `0x04` when a Will is configured. The SDK sets no Will of its own: MQTT 5 allows exactly one Will
  per CONNECT, device presence is derived from broker-emitted connection lifecycle events rather
  than from a device-authored will message, and taking the slot would deny the application its own
  "device went away" signal. The earlier LWT-based session-close protocol was superseded by the
  presence design.
- [x] Publish session open on `session_present=0` — satisfied by the birth flow, which is stronger:
  birth is published on EVERY connection, carrying the observed `session_present` as a diagnostic
  field, because the backend must not consult that flag for state decisions.
- [x] Extend adapter vtable connect options: `clean_start`, `session_expiry_interval`, LWT fields —
  the fields already existed and the Paho adapter already honoured them; the gap was that the core
  set none of them. It does now, per role, with `session_continuity` and `session_expiry_seconds`
  overridable by the application on the hub roles (see `docs/connection.md` section 3.2).
- [x] Send DISCONNECT with reason code 0x04 on close (HUB_NEXT only) — expressed as an additive,
  zero-safe `disconnect_reason_code` on `az_iot_mqtt_connect_options` rather than a new parameter or
  vtable slot, so no bring-your-own adapter's ABI changes and a zero keeps today's normal close. The
  core sets it on HUB_NEXT only, and only when a Will is configured: 0x04 asks the broker to publish
  the Will on an orderly close, which is meaningless when there is none. The reason code carries no
  protocol meaning for the platform (presence does not depend on a will message), so this exists to
  serve an application that configured its own Will.

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
      consumer (software updates) was re-layered off the twin channel
- [x] gen2: desired deliveries carry the version and a PATCH/SNAPSHOT kind; the client fetches a
      snapshot whenever the version sequence shows the device behind.

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

## Software updates: Root Key Package Runtime Rotation (Option B — deferred)

**Status:** Not started. Option A (compiled-in Microsoft production root keys via
`az_iot_su_microsoft_root_keys()`, defined in `src/features/su/su_root_keys_microsoft.c`)
ships now and anchors trust for every Microsoft-signed update manifest out of the box.

Option B adds *rotation* on top of those immutable anchors. The hardcoded keys are
the permanent trust anchor; the Root Key Package fetched at runtime is the rotation
mechanism, and is itself signed (N-of-M threshold) by the hardcoded keys. The
package is referenced by an **unprotected (unsigned)** URL — keys from it must NEVER be
trusted directly; they are only trusted because the compiled-in anchor keys vouch for
them. Skipping anchor validation would be a remote-code-execution backdoor.

**Where the URL comes from:** Device Update for IoT Hub carried it as the top-level twin property
`rootKeyPackageUrl`. That channel is cut; under software updates it arrives as
`serviceConfiguration.rootKeyDownloadUrl` in the update-check response
(see [eng/su-spec.md](eng/su-spec.md)). Either way it is unsigned input.

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
      `AZ_IOT_SU_MAX_ROOT_KEYS`), honoring disabled/revoked entries.
- [ ] Unit tests: valid package applies; package with too-few valid signatures rejected;
      package signed by an untrusted/disabled key rejected; downgrade/replay rejected.
- [ ] Update `docs/eng/su-client-plan.md` (§ "Root Key Package runtime rotation",
      currently 🔜 Deferred) once implemented.

**Dependencies:** Option A anchor keys (done). Needs the software updates channel for the package URL.
Distinct from software updates Day-0 recovery.

---

## Code Quality

- [ ] Make sure all errors are checked in the code (make a list first, then after
      reviewed, implement).

---

_Last updated: 2026-06-04_
