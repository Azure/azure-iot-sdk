# Test coverage

Scope: Azure IoT C SDK (`c/`). One table per feature area.

| Column | Meaning |
| --- | --- |
| Group | Scenario family; shown once, on the first row of the group. |
| Test | Human-readable test name (maps 1:1 to the test function name). |
| Scenario | Extra detail only when needed; never repeats Group, Test or Type. `—` when there is nothing to add. |
| Type | Test tier: `unit`, `conformance` or `e2e`. |
| Status | `Done` = implemented and running in CI. `Pending` = not implemented. |
| Code Location | Implementing function, deep-linked to its definition line on `main`. Pending rows name the file the test belongs in. |

Types: **unit** = cmocka + fake MQTT adapter, no network (`c/tests/unit`); **conformance** = live MQTT broker via `AZ_IOT_MQTT_BROKER_HOST`, no Azure service (`c/tests/conformance`); **e2e** = real DPS + IoT Hub with the Paho adapter (`c/tests/e2e`). There is no separate `integration` tier — `conformance` fills that role.

Areas: [Connection](#connection) · [Telemetry](#telemetry-device-to-cloud) ·
[Cloud-to-device](#cloud-to-device-messages) · [Direct methods](#direct-methods) ·
[Device twin](#device-twin) · [File upload](#file-upload) · [Device update](#device-update-adu) ·
[Certificate management](#certificate-management) · [Core primitives](#core-primitives).

Every area below is scoped to **IoT Hub Classic** (MQTT v3.1.1) unless a row says
otherwise. [IoT Hub Classic protocol conformance](#iot-hub-classic-protocol-conformance)
maps the service's documented MQTT surface onto what the SDK implements and what is
pinned by a test — read it first to see which gaps are missing *tests* and which are
missing *code*. The Hub-Next / AEG (MQTT v5) surface is tracked separately, see
[Hub-Next / AEG, deferred](#hub-next--aeg-deferred). Device update is frozen for this pass.

## Connection

Covers `az_iot_connection_client` lifecycle, CONNACK handling, reconnection, the Hub-Next presence handshake, DPS-before-connect, and TLS/transport. Certificate issuance and CSR renewal are tracked in a future *Certificate management* section; only the connect-time gating of those options appears here.

| Group | Test | Scenario | Type | Status | Code Location |
| --- | --- | --- | --- | --- | --- |
| Lifecycle | Init rejects null | `init()` with NULL client/options. | unit | Done | [connection_client_init_rejects_null](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/smoke_test.c#L39) |
| | Init destroy roundtrip | `init()` → `destroy()` with no connection attempt. | unit | Done | [connection_client_init_deinit_roundtrip](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/smoke_test.c#L48) |
| | Role maps to required MQTT version | DPS/Classic → v3.1.1, Hub-Next → v5. | unit | Done | [mqtt_role_to_required_version](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/smoke_test.c#L18) |
| | Register rejects null create | Factory registered without a `create` hook. | unit | Done | [register_rejects_null_create](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_client_test.c#L143) |
| | Open without factory not supported | No factory registered for the required version. | unit | Done | [open_without_factory_returns_not_supported](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_client_test.c#L209) |
| | Open connects and goes connecting | `open()` calls adapter `connect()`; IDLE → CONNECTING. | unit | Done | [open_invokes_connect_and_transitions_to_connecting](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_client_test.c#L216) |
| | Connected event goes connected | Successful CONNACK; CONNECTING → CONNECTED. | unit | Done | [connected_event_transitions_to_connected](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_client_test.c#L237) |
| | Connack failure faults | CONNACK error, reconnection disabled; adapter torn down. | unit | Done | [connack_failure_transitions_to_faulted](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_client_test.c#L256) |
| | Close returns to idle | CONNECTED → DISCONNECTING → IDLE on DISCONNECTED event. | unit | Done | [close_disconnect_returns_to_idle](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_client_test.c#L276) |
| | Close when idle is noop | `close()` before `open()`; no state callback fires. | unit | Done | [close_when_idle_is_noop](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_client_test.c#L302) |
| | Open twice rejected | Second `open()` returns `ALREADY_INITIALIZED`. | unit | Done | [open_twice_is_rejected](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_client_test.c#L309) |
| | Send before connect rejected | Feature client publish while not CONNECTED → `NOT_CONNECTED`. | unit | Done | [send_before_connect_returns_not_connected](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/telemetry_client_test.c#L116) |
| | Two clients share one connection | — | unit | Done | [two_clients_share_one_connection](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/file_upload_client_test.c#L639) |
| | Open rejects null client | — | unit | Done | [open_rejects_null_client](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_lifecycle_test.c#L95) |
| | Open without host or dps rejected | Neither `host` nor `dps.id_scope` set. | unit | Done | [open_without_host_or_dps_is_rejected](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_lifecycle_test.c#L101) |
| | Open without client id rejected | `host` set, identity missing. | unit | Done | [open_without_client_id_is_rejected](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_lifecycle_test.c#L113) |
| | Open while connecting rejected | — | unit | Done | [open_while_connecting_is_rejected](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_lifecycle_test.c#L125) |
| | Open while connected rejected | — | unit | Done | [open_while_connected_is_rejected](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_lifecycle_test.c#L132) |
| | Open from faulted rejected | FAULTED is terminal; recovery needs destroy + init. | unit | Done | [open_from_faulted_is_rejected](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_lifecycle_test.c#L142) |
| | Open rejects a wrong-version factory | Classic session, only a v5 factory registered. | unit | Done | [open_rejects_factory_of_the_wrong_version](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_lifecycle_test.c#L157) |
| | Open targets the configured endpoint | CONNECT carries host/port/client id. | unit | Done | [open_connects_to_the_configured_endpoint](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_lifecycle_test.c#L172) |
| | Connect timeout is configurable | — | unit | Done | [connect_timeout_is_configurable](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_lifecycle_test.c#L255) |
| | The username carries the host and device id | `{iothub-hostname}/{device-id}/?…`. | unit | Done | [the_username_carries_the_host_and_device_id](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_lifecycle_test.c#L274) |
| | The username carries an api version | Built by azure-sdk-for-c, so a dependency bump could drop it unnoticed. | unit | Done | [the_username_carries_an_api_version](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_lifecycle_test.c#L288) |
| | The model id is announced in the username | Plug and Play; Device Update discovers a device by this value. | unit | Done | [the_model_id_is_announced_in_the_username](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_lifecycle_test.c#L300) |
| | No model id means none in the username | — | unit | Done | [no_model_id_means_none_in_the_username](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_lifecycle_test.c#L313) |
| | An empty model id is treated as none | — | unit | Done | [an_empty_model_id_is_treated_as_none](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_lifecycle_test.c#L322) |
| | The connect does not request a clean session | The core leaves `clean_start` false; the Paho v3.1.1 path now honours it instead of forcing a clean session. | unit | Done | [the_connect_does_not_request_a_clean_session](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_lifecycle_test.c#L332) |
| | Close rejects null client | — | unit | Done | [close_rejects_null_client](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_lifecycle_test.c#L348) |
| | Close while connecting disconnects | DISCONNECTING + one adapter `disconnect()`. | unit | Done | [close_while_connecting_disconnects_the_adapter](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_lifecycle_test.c#L354) |
| | Close while connecting suppresses a late connack | An abandoned attempt must not report CONNECTED. | unit | Done | [close_while_connecting_suppresses_a_late_connack](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_lifecycle_test.c#L368) |
| | Connack without a pending close still connects | Guards against over-suppressing a live attempt. | unit | Done | [a_connack_without_a_pending_close_still_connects](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_lifecycle_test.c#L389) |
| | Close twice is idempotent | Second close fires no transition. | unit | Done | [close_twice_is_idempotent](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_lifecycle_test.c#L400) |
| | Close from faulted reports not initialized | Adapter is already gone. | unit | Done | [close_from_faulted_reports_not_initialized](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_lifecycle_test.c#L418) |
| | Destroy while connected frees the adapter | No use-after-free, no leak. | unit | Done | [destroy_while_connected_destroys_the_adapter](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_lifecycle_test.c#L452) |
| | Destroy while connecting frees the adapter | — | unit | Done | [destroy_while_connecting_destroys_the_adapter](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_lifecycle_test.c#L475) |
| | Destroy fires no state callback | — | unit | Done | [destroy_is_silent_on_the_state_callback](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_lifecycle_test.c#L494) |
| | Destroy tolerates null | — | unit | Done | [destroy_tolerates_null](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_lifecycle_test.c#L509) |
| | Destroy while reconnect scheduled | No adapter to tear down, deadline still armed; no retry fires after. | unit | Done | [destroy_while_reconnect_is_scheduled](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_reconnect_test.c#L561) |
| | Do work rejects null client | — | unit | Done | [do_work_rejects_null_client](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_lifecycle_test.c#L519) |
| | Do work before open is inert | No adapter created, no transition. | unit | Done | [do_work_before_open_touches_no_adapter](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_lifecycle_test.c#L525) |
| | Do work after close is inert | — | unit | Done | [do_work_after_close_touches_no_adapter](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_lifecycle_test.c#L536) |
| | Do work forwards the timeout | Reaches the adapter `process_loop()` unchanged. | unit | Done | [do_work_forwards_the_timeout_to_the_adapter](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_lifecycle_test.c#L551) |
| | Do work surfaces the pump error | Adapter error is returned to the caller. | unit | Done | [do_work_surfaces_the_adapter_pump_error](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_lifecycle_test.c#L562) |
| | Reopen after close starts a second session | Fresh adapter, second CONNECTED. | unit | Done | [reopen_after_close_starts_a_second_session](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_lifecycle_test.c#L575) |
| | Set state callback rejects null client | — | unit | Done | [set_state_callback_rejects_null_client](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_lifecycle_test.c#L601) |
| | State callback carries the failure reason | Reason recorded with FAULTED matches the CONNACK status. | unit | Done | [state_callback_carries_the_failure_reason](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_lifecycle_test.c#L609) |
| | State callback can be replaced | Later registration wins; the old one goes quiet. | unit | Done | [state_callback_can_be_replaced](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_lifecycle_test.c#L622) |
| | Publish before connected rejected | — | unit | Done | [publish_before_connected_is_rejected](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_lifecycle_test.c#L641) |
| | Subscribe before connected rejected | — | unit | Done | [subscribe_before_connected_is_rejected](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_lifecycle_test.c#L653) |
| | Publish after disconnect rejected | — | unit | Done | [publish_after_disconnect_is_rejected](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_lifecycle_test.c#L665) |
| | Keep alive defaults when unset | `AZ_IOT_DEFAULT_KEEP_ALIVE_SECONDS`. | unit | Done | [keep_alive_defaults_when_unset](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_lifecycle_test.c#L211) |
| | Keep alive is configurable | Reaches the adapter unchanged; it used to be hardcoded. | unit | Done | [keep_alive_is_configurable](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_lifecycle_test.c#L220) |
| | The largest useful keep alive reaches the adapter | 1177 s, the largest value IoT Hub does not clamp. | unit | Done | [the_largest_useful_keep_alive_reaches_the_adapter](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_lifecycle_test.c#L233) |
| | Connect timeout defaults when unset | `AZ_IOT_DEFAULT_CONNECT_TIMEOUT_SECONDS`. | unit | Done | [connect_timeout_defaults_when_unset](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_lifecycle_test.c#L246) |
| CONNACK mapping | Connack success maps to ok | Code 0, both v3.1.1 and v5. | unit | Done | [connack_success_maps_to_ok](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/mqtt_iface_contract_test.c#L187) |
| | Connack v3 identity codes rejected | v3.1.1 codes 2/4/5 → `IDENTITY_REJECTED`. | unit | Done | [connack_v3_identity_codes_map_to_identity_rejected](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/mqtt_iface_contract_test.c#L194) |
| | Connack v3 transport codes | v3.1.1 codes 1/3 → `ERR_MQTT`. | unit | Done | [connack_v3_transport_codes_map_to_mqtt](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/mqtt_iface_contract_test.c#L207) |
| | Connack v5 identity codes rejected | v5 0x85/0x86/0x87/0x8C → `IDENTITY_REJECTED`. | unit | Done | [connack_v5_identity_codes_map_to_identity_rejected](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/mqtt_iface_contract_test.c#L220) |
| | Connack v5 transport codes | v5 0x80/0x88/0x89/0x97 → `ERR_MQTT`. | unit | Done | [connack_v5_transport_codes_map_to_mqtt](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/mqtt_iface_contract_test.c#L233) |
| | Connack negative codes | Adapter pre-CONNACK failures (socket, TLS) → `ERR_MQTT`. | unit | Done | [connack_negative_codes_map_to_mqtt](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/mqtt_iface_contract_test.c#L245) |
| | Connack unknown version never rejects identity | Unknown version must not trigger re-provisioning. | unit | Done | [connack_unknown_version_never_rejects_the_identity](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/mqtt_iface_contract_test.c#L260) |
| | Mapped connack reaches callback | Mapping survives the adapter → inbound callback hop. | unit | Done | [mapped_connack_status_reaches_the_inbound_callback](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/mqtt_iface_contract_test.c#L276) |
| | Connack rejection tears the adapter down | No adapter is kept alive behind a FAULTED state. | unit | Done | [connack_rejection_tears_the_adapter_down](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_lifecycle_test.c#L706) |
| | Identity rejection faults without a policy | Reconnect disabled: FAULTED carrying `IDENTITY_REJECTED`. | unit | Done | [identity_rejection_faults_when_reconnect_is_disabled](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_lifecycle_test.c#L692) |
| | Identity rejected reprovisions through dps | Retry targets DPS, not the hub that refused the credential. | unit | Done | [hub_identity_rejection_reprovisions_through_dps](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_dps_test.c#L177) |
| | Transport error does not reprovision | Cached hub assignment stays valid. | unit | Done | [hub_transport_error_reconnects_without_reprovisioning](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_dps_test.c#L200) |
| | Reprovisioning connects to the new assignment | Full loop: rejection → DPS → new hub → CONNECTED. | unit | Done | [reprovisioning_connects_to_the_new_assignment](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_dps_test.c#L222) |
| | Repeated rejection honors max attempts | A deleted enrollment must not hammer DPS. | unit | Done | [repeated_identity_rejection_still_honors_max_attempts](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_dps_test.c#L262) |
| | Identity rejection without a policy faults | No retry to carry a re-provision. | unit | Done | [identity_rejection_without_a_policy_faults](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_dps_test.c#L283) |
| Reconnection | Backoff disabled when initial delay zero | `initial_delay_ms = 0` disables reconnection. | unit | Done | [disabled_when_initial_delay_is_zero](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/reconnect_policy_test.c#L18) |
| | Backoff doubles until cap | No jitter: 100/200/400/800, capped at `max_delay_ms`. | unit | Done | [no_jitter_doubles_until_cap](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/reconnect_policy_test.c#L27) |
| | Backoff jitter within band | Delay stays inside ±`jitter_pct`. | unit | Done | [jitter_stays_within_band](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/reconnect_policy_test.c#L45) |
| | Zero max delay caps at initial | `max_delay_ms = 0` → `initial_delay_ms` is the cap. | unit | Done | [zero_max_delay_means_initial_is_the_cap](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/reconnect_policy_test.c#L63) |
| | Attempt zero treated as one | Attempt counter 0 yields the initial delay. | unit | Done | [attempt_zero_treated_as_one](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/reconnect_policy_test.c#L76) |
| | Backoff shift saturates | Large attempt counts saturate; no shift UB/overflow. | unit | Done | [shift_saturates_no_ub](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/reconnect_policy_test.c#L87) |
| | Connack failure schedules retry | Reconnect enabled: CONNECTING → RECONNECTING → CONNECTING → CONNECTED. | unit | Done | [connack_fail_with_reconnect_schedules_retry](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_client_test.c#L331) |
| | Max attempts exhausted faults | Final failure after `max_attempts` → FAULTED. | unit | Done | [max_attempts_exhausted_transitions_to_faulted](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_client_test.c#L363) |
| | Peer disconnect drives retry | Server-initiated DISCONNECT: CONNECTED → RECONNECTING → CONNECTING. | unit | Done | [peer_disconnect_with_reconnect_drives_retry](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_client_test.c#L392) |
| | Close during reconnecting goes idle | `close()` cancels the pending backoff timer. | unit | Done | [close_during_reconnecting_goes_idle](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_client_test.c#L417) |
| | User close does not reconnect | Disconnect caused by `close()` never triggers a retry. | unit | Done | [user_close_after_connected_does_not_reconnect](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_client_test.c#L442) |
| | Successful reconnect resets the attempt counter | A later outage starts backoff over instead of exhausting the cap. | unit | Done | [successful_reconnect_resets_the_attempt_counter](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_reconnect_test.c#L242) |
| | Zero max attempts never gives up | `max_attempts = 0`; six consecutive failures, still retrying. | unit | Done | [zero_max_attempts_never_gives_up](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_reconnect_test.c#L268) |
| | Retry waits for the backoff deadline | No CONNECT is issued before the delay elapses. | unit | Done | [retry_waits_for_the_backoff_deadline](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_reconnect_test.c#L195) |
| | Retry rebuilds the adapter | Old instance destroyed when scheduled; replacement starts clean. | unit | Done | [retry_destroys_the_old_adapter_and_builds_a_new_one](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_reconnect_test.c#L216) |
| | Full outage recovers to connected | ERROR → RECONNECTING → CONNECTING → CONNECTED. | unit | Done | [full_outage_recovers_to_connected](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_reconnect_test.c#L288) |
| | Adapter error event schedules a retry | `EVT_ERROR`, not DISCONNECTED. | unit | Done | [adapter_error_event_schedules_a_retry](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_reconnect_test.c#L131) |
| | Adapter error faults without a policy | Reconnect disabled. | unit | Done | [adapter_error_event_faults_without_a_policy](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_reconnect_test.c#L147) |
| | User close disconnect goes idle | Never RECONNECTING after an intentional close. | unit | Done | [user_close_disconnect_goes_idle_not_reconnecting](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_reconnect_test.c#L177) |
| | Persistent subscription issued on connect | Filter + QoS reach the adapter. | unit | Done | [persistent_subscription_is_issued_on_connect](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_reconnect_test.c#L309) |
| | Persistent subscriptions reissued after reconnect | Both filters re-sent on the new session. | unit | Done | [persistent_subscriptions_are_reissued_after_a_reconnect](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_reconnect_test.c#L330) |
| | Persistent sub registry full is rejected | Beyond `AZ_IOT_MAX_PERSISTENT_SUBS` → `NOT_SUPPORTED`. | unit | Done | [persistent_subscription_registry_full_is_rejected](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_reconnect_test.c#L362) |
| | Persistent sub added while connected subscribes now | No wait for the next reconnect. | unit | Done | [persistent_subscription_added_while_connected_subscribes_now](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_reconnect_test.c#L383) |
| | Failed subscription restore keeps the connection | Adapter refuses the SUBSCRIBE; session stays CONNECTED. | unit | Done | [a_failed_subscription_restore_keeps_the_connection](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_reconnect_test.c#L401) |
| | Failed suback keeps the connection | Broker refuses the filter; session stays CONNECTED. | unit | Done | [a_failed_suback_keeps_the_connection](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_reconnect_test.c#L421) |
| | Matching puback invokes the callback | — | unit | Done | [matching_puback_invokes_the_callback](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_reconnect_test.c#L477) |
| | Unmatched puback is ignored | Unknown packet id does not fire a callback. | unit | Done | [unmatched_puback_is_ignored](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_reconnect_test.c#L491) |
| | Pending pubacks completed with an error on disconnect | Caller is told to resend; the slot is released. | unit | Done | [pending_pubacks_are_completed_with_an_error_on_disconnect](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_reconnect_test.c#L508) |
| | All pending pubacks are completed | Every outstanding publish is reported, not just the first. | unit | Done | [all_pending_pubacks_are_completed_on_disconnect](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_reconnect_test.c#L533) |
| | Destroy does not complete pending pubacks | The caller's context may already be gone. | unit | Done | [destroy_does_not_complete_pending_pubacks](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_reconnect_test.c#L584) |
| | Keep alive drop is retried like any disconnect | Reaches the core as a plain DISCONNECTED. | unit | Done | [keep_alive_drop_is_retried_like_any_disconnect](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_reconnect_test.c#L161) |
| Hub-Next presence (v5) | Hub next births then connects | CONNACK → sub `dev/#` → SUBACK → birth → birth-ack → CONNECTED. | unit | Done | [hub_next_births_then_connects_on_birth_ack](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_client_test.c#L621) |
| | Hub next username carries nonce | CONNECT username `correlationId` equals the birth correlation data. | unit | Done | [hub_next_connect_username_carries_correlation_nonce](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_client_test.c#L663) |
| | Hub next birth reports session present | CONNACK session-present flag encoded in the birth payload. | unit | Done | [hub_next_birth_reports_session_present](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_client_test.c#L766) |
| | Hub next ignores mismatched birth ack | Stale nonce ignored, handshake stays in CONNECTING. | unit | Done | [hub_next_ignores_mismatched_birth_ack](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_client_test.c#L697) |
| | Hub next birth ack after close is ignored | A valid birth ack that lands after `close()` was requested must not announce CONNECTED. | unit | Done | [hub_next_birth_ack_after_close_is_ignored](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_client_test.c#L728) |
| | Hub next ignores wrong type ack | Right nonce, wrong `type` user property → ignored. | unit | Done | [hub_next_ignores_wrong_type_ack](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_client_test.c#L814) |
| | Hub next suback failure faults | `dev/#` SUBACK error → FAULTED. | unit | Done | [hub_next_suback_failure_faults](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_client_test.c#L779) |
| | Hub next birth ack timeout faults | No birth-ack within the timeout, no reconnect policy → FAULTED/TIMEOUT. | unit | Done | [hub_next_birth_ack_timeout_faults](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_client_test.c#L862) |
| | Classic skips birth handshake | v3.1.1 contrast: CONNACK → CONNECTED with no presence traffic. | unit | Done | [classic_connect_skips_birth_handshake](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_client_test.c#L842) |
| | Hub next birth publish failure faults | Adapter rejects the birth PUBLISH. | unit | Done | [hub_next_birth_publish_failure_faults](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_client_test.c#L1306) |
| | Hub next birth timeout retries with new nonce | A stale ack must not satisfy the retry. | unit | Done | [hub_next_birth_timeout_retries_with_a_new_nonce](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_client_test.c#L1385) |
| | Hub next without v5 factory not supported | No silent downgrade to a Classic session. | unit | Done | [hub_next_without_v5_factory_is_not_supported](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_client_test.c#L1285) |
| | Hub next birth ack before suback ignored | No nonce exists yet to match against. | unit | Done | [hub_next_birth_ack_before_suback_is_ignored](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_client_test.c#L1331) |
| DPS provisioning | Dps csr flow stores issued chain | DPS register with CSR → ASSIGNED → operational chain stored. | unit | Done | [dps_csr_flow_sends_csr_and_stores_issued_chain](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_client_test.c#L1003) |
| | Open rejects operational cert without csr provider | `request_operational_certificate` set but provider has no `get_csr`. | unit | Done | [open_rejects_operational_cert_without_csr_provider](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_client_test.c#L888) |
| | Open rejects operational cert without payload buffer | `request_operational_certificate` set but `csr_payload_buffer` empty. | unit | Done | [open_rejects_operational_cert_without_payload_buffer](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_client_test.c#L1151) |
| | Dps connects to the global endpoint | Default `global.azure-devices-provisioning.net`. | unit | Done | [dps_connects_to_the_global_endpoint_by_default](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_dps_test.c#L299) |
| | Dps honors a custom global endpoint | `dps.global_endpoint` override. | unit | Done | [dps_honors_a_custom_global_endpoint](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_dps_test.c#L309) |
| | Dps uses v3 1 1 even for a next hub | `hub_protocol = NEXT`; the v5 factory is left untouched. | unit | Done | [dps_uses_v3_1_1_even_when_the_hub_is_next](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_dps_test.c#L376) |
| | Dps without a v3 1 1 factory is not supported | Only a v5 factory registered. | unit | Done | [dps_without_a_v3_1_1_factory_is_not_supported](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_dps_test.c#L396) |
| | Dps subscribes the response topic | `$dps/registrations/res/#`. | unit | Done | [dps_subscribes_the_registration_response_topic](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_dps_test.c#L414) |
| | Dps registers only after the suback | Registering earlier would race the response. | unit | Done | [dps_publishes_register_only_after_the_suback](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_dps_test.c#L427) |
| | Dps polls operation status | `assigning` → GET iotdps-get-operationstatus. | unit | Done | [dps_polls_operation_status_after_an_assigning_response](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_dps_test.c#L450) |
| | Dps honors the retry after delay | No poll before the service-supplied delay elapses. | unit | Done | [dps_honors_the_retry_after_delay](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_dps_test.c#L468) |
| | Dps assignment connects to the assigned hub | CONNECT host is the assigned hub. | unit | Done | [dps_assignment_connects_to_the_assigned_hub](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_dps_test.c#L490) |
| | Dps assignment uses the assigned device id | Not the registration id. | unit | Done | [dps_assignment_uses_the_assigned_device_id](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_dps_test.c#L508) |
| | Dps session reaches connected | End of the provisioning → hub handoff. | unit | Done | [dps_session_reaches_connected_after_assignment](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_dps_test.c#L523) |
| | Dps failed status faults | `status:failed` → FAULTED / `ERR_DPS`. | unit | Done | [dps_failed_status_faults_with_a_dps_error](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_dps_test.c#L543) |
| | Dps disabled status faults | `status:disabled` → FAULTED / `ERR_DPS`. | unit | Done | [dps_disabled_status_faults_with_a_dps_error](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_dps_test.c#L556) |
| | Dps connack failure faults | Rejected at the DPS endpoint. | unit | Done | [dps_connack_failure_faults](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_dps_test.c#L569) |
| | Dps suback failure faults | SUBACK error on the response topic. | unit | Done | [dps_suback_failure_faults](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_dps_test.c#L583) |
| | Dps disconnect midflow faults | Link drops between REGISTER and ASSIGNED. | unit | Done | [dps_disconnect_midflow_faults](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_dps_test.c#L601) |
| | Dps malformed response faults | Unparsable body → FAULTED / `ERR_PROTOCOL`, body logged. | unit | Done | [dps_malformed_response_faults_with_a_protocol_error](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_dps_test.c#L620) |
| | Dps rejects a null registration id | `id_scope` set, identity missing. | unit | Done | [dps_rejects_a_null_registration_id](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_dps_test.c#L639) |
| | Dps rejects an empty registration id | Empty string, not just NULL. | unit | Done | [dps_rejects_an_empty_registration_id](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_dps_test.c#L653) |
| | Dps rejected identity leaves the client idle | No half-started session; the instance stays reusable. | unit | Done | [dps_rejected_identity_leaves_the_client_idle](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_dps_test.c#L668) |
| | Dps empty response body faults | MQTT permits an empty payload; the parse call requires a non-empty one and az_core's precondition handler does not return, so it is rejected before the call. | unit | Done | [dps_empty_response_body_faults_without_a_null_deref](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_dps_test.c#L640) |
| Adapters | Factory advertises version | Factory `version` field matches the clients it creates. | unit | Done | [factory_advertises_version](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/mqtt_iface_contract_test.c#L59) |
| | Registering the same factory twice does not grow the registry | An exact duplicate is folded away; it used to add an unreachable entry that `destroy()` freed a second time. | unit | Done | [registering_the_same_factory_twice_does_not_grow_the_registry](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_client_test.c#L155) |
| | A duplicate registration leaves the connection usable | Folding the duplicate must not drop the registration. | unit | Done | [a_duplicate_registration_leaves_the_connection_usable](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_client_test.c#L171) |
| | A distinct factory for the same version still registers | Only an exact duplicate is folded. | unit | Done | [a_distinct_factory_for_the_same_version_still_registers](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_client_test.c#L190) |
| | Client carries iface with version | All vtable slots populated on the created client. | unit | Done | [client_carries_iface_pointer_with_version](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/mqtt_iface_contract_test.c#L68) |
| | Connect failure propagates to caller | Adapter `connect()` error returned synchronously. | unit | Done | [scripted_failure_propagates_to_caller](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/mqtt_iface_contract_test.c#L118) |
| | Publish records topic payload and assigns a packet id | Publish contract: the adapter hands back the id the PUBACK will carry. | unit | Done | [publish_records_topic_payload_and_assigns_packet_id](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/mqtt_iface_contract_test.c#L88) |
| | Process loop drains one event per call | Pump contract: one CONNECTED/MESSAGE event per `process_loop()`. | unit | Done | [process_loop_drains_one_event_per_call](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/mqtt_iface_contract_test.c#L135) |
| | Destroy through the iface is recorded | Teardown contract: the core reaches `destroy()` on the vtable, not a concrete type. | unit | Done | [destroy_via_iface_is_recorded](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/mqtt_iface_contract_test.c#L170) |
| | Paho v3 factory creates v3 client | MQTT v3.1.1. | unit | Done | [v3_factory_creates_v3_client](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/paho_adapter_smoke_test.c#L19) |
| | Paho v5 factory creates v5 client | MQTT v5. | unit | Done | [v5_factory_creates_v5_client](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/paho_adapter_smoke_test.c#L48) |
| | Rust adapter install and dispatch | FFI, v5 only: install vtable, create client, dispatch events. | unit | Done | [test_install_then_factory_then_dispatch](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/rust_mqtt_adapter_test.c#L159) |
| | Rust adapter rejects partial table | Incomplete vtable rejected at install. | unit | Done | [test_install_rejects_partial_table](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/rust_mqtt_adapter_test.c#L149) |
| TLS & transport | Connect disconnect roundtrip | v3.1.1 and v5 suites. | conformance | Done | [connect_disconnect_roundtrip](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/conformance/az_iot_conformance.c#L259) |
| | Session usable after connect | Subscribe + publish + receive own message. | conformance | Done | [publish_subscribe_roundtrip](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/conformance/az_iot_conformance.c#L275) |
| | Disconnect without connect rejected | `disconnect()` on a never-connected client is not OK. | conformance | Done | [disconnect_without_connect_is_rejected](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/conformance/az_iot_conformance.c#L327) |
| | Untrusted server cert rejected | TLS broker, `verify_server = true`, wrong trust anchor → never CONNECTED. | conformance | Done | [server_cert_validation_rejects_untrusted](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/conformance/az_iot_conformance.c#L344) |
| | Connect after disconnect reuses the client | A BYO adapter must not leak state across sessions. | conformance | Done | [connect_after_disconnect_reuses_the_client](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/conformance/az_iot_conformance.c#L546) |
| | Connection refused is rejected | Closed port; never reports CONNECTED. | conformance | Done | [connect_to_a_closed_port_is_rejected](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/conformance/az_iot_conformance.c#L396) |
| | Unresolvable host is rejected | RFC 2606 `.invalid` name. | conformance | Done | [connect_to_an_unresolvable_host_is_rejected](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/conformance/az_iot_conformance.c#L426) |
| | Black-holed address never reports connected | RFC 5737 TEST-NET-3 with a 2 s `connect_timeout_seconds`. | conformance | Done | [connect_to_a_black_holed_address_never_reports_connected](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/conformance/az_iot_conformance.c#L457) |
| | Idle session survives keep alive | Idle 5 s at `keep_alive_seconds = 2`, then a live round trip. | conformance | Done | [idle_session_survives_the_keep_alive_interval](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/conformance/az_iot_conformance.c#L493) |
| | Server cert hostname mismatch rejected | Needs a TLS broker whose CA the suite trusts; only the untrusted-CA case can be told apart today. | conformance | Pending | *az_iot_conformance.c* |
| | Expired client cert rejected | Needs a broker configured for mutual TLS plus expired-leaf fixtures. | conformance | Pending | *az_iot_conformance.c* |
| | Connect with in memory ca pem | Optional adapter capability; the bundled Paho adapter is file-path only, so this cannot be a suite-wide assertion. | conformance | Pending | *az_iot_conformance.c* |
| End-to-end | Device connects via dps | X.509 individual enrollment → DPS → assigned hub, Paho v3.1.1/v5, Linux + Windows. | e2e | Done | [e2e_device_connect](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/e2e/tests/e2e_device.c#L83) |
| | Device disconnects cleanly | `close()` pumps to IDLE and releases the certificate provider. | e2e | Done | [e2e_device_disconnect](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/e2e/tests/e2e_device.c#L153) |
| | Dps csr enrollment ec | EC key, DPS-issued operational cert, then hub connect. | e2e | Done | [test_dps_csr_enrollment_ec](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/e2e/tests/e2e_csr_test.c#L222) |
| | Dps csr enrollment rsa | RSA-2048 key, DPS-issued operational cert, then hub connect. | e2e | Done | [test_dps_csr_enrollment_rsa](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/e2e/tests/e2e_csr_test.c#L228) |
| | Reconnect after service disconnect | Needs service-side device disable/enable in the e2e harness. | e2e | Pending | *e2e_scenarios_test.c* |
| | Reconnect restores feature clients | Depends on the row above for a way to force the drop. | e2e | Pending | *e2e_scenarios_test.c* |
| | Long haul connection stability | Multi-hour; belongs on a nightly schedule, not the PR gate. | e2e | Pending | *e2e (new)* |
| | Connect with wrong device id rejected | Needs an identity provisioned in DPS but absent from the hub. | e2e | Pending | *e2e_scenarios_test.c* |

## Telemetry (device-to-cloud)

Covers `az_iot_telemetry_client`: the Classic D2C topic
`devices/{device_id}/messages/events/`, the percent-encoded `{property-bag}` that carries
system and application properties, and the QoS-1 send completion. Wire format per
[Send device-to-cloud messages](https://learn.microsoft.com/azure/iot-hub/iot-mqtt-connect-to-iot-hub#send-device-to-cloud-messages).

| Group | Test | Scenario | Type | Status | Code Location |
| --- | --- | --- | --- | --- | --- |
| Init & destroy | Init rejects a null client | — | unit | Done | [init_rejects_nulls](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/telemetry_client_test.c#L105) |
| | Init rejects a null connection | — | unit | Done | [init_rejects_nulls](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/telemetry_client_test.c#L105) |
| | Init zeroes a reused instance | A stack instance must not inherit the previous client's connection. | unit | Pending | *telemetry_client_test.c* |
| | Destroy tolerates null | — | unit | Done | [init_rejects_nulls](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/telemetry_client_test.c#L105) |
| | Destroy is idempotent | — | unit | Pending | *telemetry_client_test.c* |
| | Send after destroy is refused | `destroy()` zeroes `conn`; the next send must not follow a stale pointer. | unit | Pending | *telemetry_client_test.c* |
| Send arguments | Send rejects a null client | — | unit | Done | [send_rejects_invalid_args](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/telemetry_client_test.c#L301) |
| | Send rejects a null message | — | unit | Done | [send_rejects_invalid_args](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/telemetry_client_test.c#L301) |
| | Send rejects a null payload with a non-zero length | — | unit | Done | [send_rejects_invalid_args](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/telemetry_client_test.c#L301) |
| | Send accepts a null payload with a zero length | An empty heartbeat is a valid message. | unit | Pending | *telemetry_client_test.c* |
| | Send before connect returns not connected | — | unit | Done | [send_before_connect_returns_not_connected](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/telemetry_client_test.c#L116) |
| | Send with no resolved device id returns not initialized | DPS client still provisioning. | unit | Pending | *telemetry_client_test.c* |
| Publish shape | Topic is the classic d2c topic | `devices/{device_id}/messages/events/`. | unit | Done | [send_publishes_qos1](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/telemetry_client_test.c#L130) |
| | Topic ends with a slash when there are no properties | The property bag hangs off the trailing slash, not a `?`. | unit | Done | [send_publishes_qos1](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/telemetry_client_test.c#L130) |
| | Payload is forwarded byte for byte | — | unit | Done | [send_publishes_qos1](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/telemetry_client_test.c#L130) |
| | Publish uses qos 1 | — | unit | Done | [send_publishes_qos1](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/telemetry_client_test.c#L130), [send_qos1_defers_cb_until_puback](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/telemetry_client_test.c#L156) |
| | Publish never uses qos 2 | IoT Hub closes the network connection on a QoS 2 publish. | unit | Done | [the_publish_never_uses_qos_2](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/telemetry_client_test.c#L315) |
| | Publish never sets the retain flag | IoT Hub does not persist retained messages; it turns the flag into an `mqtt-retain` application property instead. | unit | Done | [the_publish_never_sets_retain](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/telemetry_client_test.c#L349) |
| | Topic uses the device id assigned by dps | Not the registration id. | unit | Pending | *telemetry_client_test.c* |
| | Adapter publish failure returns to the caller | — | unit | Pending | *telemetry_client_test.c* |
| | Adapter publish failure fires no callback | — | unit | Pending | *telemetry_client_test.c* |
| | Nothing is published when the topic build fails | — | unit | Done | [send_reports_not_enough_space_when_the_bag_overflows](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/telemetry_client_test.c#L276) |
| Send completion | A packet id is assigned for qos 1 | — | unit | Done | [send_qos1_defers_cb_until_puback](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/telemetry_client_test.c#L156) |
| | Callback does not fire before the puback | — | unit | Done | [send_publishes_qos1](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/telemetry_client_test.c#L130), [send_qos1_defers_cb_until_puback](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/telemetry_client_test.c#L156) |
| | Callback fires with ok on the matching puback | — | unit | Done | [send_qos1_defers_cb_until_puback](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/telemetry_client_test.c#L156) |
| | Callback receives the context it was given | — | unit | Pending | *telemetry_client_test.c* |
| | A send with no callback still publishes | — | unit | Done | [send_propagates_content_type_and_properties](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/telemetry_client_test.c#L192) |
| | Callback fires with not connected when the session drops first | C-3 seen from the telemetry surface. | unit | Pending | *telemetry_client_test.c* |
| | Puback table full is reported after the publish went out | `AZ_IOT_MAX_PENDING_PUBACKS` reached; the PUBLISH is already on the wire when `NOT_SUPPORTED` comes back. | unit | Pending | *telemetry_client_test.c* |
| Property bag | A single property is appended as key equals value | — | unit | Done | [send_propagates_content_type_and_properties](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/telemetry_client_test.c#L192) |
| | Properties are separated by an ampersand | — | unit | Done | [send_propagates_content_type_and_properties](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/telemetry_client_test.c#L192) |
| | Property order is preserved | — | unit | Done | [send_propagates_content_type_and_properties](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/telemetry_client_test.c#L192) |
| | Keys are percent encoded | Keeps the reserved `$` out of the topic. | unit | Done | [send_propagates_content_type_and_properties](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/telemetry_client_test.c#L192), [send_url_encodes_property_values](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/telemetry_client_test.c#L222) |
| | Values are percent encoded | `&`, `=`, `%` and space cannot forge bag structure. | unit | Done | [send_url_encodes_property_values](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/telemetry_client_test.c#L222) |
| | RFC 3986 unreserved characters pass through | `-_.~` and alphanumerics are not escaped. | unit | Done | [send_url_encodes_property_values](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/telemetry_client_test.c#L222) |
| | Content type and encoding match the azure sdk wire form | `%24.ct`, `%24.ce`. | unit | Done | [send_propagates_content_type_and_properties](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/telemetry_client_test.c#L192) |
| | Message id and correlation id match the azure sdk wire form | `%24.mid`, `%24.cid`. | unit | Done | [send_url_encodes_property_values](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/telemetry_client_test.c#L222), [system_property_keys_match_the_azure_sdk_wire_form](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/telemetry_client_test.c#L250) |
| | User id creation time and component name match the wire form | `$.uid`, `$.ctime`, `$.sub` are declared in the public header but never asserted. | unit | Pending | *telemetry_client_test.c* |
| | A routing content type survives encoding | `application/json;charset=utf-8` → `application%2Fjson%3Bcharset%3Dutf-8`, the form IoT Hub requires to route on the message body. | unit | Done | [a_routing_content_type_survives_encoding](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/telemetry_client_test.c#L370) |
| | A property with a null value emits a bare key | The `=` is omitted. | unit | Pending | *telemetry_client_test.c* |
| | A property with an empty value emits key and equals | — | unit | Pending | *telemetry_client_test.c* |
| | A null key is skipped | — | unit | Pending | *telemetry_client_test.c* |
| | An empty key is skipped | — | unit | Pending | *telemetry_client_test.c* |
| | A skipped key leaves no dangling separator | The `&` belongs to the pair that was dropped. | unit | Pending | *telemetry_client_test.c* |
| | Bag overflow reports not enough space | Topic buffer exhausted. | unit | Done | [send_reports_not_enough_space_when_the_bag_overflows](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/telemetry_client_test.c#L276) |
| End-to-end | Telemetry reaches the built-in endpoint | Marker payload observed on the Event Hub-compatible endpoint. | e2e | Done | [test_telemetry](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/e2e/tests/e2e_scenarios_test.c#L148) |
| | System properties observed service side | Only the body is asserted today; `$.ct` / `$.mid` are never read back. | e2e | Pending | *e2e_scenarios_test.c* |
| | Application properties observed service side | — | e2e | Pending | *e2e_scenarios_test.c* |
| | Several messages in flight keep their order | — | e2e | Pending | *e2e_scenarios_test.c* |
| | A payload at the 256 kb limit is accepted | The hub measures body plus all property names and values. | e2e | Pending | *e2e_scenarios_test.c* |
| | A payload past the 256 kb limit is rejected | — | e2e | Pending | *e2e_scenarios_test.c* |

## Cloud-to-device messages

Covers `az_iot_c2d_client`: the `devices/{device_id}/messages/devicebound/#` subscription
and the dispatch of inbound messages to the application handler. Wire format per
[Receive cloud-to-device messages](https://learn.microsoft.com/azure/iot-hub/iot-mqtt-connect-to-iot-hub#receive-cloud-to-device-messages).

C2D was the last feature client with **no unit coverage at all** — its only test was the e2e
round trip, so every argument-validation, topic-build and teardown path was unexercised.
`c2d_client_test.c` now covers them; what is left `Pending` is noted per row.

| Group | Test | Scenario | Type | Status | Code Location |
| --- | --- | --- | --- | --- | --- |
| Init | Init rejects a null client | — | unit | Done | [init_rejects_a_null_client](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/c2d_client_test.c#L171) |
| | Init rejects a null connection | — | unit | Done | [init_rejects_a_null_connection](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/c2d_client_test.c#L177) |
| | Init subscribes the devicebound filter | `devices/{device_id}/messages/devicebound/#`. | unit | Done | [init_subscribes_the_devicebound_filter](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/c2d_client_test.c#L184) |
| | The subscription uses qos 1 | The hub grants at most QoS 1 regardless, but the request should say 1. | unit | Done | [the_subscription_uses_qos_1](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/c2d_client_test.c#L194) |
| | Init registers a dispatch prefix without the wildcard | The prefix stops at the trailing slash so property-bag sub-topics still route. | unit | Done | [a_message_on_a_property_bag_sub_topic_is_delivered](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/c2d_client_test.c#L413) |
| | Init prefers the dps registration id | — | unit | Done | [init_prefers_the_dps_registration_id](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/c2d_client_test.c#L232) |
| | Init falls back to the client id | No DPS registration id configured. | unit | Done | [init_falls_back_to_the_client_id](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/c2d_client_test.c#L204) |
| | Init without either id returns not initialized | — | unit | Done | [init_without_any_device_id_is_rejected](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/c2d_client_test.c#L263) |
| | Init with a device id that overflows the topic buffer returns internal | Past `AZ_IOT_C2D_TOPIC_MAX`; refused rather than truncated into another device's topic. | unit | Done | [init_with_a_device_id_that_overflows_the_topic_is_rejected](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/c2d_client_test.c#L278) |
| | A failed init leaves the client zeroed | — | unit | Done | [a_failed_init_leaves_no_handler_registered](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/c2d_client_test.c#L299) |
| | A failed subscription unregisters the handler | Needs a mock that refuses a SUBSCRIBE at registration time. | unit | Pending | *c2d_client_test.c* |
| Destroy | Destroy unregisters the inbound handler | A message after `destroy()` reaches nobody. | unit | Done | [a_message_after_destroy_reaches_nobody](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/c2d_client_test.c#L704) |
| | Destroy tolerates null | — | unit | Done | [destroy_tolerates_null](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/c2d_client_test.c#L720) |
| | Destroy is idempotent | — | unit | Done | [destroy_is_idempotent](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/c2d_client_test.c#L726) |
| Handler | Set handler rejects a null client | — | unit | Done | [set_handler_rejects_a_null_client](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/c2d_client_test.c#L325) |
| | Set handler accepts a null callback | Pauses delivery without tearing the subscription down; a later handler resumes it. | unit | Done | [clearing_the_handler_stops_delivery](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/c2d_client_test.c#L356) |
| | A later set handler replaces the earlier one | Context as well as callback. | unit | Done | [a_later_handler_replaces_the_earlier_one](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/c2d_client_test.c#L340) |
| | A message before any handler is set is dropped | — | unit | Done | [a_message_before_any_handler_is_set_is_dropped](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/c2d_client_test.c#L331) |
| Delivery | Payload and length reach the handler unchanged | — | unit | Done | [the_payload_reaches_the_handler_unchanged](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/c2d_client_test.c#L381) |
| | An empty payload is delivered | A message whose whole content is in its properties. | unit | Done | [an_empty_payload_is_delivered](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/c2d_client_test.c#L396) |
| | A message on a property bag sub-topic is delivered | Prefix match, not exact match. | unit | Done | [a_message_on_a_property_bag_sub_topic_is_delivered](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/c2d_client_test.c#L413) |
| | Content type is decoded from the property bag | `%24.ct=application%2Fjson` → `application/json`. | unit | Done | [the_content_type_is_decoded_from_the_property_bag](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/c2d_client_test.c#L446) |
| | Property keys and values arrive as plain text | The spelling the sender used: `$.mid`, not `%24.mid`. | unit | Done | [property_keys_and_values_arrive_as_plain_text](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/c2d_client_test.c#L461) |
| | The three property bag value forms are distinguished | `key` → NULL, `key=` → empty, `key=value`. | unit | Done | [the_three_property_bag_value_forms_are_distinguished](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/c2d_client_test.c#L481) |
| | A message with no property bag has no properties | — | unit | Done | [a_message_with_no_property_bag_has_no_properties](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/c2d_client_test.c#L508) |
| | A malformed escape drops the properties but keeps the message | Losing a payload over an unreadable property would be the worse trade. | unit | Done | [a_malformed_escape_drops_the_properties_but_keeps_the_message](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/c2d_client_test.c#L523) |
| | Properties past the bound are dropped and the message survives | `AZ_IOT_C2D_MAX_PROPERTIES`; logged. | unit | Done | [properties_past_the_bound_are_dropped_and_the_message_survives](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/c2d_client_test.c#L541) |
| | A property can be looked up by name | `az_iot_c2d_message_property()`. | unit | Done | [a_property_can_be_looked_up_by_name](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/c2d_client_test.c#L562) |
| | A message addressed to another device is not delivered | Different `{device_id}` in the topic. | unit | Done | [a_message_addressed_to_another_device_is_not_delivered](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/c2d_client_test.c#L429) |
| | A second client for the same identity is rejected | There is one device-bound stream per identity; the second used to go silently deaf. | unit | Done | [a_second_client_for_the_same_identity_is_rejected](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/c2d_client_test.c#L597) |
| | A client for a different identity builds a different topic | Only an exact prefix duplicate is refused. That distinct prefixes coexist in one dispatch table — what multiplexing rests on — is proved by `dispatch_allows_distinct_identities_to_coexist`. | unit | Done | [a_client_for_a_different_identity_registers_alongside](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/c2d_client_test.c#L616) |
| | A replacement client takes over delivery | Destroy then re-init re-points the stream. | unit | Done | [a_replacement_client_takes_over_delivery](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/c2d_client_test.c#L650) |
| | The subscription is reissued after a reconnect | Losing it silently loses every message sent afterwards. | unit | Done | [the_subscription_is_reissued_after_a_reconnect](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/c2d_client_test.c#L673) |
| End-to-end | Cloud to device message received | Service sends over AMQP; the device matches the marker. | e2e | Done | [test_c2d](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/e2e/tests/e2e_scenarios_test.c#L256) |
| | A message queued while offline arrives after reconnect | The Paho v3.1.1 path now honours CleanSession 0, so the hub holds what it could not deliver. | e2e | Pending | *e2e_scenarios_test.c* |
| | Application properties are delivered | — | e2e | Pending | *e2e_scenarios_test.c* |

Properties are decoded into the same plain-text shape `az_iot_telemetry_property` uses on
the way out, so a property survives a round trip unchanged. Two C2D clients for one
identity are refused at init rather than one of them going silently deaf; distinct
identities on a multiplexed connection still register side by side.

## Direct methods

Covers `az_iot_direct_method_client`: the `$iothub/methods/POST/#` subscription, parsing
`{method-name}` and `$rid` out of the invocation topic, and publishing the answer on
`$iothub/methods/res/{status}/?$rid={request-id}`. Wire format per
[Respond to a direct method](https://learn.microsoft.com/azure/iot-hub/iot-mqtt-connect-to-iot-hub#respond-to-a-direct-method).

| Group | Test | Scenario | Type | Status | Code Location |
| --- | --- | --- | --- | --- | --- |
| Init | Init rejects a null client | — | unit | Pending | *direct_method_client_test.c* |
| | Init rejects a null connection | — | unit | Pending | *direct_method_client_test.c* |
| | Init subscribes the methods filter | `$iothub/methods/POST/#`. | unit | Done | [create_subscribes_methods_topic_on_connect](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/direct_method_client_test.c#L115) |
| | The subscription uses qos 0 | — | unit | Pending | *direct_method_client_test.c* |
| | Init registers the methods dispatch prefix | — | unit | Pending | *direct_method_client_test.c* |
| | Init on a profile without a methods prefix returns not supported | — | unit | Pending | *direct_method_client_test.c* |
| | A failed subscription unregisters the handler | — | unit | Pending | *direct_method_client_test.c* |
| Destroy | Destroy unregisters the handler | — | unit | Pending | *direct_method_client_test.c* |
| | Destroy zeroes the client | — | unit | Pending | *direct_method_client_test.c* |
| | Destroy tolerates null | — | unit | Pending | *direct_method_client_test.c* |
| | Destroy is idempotent | — | unit | Pending | *direct_method_client_test.c* |
| | An invocation after destroy reaches nobody | — | unit | Pending | *direct_method_client_test.c* |
| Handler | Set handler rejects a null client | — | unit | Pending | *direct_method_client_test.c* |
| | Set handler stores the user context | — | unit | Done | [inbound_invocation_dispatched_to_handler](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/direct_method_client_test.c#L143) |
| | A later set handler replaces the earlier one | — | unit | Pending | *direct_method_client_test.c* |
| | An invocation with no handler is dropped | No pool slot is consumed. | unit | Pending | *direct_method_client_test.c* |
| Topic parsing | The method name is parsed from the topic | — | unit | Done | [inbound_invocation_dispatched_to_handler](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/direct_method_client_test.c#L143) |
| | The rid is parsed from the topic | Proven by the rid echoed on the response topic. | unit | Done | [inbound_invocation_dispatched_to_handler](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/direct_method_client_test.c#L143) |
| | The payload reaches the handler | — | unit | Done | [inbound_invocation_dispatched_to_handler](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/direct_method_client_test.c#L143) |
| | An invocation with an empty body is delivered | The service sends either valid JSON or an empty body. | unit | Pending | *direct_method_client_test.c* |
| | A topic with no rid marker is dropped | — | unit | Done | [malformed_topic_dropped](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/direct_method_client_test.c#L182) |
| | An unparsable topic says why | Dropping in silence looked like the service having stopped delivering. | unit | Done | [an_unparsable_topic_says_why](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/direct_method_client_test.c#L391) |
| | A topic with an empty rid is dropped | `?$rid=` with nothing after it. | unit | Pending | *direct_method_client_test.c* |
| | A topic with an empty method name is dropped | `$iothub/methods/POST//?$rid=1`. | unit | Pending | *direct_method_client_test.c* |
| | A topic with the wrong prefix is dropped | — | unit | Pending | *direct_method_client_test.c* |
| | A method name past the bound is dropped | Longer than `AZ_IOT_DM_METHOD_NAME_MAX`. | unit | Pending | *direct_method_client_test.c* |
| | A rid past the bound is dropped | Longer than `AZ_IOT_DM_RID_MAX`. | unit | Pending | *direct_method_client_test.c* |
| | A non-numeric rid is accepted | The service defines `$rid` as any valid message property value, not an integer. | unit | Pending | *direct_method_client_test.c* |
| Response | Respond rejects a null request | — | unit | Done | [respond_rejects_null_request](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/direct_method_client_test.c#L197) |
| | Respond rejects a null payload with a non-zero length | Rejecting the arguments does not consume the request. | unit | Done | [respond_rejects_a_null_payload_with_a_length](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/direct_method_client_test.c#L314) |
| | The response topic is the classic res topic | `$iothub/methods/res/{status}/?$rid={rid}`. | unit | Done | [inbound_invocation_dispatched_to_handler](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/direct_method_client_test.c#L143) |
| | The response rid matches the request | — | unit | Done | [inbound_invocation_dispatched_to_handler](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/direct_method_client_test.c#L143) |
| | The response status appears in the topic | — | unit | Done | [inbound_invocation_dispatched_to_handler](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/direct_method_client_test.c#L143) |
| | A non-200 status appears in the topic | 404 / 500 from the application. | unit | Done | [respond_carries_a_non_success_status](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/direct_method_client_test.c#L295) |
| | The response payload is forwarded byte for byte | — | unit | Done | [inbound_invocation_dispatched_to_handler](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/direct_method_client_test.c#L143) |
| | The response is published at qos 0 | — | unit | Done | [inbound_invocation_dispatched_to_handler](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/direct_method_client_test.c#L143) |
| | Respond with an empty payload publishes an empty body | — | unit | Pending | *direct_method_client_test.c* |
| | Respond releases the pool slot | The slot is reusable by the next invocation. | unit | Done | [responding_frees_the_slot_for_the_next_invocation](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/direct_method_client_test.c#L253) |
| | Respond twice does not publish twice | The slot may already belong to another invocation, so a second answer would carry the wrong rid. | unit | Done | [responding_twice_is_rejected](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/direct_method_client_test.c#L273) |
| | Respond after the handler returned still publishes | Async respond: the request outlives the callback. | unit | Pending | *direct_method_client_test.c* |
| | Respond while disconnected reports not connected | — | unit | Pending | *direct_method_client_test.c* |
| | Respond after destroy is refused | `destroy()` zeroes the owner the request points at. | unit | Pending | *direct_method_client_test.c* |
| | The pool holds the documented number of concurrent requests | `AZ_IOT_DM_MAX_INFLIGHT` unanswered invocations all reach the handler. | unit | Done | [the_pool_holds_the_documented_number_of_concurrent_requests](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/direct_method_client_test.c#L214) |
| | An invocation past the pool capacity is dropped | — | unit | Done | [an_invocation_past_the_pool_capacity_is_dropped](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/direct_method_client_test.c#L232) |
| | A dropped invocation says why | The warning names the pool bound and points at the missing `respond()`. | unit | Done | [a_dropped_invocation_says_why](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/direct_method_client_test.c#L366) |
| End-to-end | Direct method invoked and answered | Service invokes; the device echoes the payload with 200. | e2e | Done | [test_direct_method](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/e2e/tests/e2e_scenarios_test.c#L324) |
| | A non-success status reaches the caller | Device answers 500; the service sees it. | e2e | Pending | *e2e_scenarios_test.c* |
| | A method invoked with no payload | — | e2e | Pending | *e2e_scenarios_test.c* |
| | An unanswered method times out service side | — | e2e | Pending | *e2e_scenarios_test.c* |

**Limitation D-2:** an unanswered request still leaks its pool slot; the client now says so
instead of dropping later invocations in silence. See
[Defects and limitations](#defects-and-limitations).

## Device twin

Covers `az_iot_twin_client`: GET, PATCH reported, `$rid` correlation of
`$iothub/twin/res/{status}/?$rid={rid}` responses, and the two-pool desired-property
subscriber registry. Wire format and status codes per
[Retrieve device twin properties](https://learn.microsoft.com/azure/iot-hub/iot-mqtt-connect-to-iot-hub#retrieve-device-twin-properties).

| Group | Test | Scenario | Type | Status | Code Location |
| --- | --- | --- | --- | --- | --- |
| Init | Init rejects a null client | — | unit | Pending | *twin_client_test.c* |
| | Init rejects a null connection | — | unit | Pending | *twin_client_test.c* |
| | Init subscribes the response filter | `$iothub/twin/res/#`. | unit | Done | [create_subscribes_response_and_desired](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/twin_client_test.c#L187) |
| | Init subscribes the desired filter | `$iothub/twin/PATCH/properties/desired/#`. | unit | Done | [create_subscribes_response_and_desired](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/twin_client_test.c#L187) |
| | Both subscriptions use qos 0 | The filter is asserted today, the QoS is not. | unit | Pending | *twin_client_test.c* |
| | Init seeds the first rid at one | Rid 0 is never used. | unit | Done | [get_publishes_and_response_fires_callback](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/twin_client_test.c#L202) |
| | A failed subscription unregisters both handlers | — | unit | Pending | *twin_client_test.c* |
| Destroy | Destroy unregisters both handlers | — | unit | Pending | *twin_client_test.c* |
| | Destroy zeroes the client | — | unit | Pending | *twin_client_test.c* |
| | Destroy tolerates null | — | unit | Pending | *twin_client_test.c* |
| | Destroy is idempotent | — | unit | Pending | *twin_client_test.c* |
| Get | Get publishes the classic get topic | `$iothub/twin/GET/?$rid=<n>`. | unit | Done | [get_publishes_and_response_fires_callback](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/twin_client_test.c#L202) |
| | Get publishes an empty body | The service expects an empty message. | unit | Pending | *twin_client_test.c* |
| | Get does not fire the callback before the response | — | unit | Done | [get_publishes_and_response_fires_callback](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/twin_client_test.c#L202) |
| | A 200 response delivers the twin body | — | unit | Done | [get_publishes_and_response_fires_callback](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/twin_client_test.c#L202) |
| | Get rejects a null client | — | unit | Pending | *twin_client_test.c* |
| | Get with a full pending table is rejected | `AZ_IOT_TWIN_MAX_PENDING` in flight → `NOT_SUPPORTED`. | unit | Pending | *twin_client_test.c* |
| | A publish failure releases the pending slot | A refused PUBLISH must not leak a slot. | unit | Pending | *twin_client_test.c* |
| Patch reported | Patch publishes the classic patch topic | `$iothub/twin/PATCH/properties/reported/?$rid=<n>`. | unit | Done | [patch_publishes_and_204_response_fires_callback](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/twin_client_test.c#L228) |
| | The patch body is forwarded byte for byte | — | unit | Pending | *twin_client_test.c* |
| | A 204 response fires the ack callback | — | unit | Done | [patch_publishes_and_204_response_fires_callback](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/twin_client_test.c#L228) |
| | The reported version reaches the ack callback | `$version` rides the 204 topic and is handed to the caller. | unit | Done | [patch_publishes_and_204_response_fires_callback](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/twin_client_test.c#L228) |
| | A patch ack without a version reports zero | — | unit | Done | [a_patch_ack_without_a_version_reports_zero](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/twin_client_test.c#L256) |
| | A failed patch reports version zero | A non-2xx status must not look like an applied update. | unit | Done | [a_failed_patch_reports_version_zero](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/twin_client_test.c#L276) |
| | Patch rejects a null client | — | unit | Pending | *twin_client_test.c* |
| | Patch rejects a null patch with a non-zero length | — | unit | Pending | *twin_client_test.c* |
| | An empty patch is publishable | `patch_len = 0`. | unit | Pending | *twin_client_test.c* |
| Response correlation | An unknown rid drops the response | Stale or foreign `$rid`. | unit | Done | [unknown_rid_drops_response](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/twin_client_test.c#L322) |
| | A get response does not satisfy a patch slot | Kind is checked as well as rid. | unit | Pending | *twin_client_test.c* |
| | Concurrent get and patch correlate independently | Two rids in flight, answered out of order. | unit | Pending | *twin_client_test.c* |
| | A second response for the same rid is dropped | The slot was released by the first. | unit | Pending | *twin_client_test.c* |
| | Status 429 is reported as a distinct code | Throttling; `BUSY`, not the `NOT_SUPPORTED` a full pending table returns. | unit | Done | [throttled_status_is_reported_as_busy](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/twin_client_test.c#L349) |
| | Status 400 is reported as a distinct code | Malformed reported-properties JSON. | unit | Done | [bad_request_status_is_reported_as_invalid_arg](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/twin_client_test.c#L361) |
| | Status 404 is reported as not found | Undocumented for twin, but distinguishable. | unit | Done | [not_found_status_is_reported_as_not_found](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/twin_client_test.c#L373) |
| | Status 5xx is reported as an error | — | unit | Done | [server_error_status_is_reported_as_an_mqtt_error](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/twin_client_test.c#L382) |
| | Statuses 200 and 204 are reported as ok | — | unit | Done | [success_statuses_are_reported_as_ok](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/twin_client_test.c#L390) |
| | A response topic with a non-numeric status is dropped | — | unit | Pending | *twin_client_test.c* |
| | A response topic with no query string is dropped | — | unit | Pending | *twin_client_test.c* |
| | A response topic with no rid is dropped | — | unit | Pending | *twin_client_test.c* |
| | A response topic with the wrong prefix is dropped | — | unit | Pending | *twin_client_test.c* |
| | The rid counter wraps without reusing zero | — | unit | Pending | *twin_client_test.c* |
| | A pending get is failed when the session drops | Completed with `NOT_CONNECTED` instead of waiting for a response that died with the session. | unit | Done | [a_pending_get_is_failed_when_the_session_drops](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/twin_client_test.c#L401) |
| | A pending patch is failed when the session drops | — | unit | Done | [a_pending_patch_is_failed_when_the_session_drops](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/twin_client_test.c#L419) |
| | Every pending request is failed, not just the first | — | unit | Done | [every_pending_request_is_failed_not_just_the_first](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/twin_client_test.c#L438) |
| | The pending pool is reusable after a dropped session | The outage that used to cost a slot permanently. | unit | Done | [the_pending_pool_is_reusable_after_a_dropped_session](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/twin_client_test.c#L460) |
| | A destroyed client is not called on a later session end | `destroy()` unhooks the handler. | unit | Done | [a_destroyed_twin_client_is_not_called_on_a_later_session_end](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/twin_client_test.c#L490) |
| | Destroying the connection does not complete pending requests | Same rule as the QoS-1 acks: the caller's context may already be gone. | unit | Done | [destroying_the_connection_does_not_complete_pending_requests](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/twin_client_test.c#L511) |
| Desired properties | A desired patch reaches the subscriber | — | unit | Done | [desired_message_dispatched_to_callback](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/twin_client_test.c#L299) |
| | The version is parsed from the topic | `?$version=<v>` reaches the callback. | unit | Done | [desired_message_dispatched_to_callback](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/twin_client_test.c#L299) |
| | A desired topic with no version yields zero | — | unit | Pending | *twin_client_test.c* |
| | A version past 32 bits is preserved | The callback takes a `uint64_t`. | unit | Pending | *twin_client_test.c* |
| | Feature subscribers are notified before app subscribers | — | unit | Done | [feature_subscribers_notified_before_app](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/twin_client_test.c#L577) |
| | Every subscriber in a pool is notified | Not just the first slot. | unit | Pending | *twin_client_test.c* |
| | Subscribe rejects a null client | — | unit | Pending | *twin_client_test.c* |
| | Subscribe rejects a null callback | — | unit | Pending | *twin_client_test.c* |
| | The app pool full returns not supported | Beyond `AZ_IOT_TWIN_MAX_DESIRED_APP_SUBS`. | unit | Done | [app_pool_full_returns_not_supported](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/twin_client_test.c#L600) |
| | Resubscribing the same pair consumes one slot | — | unit | Done | [resubscribe_same_pair_is_idempotent](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/twin_client_test.c#L616) |
| | The same callback with a different context takes a second slot | The pair is the identity, not the function pointer. | unit | Pending | *twin_client_test.c* |
| | Unsubscribe stops delivery | — | unit | Done | [unsubscribe_stops_delivery](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/twin_client_test.c#L631) |
| | Unsubscribe frees the slot for reuse | — | unit | Pending | *twin_client_test.c* |
| | Unsubscribing an unregistered pair leaves the others alone | — | unit | Pending | *twin_client_test.c* |
| | Subscribing during a dispatch is busy | — | unit | Done | [mutating_registry_during_dispatch_is_busy](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/twin_client_test.c#L665) |
| | Unsubscribing during a dispatch is busy | — | unit | Done | [mutating_registry_during_dispatch_is_busy](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/twin_client_test.c#L665) |
| | The dispatch guard is cleared after a dispatch | A subscribe issued after the callback returns must succeed. | unit | Pending | *twin_client_test.c* |
| End-to-end | Desired patch observed and reported patch visible | Cloud patches desired, device observes; device patches reported, cloud reads it back. | e2e | Done | [test_twin](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/e2e/tests/e2e_scenarios_test.c#L425) |
| | Twin get returns the full document | `az_iot_twin_client_get()` is never exercised against a live hub. | e2e | Pending | *e2e_scenarios_test.c* |
| | A null member in a reported patch deletes the property | Documented service semantics. | e2e | Pending | *e2e_scenarios_test.c* |
| | Desired updates missed while offline are picked up by a get | The hub only sends change notifications to connected devices. | e2e | Pending | *e2e_scenarios_test.c* |

Pending twin requests are completed when the session ends, and the reported-properties
version the service returns is handed to the ack callback.

## File upload

Covers `az_iot_file_upload_client`: on Classic, building the two HTTPS control-plane
requests, driving them through the application's transport hook, and parsing the SAS-URI
response. The blob PUT itself is the application's job and is deliberately out of scope.

| Group | Test | Scenario | Type | Status | Code Location |
| --- | --- | --- | --- | --- | --- |
| Lifecycle | Init rejects null | — | unit | Done | [init_rejects_null](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/file_upload_client_test.c#L255) |
| | Classic init requires an http hook | — | unit | Done | [classic_init_requires_http_hook](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/file_upload_client_test.c#L263) |
| | Classic init rejects a transport with a null send | Struct present, function pointer missing. | unit | Done | [classic_init_rejects_transport_with_null_send](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/file_upload_client_test.c#L440) |
| | Init rejects an unresolved hub address | — | unit | Done | [init_rejects_unresolved_hub_address](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/file_upload_client_test.c#L450) |
| | Init rejects a missing device id | — | unit | Done | [init_rejects_missing_device_id](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/file_upload_client_test.c#L467) |
| | Failed init leaves the client unusable | No half-built client survives. | unit | Done | [failed_init_leaves_client_unusable](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/file_upload_client_test.c#L562) |
| | Calls after destroy are rejected | — | unit | Done | [calls_after_destroy_are_rejected](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/file_upload_client_test.c#L591) |
| | Destroy is null safe | — | unit | Done | [destroy_is_null_safe](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/file_upload_client_test.c#L608) |
| | Destroy is idempotent | — | unit | Done | [destroy_is_idempotent](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/file_upload_client_test.c#L616) |
| | Reinit over a live client succeeds | — | unit | Done | [reinit_over_live_client_succeeds](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/file_upload_client_test.c#L624) |
| | Destroying one client leaves the other working | — | unit | Done | [destroying_one_client_leaves_the_other_working](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/file_upload_client_test.c#L666) |
| Get sas uri | Get sas uri builds the request | Method, URL and body. | unit | Done | [get_sas_uri_builds_the_request](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/file_upload_client_test.c#L272) |
| | Get sas uri delivers the uri and correlation id | — | unit | Done | [get_sas_uri_delivers_the_sas_uri_and_correlation_id](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/file_upload_client_test.c#L295) |
| | Get sas uri http error delivers an error | Non-2xx status from the hook. | unit | Done | [get_sas_uri_http_error_delivers_error](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/file_upload_client_test.c#L317) |
| | Get sas uri transport failure delivers an error | The hook itself fails. | unit | Done | [get_sas_uri_transport_failure_delivers_error](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/file_upload_client_test.c#L329) |
| | Get sas uri rejects bad args | — | unit | Done | [get_sas_uri_rejects_bad_args](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/file_upload_client_test.c#L341) |
| | Get sas uri rejects a null client | — | unit | Done | [get_sas_uri_rejects_null_client](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/file_upload_client_test.c#L425) |
| | Get sas uri maps failure status | HTTP status → `az_iot_result`. | unit | Done | [get_sas_uri_maps_failure_status](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/file_upload_client_test.c#L1013) |
| | Get sas uri accepts any 2xx | — | unit | Done | [get_sas_uri_accepts_any_2xx](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/file_upload_client_test.c#L1064) |
| | Get sas uri is reentrant from a callback | A second request started from inside the first's callback. | unit | Done | [get_sas_uri_is_reentrant_from_callback](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/file_upload_client_test.c#L883) |
| | Get sas uri escapes the blob name | JSON escaping in the request body. | unit | Done | [get_sas_uri_escapes_blob_name](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/file_upload_client_test.c#L1501) |
| | Get sas uri oversized blob name is refused | Escaped form past `AZ_IOT_FILE_UPLOAD_BODY_MAX`. | unit | Done | [get_sas_uri_oversized_blob_name_is_refused](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/file_upload_client_test.c#L1522) |
| Notify complete | Notify complete builds the request | — | unit | Done | [notify_complete_builds_the_request](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/file_upload_client_test.c#L352) |
| | Notify complete delivers the ack | — | unit | Done | [notify_complete_delivers_the_ack](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/file_upload_client_test.c#L376) |
| | Notify complete failure body | `isSuccess:false` reported to the hub. | unit | Done | [notify_complete_failure_body](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/file_upload_client_test.c#L390) |
| | Notify complete rejects bad args | — | unit | Done | [notify_complete_rejects_bad_args](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/file_upload_client_test.c#L406) |
| | Notify complete maps failure status | — | unit | Done | [notify_complete_maps_failure_status](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/file_upload_client_test.c#L1040) |
| | Notify complete accepts any 2xx | — | unit | Done | [notify_complete_accepts_any_2xx](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/file_upload_client_test.c#L1082) |
| | Notify complete transport failure delivers an error | — | unit | Done | [notify_complete_transport_failure_delivers_error](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/file_upload_client_test.c#L1098) |
| | Notify complete oversized correlation id is refused | — | unit | Done | [notify_complete_oversized_correlation_id_is_refused](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/file_upload_client_test.c#L1537) |
| Response parsing | Malformed json reports protocol | — | unit | Done | [get_sas_uri_malformed_json_reports_protocol](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/file_upload_client_test.c#L1129) |
| | Empty response reports protocol | 2xx with no body. | unit | Done | [get_sas_uri_empty_response_reports_protocol](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/file_upload_client_test.c#L1152) |
| | Overreported body length is clamped | A hook that lies about `body_len` cannot read past the buffer. | unit | Done | [get_sas_uri_clamps_overreported_body_len](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/file_upload_client_test.c#L1168) |
| | Missing field reports protocol | — | unit | Done | [get_sas_uri_missing_field_reports_protocol](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/file_upload_client_test.c#L1185) |
| | Wrong field type reports protocol | — | unit | Done | [get_sas_uri_wrong_field_type_reports_protocol](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/file_upload_client_test.c#L1209) |
| | Unknown nested members are skipped | Forward compatibility with new response fields. | unit | Done | [get_sas_uri_skips_unknown_nested_members](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/file_upload_client_test.c#L1235) |
| | Json strings are unescaped | — | unit | Done | [get_sas_uri_unescapes_json_strings](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/file_upload_client_test.c#L1260) |
| | Unicode escape reports protocol | `\uXXXX` is not decoded; refused rather than mangled. | unit | Done | [get_sas_uri_unicode_escape_reports_protocol](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/file_upload_client_test.c#L1284) |
| | Duplicate property uses the first | — | unit | Done | [get_sas_uri_duplicate_property_uses_first](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/file_upload_client_test.c#L1296) |
| | Oversized field reports protocol | Past `AZ_IOT_FILE_UPLOAD_SAS_URI_MAX` / `_CORR_ID_MAX`. | unit | Done | [get_sas_uri_oversized_field_reports_protocol](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/file_upload_client_test.c#L1325) |
| | Truncated response reports protocol | — | unit | Done | [get_sas_uri_truncated_response_reports_protocol](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/file_upload_client_test.c#L1364) |
| | Large fields are accepted | At the documented bounds, not past them. | unit | Done | [get_sas_uri_accepts_large_fields](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/file_upload_client_test.c#L1384) |
| | A redirected response buffer is rejected | A hook that swaps `body` for its own pointer. | unit | Done | [get_sas_uri_rejects_a_redirected_response_buffer](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/file_upload_client_test.c#L1422) |
| Request shape | Sas uri request carries empty auth and json content type | X.509 mutual TLS owns authentication. | unit | Done | [sas_uri_request_carries_empty_auth_and_json_content_type](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/file_upload_client_test.c#L1443) |
| | Notification request carries empty auth and json content type | — | unit | Done | [notification_request_carries_empty_auth_and_json_content_type](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/file_upload_client_test.c#L1456) |
| | Sas uri request supplies a response buffer | — | unit | Done | [sas_uri_request_supplies_a_response_buffer](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/file_upload_client_test.c#L1470) |
| | Notification request supplies no response buffer | The hook must tolerate `body == NULL`. | unit | Done | [notification_request_supplies_no_response_buffer](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/file_upload_client_test.c#L1486) |
| | Max length endpoint still builds a url | — | unit | Done | [max_length_endpoint_still_builds_a_url](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/file_upload_client_test.c#L1556) |
| Endpoint resolution | Two clients share one connection | — | unit | Done | [two_clients_share_one_connection](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/file_upload_client_test.c#L639) |
| | Sas uri requests follow a hub reassignment | The connection stays the single source of truth. | unit | Done | [sas_uri_requests_follow_a_hub_reassignment](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/file_upload_client_test.c#L691) |
| | Notifications follow a hub reassignment | — | unit | Done | [notifications_follow_a_hub_reassignment](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/file_upload_client_test.c#L714) |
| | Requests fail while the hub address is unavailable | — | unit | Done | [requests_fail_while_the_hub_address_is_unavailable](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/file_upload_client_test.c#L750) |
| | Requests fail while the hub address is empty | — | unit | Done | [requests_fail_while_the_hub_address_is_empty](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/file_upload_client_test.c#L775) |
| | Requests fail while the device id is unavailable | — | unit | Done | [requests_fail_while_the_device_id_is_unavailable](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/file_upload_client_test.c#L791) |
| | Requests fail while the device id is empty | — | unit | Done | [requests_fail_while_the_device_id_is_empty](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/file_upload_client_test.c#L812) |
| | Requests resume when the endpoint returns | Failure is transient, not latched. | unit | Done | [requests_resume_when_the_endpoint_returns](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/file_upload_client_test.c#L832) |
| | Oversized hub address is rejected at the operation | — | unit | Done | [oversized_hub_address_is_rejected_at_the_operation](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/file_upload_client_test.c#L493) |
| | Oversized device id is rejected at the operation | — | unit | Done | [oversized_device_id_is_rejected_at_the_operation](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/file_upload_client_test.c#L527) |
| End-to-end | Upload round trip and failure reporting | Real SAS URI, real blob PUT, real completion notification. | e2e | Done | [test_upload_round_trip_and_failure_reporting](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/e2e/tests/e2e_fileupload_test.c#L425) |
| | Notify with an unknown correlation id is rejected | — | e2e | Done | [test_notify_with_unknown_correlation_id_is_rejected](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/e2e/tests/e2e_fileupload_test.c#L547) |
| | Client rejects invalid arguments | — | e2e | Done | [test_client_rejects_invalid_arguments](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/e2e/tests/e2e_fileupload_test.c#L564) |
| | Sequential uploads reuse the client | — | e2e | Done | [test_sequential_uploads_reuse_the_client](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/e2e/tests/e2e_fileupload_test.c#L587) |
| | A slow hook does not wedge the mqtt pump | The hook is called synchronously on the `do_work()` thread; the header documents the risk but nothing measures it. | unit | Pending | *file_upload_client_test.c* |

## Device update (ADU)

> **Frozen for this pass.** The ADU feature is expected to change, so the table below
> inventories what exists today — including the `Pending` rows already known — and is not
> being extended with newly identified gaps the way the other areas are. Revisit once the
> feature settles.

Covers `az_iot_adu_client`: the deployment workflow driven off desired properties, the
agent state reported back through the twin, and the manifest crypto (SHA-256 file hashes,
RS256 signature verification) in `c/adapters/adu/crypto_openssl`.

| Group | Test | Scenario | Type | Status | Code Location |
| --- | --- | --- | --- | --- | --- |
| Workflow | Init starts idle with a pending report | — | unit | Done | [init_starts_idle_and_pending_report](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/adu_client_test.c#L597) |
| | Deployment drives the full workflow | Single-step update: download → verify → install → apply. | unit | Done | [deployment_drives_full_workflow_single_step](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/adu_client_test.c#L608) |
| | Verify failure blocks download and fails | — | unit | Done | [verify_failure_blocks_download_and_fails](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/adu_client_test.c#L633) |
| | Install failure triggers rollback | — | unit | Done | [install_failure_triggers_rollback](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/adu_client_test.c#L657) |
| | Hash mismatch blocks install and fails | — | unit | Done | [hash_mismatch_blocks_install_and_fails](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/adu_client_test.c#L683) |
| | Already installed is rejected without download | No bytes fetched for a no-op deployment. | unit | Done | [already_installed_is_rejected_without_download](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/adu_client_test.c#L710) |
| | Install in progress reenters then completes | — | unit | Done | [install_in_progress_reenters_then_completes](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/adu_client_test.c#L733) |
| | Reboot required persists and resumes | State survives a restart. | unit | Done | [reboot_required_persists_and_resumes](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/adu_client_test.c#L758) |
| | Resume with no persisted state stays idle | — | unit | Done | [resume_with_no_persisted_state_stays_idle](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/adu_client_test.c#L792) |
| | Cancel action sets the cancelled flag | — | unit | Done | [cancel_action_sets_cancelled_flag](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/adu_client_test.c#L803) |
| | Multi step update runs every step in order | Only the single-step path is covered today. | unit | Pending | *adu_client_test.c* |
| | Download failure is reported and does not install | — | unit | Pending | *adu_client_test.c* |
| | Cancel during download aborts the transfer | The flag is set, but no test proves the transfer stops. | unit | Pending | *adu_client_test.c* |
| Deduplication | Duplicate redelivery is ignored | Same manifest re-sent by the service. | unit | Done | [duplicate_redelivery_is_ignored](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/adu_client_test.c#L1014) |
| | Retry with a newer timestamp restarts | — | unit | Done | [retry_with_newer_timestamp_restarts](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/adu_client_test.c#L1040) |
| | Replacement with a new id restarts | — | unit | Done | [replacement_with_new_id_restarts](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/adu_client_test.c#L1064) |
| | Retry timestamp survives resume | — | unit | Done | [retry_timestamp_survives_resume](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/adu_client_test.c#L1087) |
| | Same id with a changed manifest restarts | — | unit | Done | [same_id_changed_manifest_restarts](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/adu_client_test.c#L1119) |
| Device properties | Update device properties sets report pending | — | unit | Done | [update_device_properties_sets_report_pending](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/adu_client_test.c#L822) |
| | Custom device properties are reported | — | unit | Done | [custom_device_properties_are_reported](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/adu_client_test.c#L869) |
| | Device props too small is rejected | — | unit | Done | [device_props_too_small_is_rejected](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/adu_client_test.c#L910) |
| | Device props buffer size matches the need | The reported requirement is exact, not an estimate. | unit | Done | [device_props_buffer_size_matches_need](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/adu_client_test.c#L952) |
| | Build report with too small a buffer is rejected | `az_iot_adu_build_report()` bound. | unit | Pending | *adu_client_test.c* |
| Manifest & crypto | Microsoft root keys are embedded | The shipped roots match the published values. | unit | Done | [microsoft_root_keys_are_embedded](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/adu_client_test.c#L1143) |
| | Sha256 oneshot matches a known vector | — | unit | Done | [sha256_oneshot_matches_known_vector](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/adu_crypto_openssl_test.c#L37) |
| | Sha256 incremental matches a known vector | Streaming a large file in chunks. | unit | Done | [sha256_incremental_matches_known_vector](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/adu_crypto_openssl_test.c#L47) |
| | Verify rs256 accepts a valid signature | — | unit | Done | [verify_rs256_accepts_valid_signature](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/adu_crypto_openssl_test.c#L100) |
| | Verify rs256 rejects a tampered signature | — | unit | Done | [verify_rs256_rejects_tampered_signature](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/adu_crypto_openssl_test.c#L118) |
| | Verify rs256 rejects modified data | — | unit | Done | [verify_rs256_rejects_modified_data](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/adu_crypto_openssl_test.c#L137) |
| | Manifest signed by an unknown root key is rejected | End-to-end through `az_iot_adu_parse_update_request()`. | unit | Pending | *adu_client_test.c* |
| | Malformed jws is rejected | Wrong segment count, bad base64url, missing header. | unit | Pending | *adu_client_test.c* |
| | Malformed manifest json is rejected | — | unit | Pending | *adu_client_test.c* |
| | Verify file hash rejects an unsupported algorithm | — | unit | Pending | *adu_client_test.c* |
| End-to-end | Agent state report | Device reports its ADU agent state through the twin. | e2e | Done | [test_adu_agent_state_report](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/e2e/tests/e2e_adu_test.c#L707) |
| | Update deployment | Real deployment driven from the service. | e2e | Done | [test_adu_update_deployment](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/e2e/tests/e2e_adu_test.c#L744) |
| | Install failure rollback | — | e2e | Done | [test_adu_install_failure_rollback](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/e2e/tests/e2e_adu_test.c#L773) |
| | Verify rejects the deployment | — | e2e | Done | [test_adu_verify_rejects_deployment](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/e2e/tests/e2e_adu_test.c#L803) |
| | Already installed is a noop | — | e2e | Done | [test_adu_already_installed_noop](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/e2e/tests/e2e_adu_test.c#L834) |
| | Cancelled deployment reported service side | — | e2e | Pending | *e2e_adu_test.c* |

## Certificate management

Covers `az_iot_certificate_provider` and its two bundled implementations (file-path PEM,
and the managed provider that generates a key, emits a CSR and stores the issued chain),
plus the two issuance paths: CSR-in-DPS-registration, and runtime renewal against the
connected hub over `$iothub/credentials/...` per
[Renew a device certificate](https://learn.microsoft.com/azure/iot-hub/iot-mqtt-connect-to-iot-hub#renew-a-device-certificate-operational-certificate).

| Group | Test | Scenario | Type | Status | Code Location |
| --- | --- | --- | --- | --- | --- |
| PEM provider | Options default is zeroed | — | unit | Done | [test_options_default_is_zeroed](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/certificate_provider_pem_test.c#L93) |
| | Create rejects a null provider | — | unit | Done | [test_create_rejects_null](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/certificate_provider_pem_test.c#L103) |
| | Create rejects null options | — | unit | Done | [test_create_rejects_null](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/certificate_provider_pem_test.c#L103) |
| | Create rejects a missing certificate path | — | unit | Done | [test_create_rejects_missing_required_paths](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/certificate_provider_pem_test.c#L112) |
| | Create rejects a missing key path | — | unit | Done | [test_create_rejects_missing_required_paths](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/certificate_provider_pem_test.c#L112) |
| | Create fails on a file that does not exist | — | unit | Done | [test_create_fails_on_missing_file](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/certificate_provider_pem_test.c#L127) |
| | Load returns the certificate contents | — | unit | Done | [test_load_returns_file_contents](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/certificate_provider_pem_test.c#L138) |
| | Load returns the key and ca contents | — | unit | Done | [test_load_returns_file_contents](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/certificate_provider_pem_test.c#L138) |
| | Load without the optional fields | No CA path, no key password. | unit | Done | [test_load_without_optional_fields](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/certificate_provider_pem_test.c#L178) |
| | Load of an unreadable file is reported | Permissions, not absence. | unit | Pending | *certificate_provider_pem_test.c* |
| | Load of an operational credential with none stored returns not found | The connect path falls back to BOOTSTRAP on this code. | unit | Pending | *certificate_provider_pem_test.c* |
| | Destroy tolerates null | — | unit | Pending | *certificate_provider_pem_test.c* |
| | Destroy is idempotent | — | unit | Pending | *certificate_provider_pem_test.c* |
| Managed provider | Init generates a key | — | unit | Done | [managed_init_generates_key_and_valid_csr](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/certificate_provider_managed_test.c#L90) |
| | Init emits a parseable csr | — | unit | Done | [managed_init_generates_key_and_valid_csr](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/certificate_provider_managed_test.c#L90) |
| | The csr subject carries the registration id | — | unit | Done | [managed_init_generates_key_and_valid_csr](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/certificate_provider_managed_test.c#L90) |
| | Init rejects bad args | — | unit | Done | [managed_init_rejects_bad_args](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/certificate_provider_managed_test.c#L206) |
| | Store persists the issued chain | — | unit | Done | [managed_store_persists_and_survives_restart](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/certificate_provider_managed_test.c#L151) |
| | The persisted chain reloads after a restart | — | unit | Done | [managed_store_persists_and_survives_restart](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/certificate_provider_managed_test.c#L151) |
| | A csr buffer that is too small is reported | — | unit | Pending | *certificate_provider_managed_test.c* |
| | Storing a malformed chain is rejected | Not PEM, or a leaf that does not match the generated key. | unit | Pending | *certificate_provider_managed_test.c* |
| | Store overwrites a previously issued chain | Renewal replaces rather than appends. | unit | Pending | *certificate_provider_managed_test.c* |
| | The sign hook is exercised | `az_iot_certificate_provider.sign` has no test on either provider. | unit | Pending | *certificate_provider_managed_test.c* |
| Issuance via dps | The csr rides the dps registration body | — | unit | Done | [dps_csr_flow_sends_csr_and_stores_issued_chain](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_client_test.c#L1003) |
| | The issued chain is handed to the provider | — | unit | Done | [dps_csr_flow_sends_csr_and_stores_issued_chain](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_client_test.c#L1003) |
| | Open is refused without a csr-capable provider | — | unit | Done | [open_rejects_operational_cert_without_csr_provider](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_client_test.c#L888) |
| | Open is refused without a payload buffer | — | unit | Done | [open_rejects_operational_cert_without_payload_buffer](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_client_test.c#L1151) |
| | The dps username carries the csr api version | The CSR flow needs a newer api-version than the azure-sdk-for-c default. | unit | Pending | *connection_client_test.c* |
| | The operational cert callback fires with the issued chain | `az_iot_connection_client_set_operational_cert_callback()` has no test. | unit | Pending | *connection_client_test.c* |
| Hub-side renewal | A renewal publishes the issue-certificate topic | `$iothub/credentials/POST/issueCertificate/?$rid=<id>`. | unit | Done | [send_csr_two_phase_delivers_issued_chain](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_client_test.c#L1172) |
| | A 202 reports accepted without completing | — | unit | Done | [send_csr_two_phase_delivers_issued_chain](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_client_test.c#L1172) |
| | A 200 delivers the issued chain | — | unit | Done | [send_csr_two_phase_delivers_issued_chain](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_client_test.c#L1172) |
| | An error status reports the service code | 409 conflict with an `errorCode` body. | unit | Done | [send_csr_error_reports_service_code](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_client_test.c#L1230) |
| | Only one renewal is in flight at a time | A second `send_csr()` is refused while one is open. | unit | Done | [send_csr_cancel_frees_slot](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_client_test.c#L1255) |
| | Cancel frees the slot for a new renewal | — | unit | Done | [send_csr_cancel_frees_slot](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_client_test.c#L1255) |
| | The renewal subscribes the credentials response filter | `$iothub/credentials/res/#`. | unit | Pending | *connection_client_test.c* |
| | The request body carries the device id and csr | — | unit | Pending | *connection_client_test.c* |
| | The optional replace field is emitted only when supplied | `"replace":"*"`. | unit | Pending | *connection_client_test.c* |
| | A response for a different rid is ignored | — | unit | Pending | *connection_client_test.c* |
| | A 400 reports the service code | Invalid request payload. | unit | Pending | *connection_client_test.c* |
| | A 412 reports the service code | No matching request to replace. | unit | Pending | *connection_client_test.c* |
| | A 429 surfaces the retry-after hint | The response body carries `retryAfter`. | unit | Pending | *connection_client_test.c* |
| | A malformed issued chain reports a protocol error | 200 whose body has no `certificates` array. | unit | Pending | *connection_client_test.c* |
| | A renewal with no response times out | `CSR_OP_TIMEOUT_MS` elapses. | unit | Pending | *connection_client_test.c* |
| | A 202 extends the timeout deadline | A slow-but-alive signer is not abandoned. | unit | Pending | *connection_client_test.c* |
| | Send csr while disconnected is refused | — | unit | Pending | *connection_client_test.c* |
| | A renewal open across a reconnect is completed or failed | Same family as C-3 / D-3. | unit | Pending | *connection_client_test.c* |
| | A csr larger than the service cap is refused | `CSR_MAX_BASE64` is 8 KB. | unit | Pending | *connection_client_test.c* |
| End-to-end | Dps csr enrollment with an ec key | — | e2e | Done | [test_dps_csr_enrollment_ec](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/e2e/tests/e2e_csr_test.c#L222) |
| | Dps csr enrollment with an rsa key | — | e2e | Done | [test_dps_csr_enrollment_rsa](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/e2e/tests/e2e_csr_test.c#L228) |
| | Hub-side renewal against a live hub | `az_iot_connection_client_send_csr()` is unit-tested only. | e2e | Pending | *e2e_csr_test.c* |
| | A reconnect on the renewed certificate | The renewed chain is actually usable to authenticate. | e2e | Pending | *e2e_csr_test.c* |

## Core primitives

Shared infrastructure every feature client sits on. These have no service surface of their
own, so they are unit-tested only.

| Group | Test | Scenario | Type | Status | Code Location |
| --- | --- | --- | --- | --- | --- |
| Span writer | Empty destination latches failure | — | unit | Done | [empty_destination_latches_failure](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/span_writer_test.c#L26) |
| | Builds a topic that fits exactly | — | unit | Done | [builds_a_topic_that_fits_exactly](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/span_writer_test.c#L35) |
| | Content fitting without room for the terminator fails | — | unit | Done | [content_fitting_without_room_for_terminator_fails](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/span_writer_test.c#L52) |
| | Overflow leaves an empty string not a partial one | No truncated topic can reach the wire. | unit | Done | [overflow_leaves_an_empty_string_not_a_partial_one](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/span_writer_test.c#L70) |
| | First failure is latched | Later appends cannot clear it. | unit | Done | [first_failure_is_latched](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/span_writer_test.c#L85) |
| | Null string is reported rather than undefined | Unlike `printf("%s", NULL)`. | unit | Done | [null_string_is_reported_rather_than_undefined](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/span_writer_test.c#L99) |
| | Empty appends are no ops | — | unit | Done | [empty_appends_are_no_ops](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/span_writer_test.c#L112) |
| | Decimal matches snprintf at the extremes | — | unit | Done | [decimal_matches_snprintf_at_the_extremes](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/span_writer_test.c#L127) |
| | Hex pads clamps and widens | — | unit | Done | [hex_pads_clamps_and_widens](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/span_writer_test.c#L160) |
| | A number that does not fit writes nothing | — | unit | Done | [a_number_that_does_not_fit_writes_nothing](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/span_writer_test.c#L187) |
| | Matches snprintf for a real url | — | unit | Done | [matches_snprintf_for_a_real_url](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/span_writer_test.c#L200) |
| | Matches snprintf for a mixed topic | — | unit | Done | [matches_snprintf_for_a_mixed_topic](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/span_writer_test.c#L232) |
| | Null writer is rejected | — | unit | Done | [null_writer_is_rejected](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/span_writer_test.c#L250) |
| | Appends spans and bytes | — | unit | Done | [appends_spans_and_bytes](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/span_writer_test.c#L262) |
| | Url encoding follows rfc3986 unreserved | — | unit | Done | [url_encoding_follows_rfc3986_unreserved](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/span_writer_test.c#L277) |
| | Url encoding is all or nothing on overflow | No half-encoded escape survives. | unit | Done | [url_encoding_is_all_or_nothing_on_overflow](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/span_writer_test.c#L307) |
| | Url encoding rejects null | — | unit | Done | [url_encoding_rejects_null](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/span_writer_test.c#L326) |
| | Build str concatenates parts | — | unit | Done | [build_str_concatenates_parts](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/span_writer_test.c#L460) |
| | Build str reports its failures | — | unit | Done | [build_str_reports_its_failures](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/span_writer_test.c#L473) |
| Dispatch & profile | Profile for classic role is v3 | — | unit | Done | [profile_for_classic_role_is_v3](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/protocol_profile_dispatch_test.c#L26) |
| | Profile for dps role is classic | — | unit | Done | [profile_for_dps_role_is_classic](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/protocol_profile_dispatch_test.c#L40) |
| | Profile for next role is a stub | — | unit | Done | [profile_for_next_role_is_stub_null](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/protocol_profile_dispatch_test.c#L48) |
| | Route returns false when no match | — | unit | Done | [dispatch_route_returns_false_when_no_match](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/protocol_profile_dispatch_test.c#L84) |
| | Routes to the matching prefix | — | unit | Done | [dispatch_routes_to_matching_prefix](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/protocol_profile_dispatch_test.c#L95) |
| | Longest prefix wins | Twin response vs twin desired share a stem. | unit | Done | [dispatch_longest_prefix_wins](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/protocol_profile_dispatch_test.c#L122) |
| | Unregister by ctx removes all owned | One feature client's teardown leaves the others intact. | unit | Done | [dispatch_unregister_by_ctx_removes_all_owned](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/protocol_profile_dispatch_test.c#L150) |
| | Register rejects when full | — | unit | Done | [dispatch_register_rejects_when_full](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/protocol_profile_dispatch_test.c#L174) |
| | Register rejects a duplicate prefix | A second handler on the same prefix could never be reached. | unit | Done | [dispatch_register_rejects_a_duplicate_prefix](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/protocol_profile_dispatch_test.c#L206) |
| | Distinct identities coexist | Exact-match only, so multiplexed identities register side by side. | unit | Done | [dispatch_allows_distinct_identities_to_coexist](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/protocol_profile_dispatch_test.c#L232) |
| | A prefix frees up when its owner unregisters | — | unit | Done | [dispatch_frees_a_prefix_when_its_owner_unregisters](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/protocol_profile_dispatch_test.c#L256) |
| | Register validates args | — | unit | Done | [dispatch_register_validates_args](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/protocol_profile_dispatch_test.c#L193) |
| | Inbound message routes through dispatch | The connection hands MESSAGE events to the table. | unit | Done | [inbound_message_routes_through_dispatch](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_client_test.c#L491) |
| | Unmatched inbound topic is dropped quietly | No handler owns the prefix. | unit | Pending | *protocol_profile_dispatch_test.c* |
| Logging | Nothing is emitted without a sink | — | unit | Done | [nothing_is_emitted_without_a_sink](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/log_test.c#L77) |
| | Plain and formatted messages reach the sink | — | unit | Done | [plain_and_formatted_messages_reach_the_sink](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/log_test.c#L89) |
| | Every level is routed | — | unit | Done | [every_level_is_routed](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/log_test.c#L105) |
| | Levels below the minimum are dropped | — | unit | Done | [levels_below_the_minimum_are_dropped](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/log_test.c#L121) |
| | An oversized message is truncated not dropped | — | unit | Done | [an_oversized_message_is_truncated_not_dropped](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/log_test.c#L142) |
| | A precision bounded argument is not over read | `%.*s` on a non-terminated buffer. | unit | Done | [a_precision_bounded_argument_is_not_over_read](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/log_test.c#L156) |
| | Null message and file reach the sink as text | — | unit | Done | [null_message_and_file_reach_the_sink_as_text](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/log_test.c#L166) |
| Result & version | Result to string known codes | A sample of the enum. | unit | Done | [result_to_string_known_codes](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/smoke_test.c#L30) |
| | Result to string covers every code | No enumerator falls through to the unknown string. | unit | Pending | *smoke_test.c* |
| | Version string matches the header macros | `az_iot_version_string()` vs `AZ_IOT_VERSION_STRING`. | unit | Pending | *smoke_test.c* |

## IoT Hub Classic protocol conformance

Checked against [Use MQTT to communicate with Azure IoT Hub](https://learn.microsoft.com/azure/iot-hub/iot-mqtt-connect-to-iot-hub)
and [Understand message format](https://learn.microsoft.com/azure/iot-hub/iot-hub-devguide-messages-construct)
(retrieved 2026-08-03). **SDK** is about the shipping library, not the tests; **Tests** says
whether the behaviour is pinned anywhere. A requirement with `Implemented` + `none` is the
riskiest kind of row: it works today and nothing would notice if it stopped.

### Connect and transport

| Spec requirement | SDK | Tests |
| --- | --- | --- |
| MQTT v3.1.1 over TLS on port 8883 | Implemented — `port` defaults to 8883; the Paho adapter builds an `ssl://` URI when TLS material is configured. | Connection → TLS & transport |
| MQTT over WebSockets on port 443 | **Not implemented** — `build_server_uri()` emits only `ssl://` or `tcp://`. Devices behind a firewall that blocks 8883 cannot connect. | — |
| TLS is mandatory; port 1883 unsupported | Partial — the adapter falls back to `tcp://`+1883 when no TLS material is set. That path exists for the local conformance broker, but nothing prevents it being aimed at a hub. | — |
| ClientId is the device id | Implemented. | [open_connects_to_the_configured_endpoint](https://github.com/Azure/azure-iot-sdk/blob/main/c/tests/unit/connection_lifecycle_test.c#L172) |
| Username is `{host}/{device-id}/?api-version=…` | Implemented — delegated to `az_iot_hub_client_get_user_name()`. | Done |
| X.509 client authentication | Implemented via `az_iot_certificate_provider`. | Certificate management; e2e |
| SAS token password | Not implemented, by design — this SDK authenticates with X.509 only and never sets `password`. | — |
| Plug and Play model id announced at connect | Implemented — `opts.model_id` → `az_iot_hub_client_options.model_id`. Required for ADU to discover the device. | Done |
| CleanSession 0 so subscriptions persist | Implemented — the core leaves `clean_start` false and the Paho adapter now honours it on v3.1.1 as well as v5. It previously hardcoded `cleansession = 1` there, so Classic sessions were clean and queued messages were lost on every reconnect. | Done |
| Client keep-alive is configurable | Implemented — `opts.keep_alive_seconds`, defaulting to `AZ_IOT_DEFAULT_KEEP_ALIVE_SECONDS`. | Done |
| Connect timeout is configurable | Implemented — `opts.connect_timeout_seconds`, defaulting to `AZ_IOT_DEFAULT_CONNECT_TIMEOUT_SECONDS`. | Done |
| A second connection for the same device id evicts the first | Service behaviour; surfaces to the SDK as a plain DISCONNECT. Also the cheapest way to force the drop the reconnect e2e rows need. | Connection → End-to-end (pending) |
| Will message published as telemetry on disconnect | **Not exposed** — `az_iot_mqtt_connect_options` does carry an `lwt` section and the Paho v5 path wires it to Paho's will options, but `az_iot_connection_client` never populates it and the v3.1.1 path ignores it, so no application can set a Will. The plumbing exists; the surface does not. | — |
| Module identity (`{device-id}/{module-id}`) | **Not implemented** — no module option; every topic the SDK builds is device-scoped. | — |
| A device may subscribe to at most five topics | Partial — `AZ_IOT_MAX_PERSISTENT_SUBS` defaults to 8, so the SDK accepts filters the hub will refuse (D-6). A device using C2D + methods + twin (×2) + credential renewal is already at 5. | — |
| QoS 2 publish closes the connection | Implemented — every publish is QoS 0 or QoS 1. | Done |
| RETAIN is turned into an `mqtt-retain` property | Implemented — every publish sets `retain = false`. | Done |

### Device-to-cloud

| Spec requirement | SDK | Tests |
| --- | --- | --- |
| Topic `devices/{device-id}/messages/events/` | Implemented. | Done |
| `{property-bag}` in HTTPS query-string encoding | Implemented — both halves percent-encoded. | Done |
| System properties `$.ct` `$.ce` `$.mid` `$.cid` `$.uid` `$.ctime` `$.sub` | Implemented — all seven are public constants. | Partial — only `$.ct` `$.ce` `$.mid` `$.cid` are asserted |
| `$.ct=application/json;charset=utf-8` enables body-based routing | Implemented — the caller supplies it as an ordinary property. | Done |
| 256 KB maximum message size | Not enforced client-side; the hub rejects. | — |
| Device-to-cloud batching | **Not implemented.** | — |

### Cloud-to-device

| Spec requirement | SDK | Tests |
| --- | --- | --- |
| Subscribe `devices/{device-id}/messages/devicebound/#` | Implemented. | Done |
| Deliver the `{property-bag}` (system + application properties) | Implemented — decoded into `az_iot_c2d_message.properties` as plain text. | Done |
| The three property-bag value forms (`key`, `key=`, `key=value`) | Implemented — a null value yields `NULL`, an empty one yields `""`. | Done |
| Reject / abandon is unavailable on MQTT | Matches the API — there is no settle call to misuse. | — |

### Device twin

| Spec requirement | SDK | Tests |
| --- | --- | --- |
| Subscribe `$iothub/twin/res/#` | Implemented. | Done |
| GET `$iothub/twin/GET/?$rid={request-id}` with an empty body | Implemented. | Done (topic); body untested |
| PATCH `$iothub/twin/PATCH/properties/reported/?$rid={request-id}` | Implemented. | Done |
| Desired notification `$iothub/twin/PATCH/properties/desired/?$version={v}` | Implemented, version parsed. | Done |
| GET status 200 | Implemented. | Done |
| PATCH status 204 | Implemented. | Done |
| PATCH status 400 (malformed JSON) | Implemented — reported as `INVALID_ARG`. | Done |
| Status 429 (throttled) | Implemented — reported as `BUSY`, distinct from a locally full pending table. | Done |
| Status 5xx | Implemented. | Done |
| `$version` on the reported-properties ack | Implemented — delivered to the ack callback. | Done |
| A `null` member deletes the property | Service-side semantics; the SDK forwards the body verbatim. | — |

### Direct methods

| Spec requirement | SDK | Tests |
| --- | --- | --- |
| Subscribe `$iothub/methods/POST/#` | Implemented. | Done |
| Parse `{method-name}` and `$rid` from the request topic | Implemented. | Done |
| Respond on `$iothub/methods/res/{status}/?$rid={request-id}` | Implemented. | Done |
| `$rid` may be any valid message property value | Implemented — kept as an opaque string, not parsed as an integer. | **none** |
| `status` must be an integer | Implemented. | Done |
| Request and response bodies are valid JSON or empty | Caller's responsibility; the SDK forwards bytes. | — |

### File upload

| Spec requirement | SDK | Tests |
| --- | --- | --- |
| `POST /devices/{deviceId}/files?api-version=…` for the SAS URI | Implemented. | Done |
| `POST /devices/{deviceId}/files/notifications?api-version=…` for completion | Implemented. | Done |
| Blob PUT to the returned SAS URI | Out of scope by design — the application's HTTP client. | e2e |

### Operational certificate renewal

| Spec requirement | SDK | Tests |
| --- | --- | --- |
| Publish `$iothub/credentials/POST/issueCertificate/?$rid={request-id}` | Implemented. | Done |
| Request body carries `id`, `csr` and optional `replace` | Implemented. | **none** |
| Subscribe the response filter | Implemented as `$iothub/credentials/res/#` (plural). **The Learn article is internally inconsistent** — it says subscribe `$iothub/credential/#` and receive on `$iothub/credential/res/{status}` (singular) while publishing to `$iothub/credentials/…` (plural). Worth confirming against the service before treating either spelling as authoritative. | — |
| Status 200 (issued) and 202 (accepted) | Implemented, two-phase. | Done |
| Status 409 (conflict) | Implemented. | Done |
| Status 400 / 412 / 429 / 5xx | Implemented generically; `errorCode` and `retryAfter` are parsed. | **none** |

## Hub-Next / AEG, deferred

Not enumerated yet, by decision — this pass covers IoT Hub Classic. The Hub-Next (MQTT v5)
paths that already ship tests are listed in place: the presence handshake and the v5 CONNACK
mapping under [Connection](#connection), and the `NOT_SUPPORTED` contract of the Next file
upload control plane under [File upload](#file-upload). Everything else on that surface —
`ih/{device_id}/…` topics for telemetry, C2D, methods and twin, MQTT v5 user properties and
correlation data, and the AEG Files message schema — gets its own pass.

## Connection: not implemented, and why

The `Pending` rows in the [Connection](#connection) table are blocked on infrastructure or
on a decision, not on effort — each entry below says what specifically is missing, so the
cost of closing it is visible rather than implied. The `Pending` rows in the feature-area
tables above are not: those are tests that simply have not been written yet.

### TLS cases that need broker-side fixtures

The conformance suite talks to whatever broker `AZ_IOT_MQTT_BROKER_HOST` names. It can
supply its own *client* material and its own trust anchors, but it cannot reconfigure
the broker, and CI runs a plain mosquitto service container with no TLS listener. Note
this already affects a **shipping** test: `server_cert_validation_rejects_untrusted`
exists and passes locally, but skips in CI for exactly this reason.

| Row | What is missing |
| --- | --- |
| Server cert hostname mismatch rejected | A TLS listener **whose CA the suite trusts**, reachable under a name the certificate does not cover. Without a trusted CA the handshake fails for the wrong reason and the test proves nothing — it would pass even if hostname verification were disabled. |
| Expired client cert rejected | A listener configured for mutual TLS (`require_certificate true`), plus generated expired-leaf fixtures. The assertion belongs to the broker's verification, so a broker that does not ask for a client certificate cannot exercise it. |

**To close both:** add a TLS listener to the CI broker with a generated CA/server/client
set and export `AZ_IOT_MQTT_BROKER_TLS_HOST` / `_TLS_PORT` plus the CA path. That is a CI
and fixture change rather than a test change, which is why it is not in this series.

### Adapter capability, not SDK behaviour

| Row | What is missing |
| --- | --- |
| Connect with in memory ca pem | `az_iot_mqtt_tls_options` exposes both file-path and in-memory PEM fields, and adapters are free to support either. The bundled Paho adapter is file-path only (it hands `trustStore`/`keyStore` to OpenSSL). A conformance test asserting in-memory PEM works would therefore fail for a *conforming* adapter. Making it testable needs a capability flag on the iface so the suite can skip adapters that do not advertise support — a public API change that should be decided on its own merits. |

### End-to-end

| Row | What is missing |
| --- | --- |
| Reconnect after service disconnect | A way to force a server-initiated drop. The e2e harness has no service-side device disable/enable, and `e2e_device` connects with no reconnection policy, so there is nothing to recover. Two viable designs: (a) add disable/enable to the service half, or (b) open a second connection with the same device identity — IoT Hub evicts the first — which is deterministic and needs no new service API. Either way `e2e_device_connect()` must configure a reconnection policy first. |
| Reconnect restores feature clients | Depends entirely on the row above for a way to force the drop; the assertions themselves (twin/method/C2D after recovery) are straightforward once it exists. |
| Long haul connection stability | Multi-hour runtime. This does not belong on a PR gate at all — it needs a scheduled job with its own timeout budget and leak/reconnect-counter reporting. |
| Connect with wrong device id rejected | An identity provisioned in DPS but absent from (or disabled in) the hub, created and torn down by the provisioning step. The e2e resource provisioning is [downloaded at runtime from `Azure/iot-sdks-e2e-fx`](https://github.com/Azure/iot-sdks-e2e-fx), so the fixture change lands in that repository, not this one. |


## Defects and limitations

`C-*` were found while building the Connection coverage; all five are fixed, and the rows
that used to pin the shipping behaviour now assert the corrected behaviour instead. `D-*`
were found while surveying the feature clients for this pass and are **open** — each has a
`Pending` row that will pin the current behaviour, and closing it is an API or design
decision rather than a test.

| Id | Severity | Status | Finding |
| --- | --- | --- | --- |
| C-1 | Medium | **Fixed** | `close()` during CONNECTING was overridden by a CONNACK already in flight: the CONNACK success branch called `announce_connected()` without consulting `user_close` (it was only checked on the failure branch), so the app was told CONNECTED after it asked to close, and anything publishing on CONNECTED wrote into a socket being torn down. The success branch now returns early when `user_close` is set or the state has already left CONNECTING. |
| C-2 | Medium | **Fixed** | `AZ_IOT_ERR_IDENTITY_REJECTED` had no special handling. `az_iot_mqtt_iface.h` documents it as the code that "makes the SDK re-provision through DPS instead of retrying an identity the broker has already refused", and the CONNACK mapping faithfully produced it — but `connection_client.c` never read it, so a device with a revoked identity retried the refused credential until `max_attempts`. A hub CONNACK carrying `IDENTITY_REJECTED` on a DPS-provisioned client now arms `reprovision_pending`, and the scheduled retry runs `dps_start()` instead of reconnecting to the rejected assignment. The retry still goes through the reconnection policy, so backoff and `max_attempts` continue to bound it. |
| C-3 | Low | **Fixed** | In-flight QoS-1 publish callbacks were dropped, not completed, when a session ended: `teardown_active()` cleared every `pending_pubacks` slot without invoking the callbacks, so an app tracking outstanding publishes leaked one entry per unacknowledged publish on every reconnect and was never told to resend. They are now completed with `AZ_IOT_ERR_NOT_CONNECTED`, slot released first so a callback that republishes immediately can claim it. `destroy()` deliberately stays silent — the caller is tearing the client down and the context those callbacks close over may already be gone. |
| C-4 | Low | **Fixed** | An unparsable DPS registration response was skipped silently — no fault, no re-poll, no diagnostic — leaving the client in CONNECTING indistinguishably from a hang. It now fails the provisioning attempt with `AZ_IOT_ERR_PROTOCOL` and logs the body. |
| C-5 | **High** | **Fixed** | A NULL or empty `dps.registration_id` (with a valid `dps.id_scope`) made `az_iot_connection_client_open()` hang forever: `dps_configured()` checks only `id_scope`, so the empty value reached `az_iot_provisioning_client_init()`, and this build has `AZ_NO_PRECONDITION_CHECKING=OFF` with no precondition handler installed — az_core's default is an infinite `while(1)` loop. `dps_start()` now validates both `id_scope` and `registration_id` up front and returns `AZ_IOT_ERR_INVALID_ARG`. |
| D-1 | Medium | **Fixed** | Classic C2D dropped every message property. `on_c2d_classic()` passed `content_type = NULL` unconditionally and never parsed the topic property bag, and the application could not recover them itself because the callback was not given the topic. The handler now receives an `az_iot_c2d_message` -- payload, `content_type`, and a decoded `properties` array shaped exactly like the `az_iot_telemetry_property` array on the send side, so a property survives a round trip unchanged. The three service-defined value forms (`key`, `key=`, `key=value`) stay distinguishable, and `az_iot_c2d_message_property()` looks one up by name. Bounded by `AZ_IOT_C2D_MAX_PROPERTIES` and `AZ_IOT_C2D_PROPERTY_BUFFER`, both stack-allocated for the duration of the callback, so there is no per-client cost; overflow or a malformed escape drops the properties and logs, but still delivers the message. |
| D-2 | Medium | **Partly fixed** | A direct-method request slot is released only by `az_iot_direct_method_respond()`. A handler that returns without responding -- including on its own error paths -- leaks the slot; after `AZ_IOT_DM_MAX_INFLIGHT` (default 4) such requests every further invocation was dropped **silently**, which is indistinguishable from the service having stopped delivering. Two things are fixed: a dropped invocation now logs a warning naming the bound and the missing `respond()` call, and an unparsable request topic logs the topic it rejected. Responding twice is now rejected too -- the slot may already have been handed to another invocation, so the second answer carried that invocation's rid and replied to the wrong call. **Still open:** nothing reclaims a slot the application abandons. Whether to add a timeout, or to make it the application's documented contract, is a design call. |
| D-3 | Medium | **Fixed** | Twin `pending[]` slots were never cleared when the connection dropped, so a GET or PATCH issued just before an outage never fired its callback and never released its slot; after `AZ_IOT_TWIN_MAX_PENDING` (default 8) outages the client refused every request with `NOT_SUPPORTED`. Same family as C-3, which fixed the equivalent hole for QoS-1 publish callbacks but did not reach the twin's own correlation table. The connection client now notifies registered feature clients when a session ends, and the twin client completes its pending requests with `AZ_IOT_ERR_NOT_CONNECTED`, releasing each slot before the callback runs so a callback that re-issues immediately can claim it. `destroy()` stays silent, for the same reason C-3 does. |
| D-3b | Low | **Fixed** | Twin service statuses collapsed onto codes the caller could not act on: 429 (throttled) mapped to `NOT_SUPPORTED` — the same code a locally full pending table returns, so "back off" was indistinguishable from "too many requests in flight here" — and 400 (malformed reported-properties JSON) fell through to the generic `ERR_MQTT`, looking like a transport failure worth retrying. Now 400 maps to `INVALID_ARG`, 404 to `NOT_FOUND`, 429 to `BUSY`, and everything else to `ERR_MQTT`. |
| D-4 | Low | **Fixed** | The `$version` IoT Hub returns on a reported-properties ack (`$iothub/twin/res/204/?$rid=1&$version=6`) was parsed off the topic and thrown away, so an application tracking the reported version to detect a lost update could not. `az_iot_twin_patch_ack_callback` now takes it alongside the status; 0 means the service sent none, which includes every failure. |
| D-5 | Medium | **Fixed** | The MQTT keep-alive was hardcoded to 30 s in `start_connect_attempt()` with no option to change it, and the connect timeout was likewise fixed at 30 s. IoT Hub's server-side timeout is 1.5x the client value (capped at 1767 s, so 1177 s is the largest useful setting), and every other Azure IoT device SDK exposes this: a battery-powered or metered-link device could not lengthen it, and a device on a lossy link could not shorten it to notice a dead link sooner. Now `opts.keep_alive_seconds` and `opts.connect_timeout_ms`, with 0 selecting the previous values so existing callers are unaffected. |
| D-6 | Low | Open | `AZ_IOT_MAX_PERSISTENT_SUBS` defaults to 8 while IoT Hub allows a device **five** topic subscriptions. The SDK therefore accepts a sixth filter locally and only finds out at SUBACK time. A device using C2D + direct methods + twin (response and desired) + credential renewal already sits at exactly five, so there is no headroom left for an application filter. |
| D-7 | **High** | **Fixed** | `az_iot_connection_client_register_mqtt_factory()` appended unconditionally, so registering the same factory twice stored two entries pointing at one `factory_ctx`. The duplicate was never reachable -- `find_factory()` returns the first match for a version -- but `destroy()` walks the whole registry and calls every entry's `destroy` hook, so it freed that `factory_ctx` twice and corrupted the heap. Found while writing the D-3 tests: a test that re-opened a connection through the existing `open_to_connected()` helper aborted with `double free or corruption (fasttop)`. An exact duplicate (same `create`, `factory_ctx`, `destroy` and `version`) is now idempotent; a genuinely different factory still registers. |
| D-8 | Low | **Fixed** | Two feature clients of the same type on one connection registered the identical dispatch prefix, and routing is longest-prefix-wins with no tie-break, so one of them silently received nothing for the life of the connection. Fixed at the dispatch layer rather than in one feature client, so it protects all of them: `az_iot_dispatch_register_prefix()` now refuses an exact-duplicate prefix with `ALREADY_INITIALIZED`, which surfaces as an init failure the caller can see. The check is deliberately exact-match only -- connection multiplexing puts several identities on one connection and their topics differ by device id, so distinct identities still register side by side. |
