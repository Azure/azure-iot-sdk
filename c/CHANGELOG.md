# Release History

## 1.0.0 (2026-11-05)

### Features Added

- Breaking: the az_mqtt adapter footprint is removed. `AZ_IOT_AZ_MQTT_FOOTPRINT` (macro, with
  `AZ_IOT_AZ_MQTT_FOOTPRINT_DEFAULT` and `_CONSTRAINED`, and CMake option `DEFAULT` or
  `CONSTRAINED`) selected only `AZ_IOT_AZ_MQTT_USER_PROPERTIES_MAX`, which is now the one setting
  (default 16; `CONSTRAINED` was 4). Defining the macro or setting the CMake option fails the
  build. `AZ_IOT_AZ_MQTT_CONFIG_FILE` stays.
- Breaking: `az_iot_mqtt_adapter` gains an optional `validate_limits_for_session_usage()`, which
  `az_iot_connection_client_init()` calls with whether the hub session outlives its connection
  (`az_iot_mqtt_session_usage`); init fails with `AZ_IOT_ERR_INVALID_ARG` when it refuses the
  limits. Positional initializers of the struct need a third member (NULL). The az_mqtt adapter
  refuses `message_store_size` 0 unless `session_continuity` is `AZ_IOT_SESSION_CONTINUITY_CLEAN`
  or `dps.provision_only` is set, so this fails at init instead of at every connect. Paho, and
  adapters that leave it NULL, are unaffected.
- Breaking: the mqttv3 direct method client holds its in-flight invocations in a workspace the
  application provides, sized at run time. `az_iot_mqttv3_direct_method_client_init()` takes
  `az_iot_mqttv3_direct_method_client_options` (`az_iot_mqttv3_direct_method_client_options_default()`):
  `limits.max_in_flight_invocations` (0: the default
  `AZ_IOT_MQTTV3_DIRECT_METHOD_CLIENT_DEFAULT_MAX_IN_FLIGHT_INVOCATIONS`, 4; at most 1024),
  `workspace` and `workspace_size` (`az_iot_mqttv3_direct_method_client_get_workspace_size()`,
  `AZ_IOT_MQTTV3_DIRECT_METHOD_CLIENT_WORKSPACE_SIZE()`). `init()` returns `AZ_IOT_ERR_INVALID_ARG`
  for a limit above its maximum or a NULL workspace, and `AZ_IOT_ERR_NOT_ENOUGH_SPACE` for one too
  small; `deinit()` wipes it. `AZ_IOT_DM_MAX_INFLIGHT` is removed (defining it is a compile error).
- Breaking: the mqttv3 twin client holds its pending requests in a workspace the
  application provides, sized at run time. `az_iot_mqttv3_twin_client_init()` takes
  `az_iot_mqttv3_twin_client_options` (`_options_default()`): `limits.max_pending_requests` (`az_iot_twin_client_limits`;
  0: the default `AZ_IOT_TWIN_CLIENT_DEFAULT_MAX_PENDING_REQUESTS`, 8; at most 1024), `workspace`
  and `workspace_size` (`az_iot_mqttv3_twin_client_get_workspace_size()`,
  `AZ_IOT_MQTTV3_TWIN_CLIENT_WORKSPACE_SIZE()`). `init()` returns
  `AZ_IOT_ERR_INVALID_ARG` for a limit above its maximum or a NULL workspace, and
  `AZ_IOT_ERR_NOT_ENOUGH_SPACE` for one too small; `deinit()` wipes it. `AZ_IOT_TWIN_MAX_PENDING`
  is removed (defining it is a compile error).
- Breaking: feature-client string and topic sizes that lay out public structs are fixed:
  `AZ_IOT_DM_METHOD_NAME_MAX` (96), `AZ_IOT_DM_RID_MAX` (32) and `AZ_IOT_LOG_FILE_PATH_MAX` (256).
  Defining any of them before including the headers is a compile error, so these sizes cannot
  differ between the application and the SDK.
- Breaking: the MQTT factory API is removed; the connection client builds every session's MQTT
  client with `az_iot_connection_client_options::mqtt_adapter` in `mqtt_workspace`, sized by
  `mqtt_limits`. All three are required: `init()` returns `AZ_IOT_ERR_INVALID_ARG` when
  `mqtt_adapter` is NULL. Removed: `az_iot_mqtt_factory`,
  `az_iot_connection_client_register_mqtt_factory()`, `AZ_IOT_MAX_MQTT_FACTORIES` (defining it is
  a compile error), `az_iot_paho_factory_create_v3_1_1()`, `az_iot_paho_factory_create_v5()`,
  `az_iot_paho_factory_destroy()`, `az_iot_az_mqtt_factory_create_v3_1_1()`,
  `az_iot_az_mqtt_factory_create_v5()` and `az_iot_az_mqtt_factory_destroy()`. The az_mqtt
  adapter's sizes are run-time values only: `AZ_IOT_AZ_MQTT_SEND_BUFFER_SIZE`,
  `AZ_IOT_AZ_MQTT_RECEIVE_BUFFER_SIZE`, `AZ_IOT_AZ_MQTT_INFLIGHT_MAX` and
  `AZ_IOT_AZ_MQTT_MESSAGE_STORAGE_SIZE` (macros, and the first three as CMake options) are removed;
  defining one fails the build. Neither adapter allocates its client any more.
- Connection client: `az_iot_connection_client_set_dps_assignment_invalidated_callback()` reports,
  once, when the client will not use the assignment it holds again: DPS returned an assignment it
  rejected, the hub refused the identity in `AZ_IOT_IDENTITY_RECOVERY_REPROVISION` mode, or
  `dps.max_hub_connect_attempts_before_reprovision` was reached. An application that saves the
  assignment deletes it there, so a restart registers with DPS instead of restoring it. The
  `dps_persist_assignment` sample does. See
  [docs/connecting.md](docs/connecting.md#saved-hub-with-dps-fallback).
- Breaking: the connection client runs one MQTT session at a time; the provisioning session and the
  hub session no longer overlap. Creating an MQTT client for one session while the other's exists
  fails with `AZ_IOT_ERR_INTERNAL`.
- Connection client (internal): the provisioning session is wanted through
  `__dps_session_ensure()` or `__dps_publish()` and given back with `__dps_session_release()`;
  `__dps_user_acquire()` and `__dps_user_release()` are removed. Behavior is unchanged.
- Connection client: `az_iot_connection_client_options::mqtt_adapter`, `mqtt_limits`,
  `mqtt_workspace` and `mqtt_workspace_size` build each session's MQTT client in application
  memory (e.g. `az_iot_az_mqtt_get_adapter()`), instead of registered factories. `init()` checks
  the workspace size; `deinit()` wipes it. The workspace
  holds one client at a time, as one MQTT session runs at a time.
- SAS token renewal lets the session's deliveries settle before its DISCONNECT: publishes
  awaiting a PUBACK and SDK requests awaiting a response (twin), for up to 5 s (not past the
  token's expiry). Meanwhile a new QoS 1 publish returns `AZ_IOT_ERR_BUSY`; once the DISCONNECT
  is sent, any publish returns `AZ_IOT_ERR_NOT_CONNECTED`. A renewal now takes up to that plus
  the disconnect bound.
- mqttv3 twin: requests go through the delivery tracker and time out:
  `az_iot_mqttv3_twin_client_set_request_timeout()` (default
  `AZ_IOT_MQTTV3_TWIN_REQUEST_TIMEOUT_MS_DEFAULT`, 60 s, from the call). A GET waits for a
  connection and is sent again on the next session when its session ends unanswered; a reported
  patch is sent at once or refused (`AZ_IOT_ERR_BUSY` or `AZ_IOT_ERR_NOT_CONNECTED`) and fails
  with `AZ_IOT_ERR_NOT_CONNECTED` at a session end.
- Breaking: `az_iot_subscription_failed_callback` takes the refused SUBACK's MQTT 5 Reason String
  (`reason_string`, `reason_string_len`, after `protocol_code`). On a session-scoped filter the
  Reason String is reported as `az_iot_connection_state_event.error->message`. Both adapters report
  it on `AZ_IOT_MQTT_EVT_SUBSCRIBE_ACK`.
- Certificate renewal: `az_iot_connection_client_options::csr_response_timeout_ms` sets how long
  `az_iot_connection_client_send_csr()` waits for a terminal response (0: 120 s,
  `AZ_IOT_DEFAULT_CSR_RESPONSE_TIMEOUT_MS`). A renewal open when the session ends (a drop or
  `close()`) now fails with `AZ_IOT_CSR_FAILED` / `AZ_IOT_ERR_NOT_CONNECTED` instead of staying
  open; resubmit it with the same request id once reconnected. `deinit()` calls no renewal
  callback.
- Connection client: on MQTT 5, the Reason String of a refused CONNACK or of a server DISCONNECT
  with a nonzero reason code is reported as `az_iot_connection_state_event.error->message` (source
  `AZ_IOT_CONN_ERR_SRC_MQTT`), truncated to `AZ_IOT_CONN_ERROR_MESSAGE_MAX` bytes.
  `az_iot_mqtt_event.reason_string` now also carries it on `AZ_IOT_MQTT_EVT_DISCONNECTED` (both
  adapters) and on a failed `AZ_IOT_MQTT_EVT_CONNECTED` (az_mqtt adapter; Paho does not expose it).
  The Paho adapter keeps 128 bytes of a DISCONNECT Reason String.
- Connection client: a hub that refuses the device right after a DPS assignment, before the new
  registration reaches it, is retried soon: `identity_recovery.after_assignment_policy` (default
  `az_iot_connection_client_get_default_identity_recovery_after_assignment_policy()`: 5 s doubling
  to 15 s, ±20%, 4 attempts) paces those refusals until `HUB:CONNECTED`, before the 5-minute
  identity recovery policy. Zeroed, every refusal waits on `policy` as before. See
  [Identity recovery](docs/connecting.md#identity-recovery).
- Breaking: removed the Rust MQTT adapter shell, its `az_iot_rust_mqtt_*` APIs and FFI contract,
  `AZ_IOT_RUST_MQTT_WORKSPACE_SIZE`, `AZ_IOT_WITH_RUST_MQTT`, the `adapter_rust_mqtt` CMake and
  pkg-config component, and Yocto's `rust-mqtt` option. Use Paho or az_mqtt instead; the generic
  bring-your-own MQTT interface is unchanged.
- `az_iot_connection_profile_to_string()` names an `az_iot_connection_profile`, for logging.
- `az_iot_certificate_material::client_key_psa_id` and `az_iot_mqtt_tls_options::client_key_psa_id`
  name a non-extractable client key by PSA Crypto key id (0: none), e.g. the ESP32 Digital
  Signature peripheral on ESP-IDF v6. The az_mqtt adapter uses it with mbedTLS; with another TLS
  backend, and in the Paho adapter, the connect fails with
  `AZ_IOT_ERR_NOT_SUPPORTED`. Bring-your-own adapters must do the same unless they support it.
- Conformance: `AZ_IOT_CONFORMANCE_CAP_KEY_CUSTODY_PSA` and `az_iot_conformance_options::client_key_psa_id`
  prove the PSA key route with a TLS handshake and a reconnect. The az_mqtt harness declares it on
  mbedTLS with PSA keys and imports the PEM key at `AZ_IOT_CONFORMANCE_PSA_KEY`.
- Breaking: `az_iot_connection_client_open()` and `az_iot_connection_client_close()` return
  `AZ_IOT_ERR_BUSY` when called from a callback the connection client runs (state, publish
  completion, SAS token, DPS, CSR, subscription failure, inbound message, feature observer). Call
  them after `do_work()` returns. See [Opening and closing](docs/connecting.md#opening-and-closing).
- Breaking: `az_iot_connection_client_deinit()` returns `az_iot_result`: `AZ_IOT_ERR_BUSY` from
  the same callbacks, where it releases nothing, and `AZ_IOT_ERR_INVALID_ARG` for a NULL client.
- Breaking: `az_iot_connection_client_limits::max_session_handlers` and `max_feature_state_observers`
  are replaced by `max_feature_observers` (default 10,
  `AZ_IOT_CONNECTION_CLIENT_DEFAULT_MAX_FEATURE_OBSERVERS`): feature clients observe connection
  state, session end, publish completion and the `do_work()` tick through one table.
  `AZ_IOT_CONNECTION_CLIENT_WORKSPACE_SIZE()` takes 7 counts (was 8), and
  `AZ_IOT_CONNECTION_CLIENT_DEFAULT_MAX_SESSION_HANDLERS`,
  `AZ_IOT_CONNECTION_CLIENT_DEFAULT_MAX_FEATURE_STATE_OBSERVERS` and `az_iot_session_end_callback`
  are removed.
- Connection client (internal): feature observers learn why a session ended
  (`AZ_IOT_CONNECTION_SESSION_END_PLANNED` after `close()` or a SAS-renewal reconnect,
  `AZ_IOT_CONNECTION_SESSION_END_LOST` otherwise) and get `AZ_IOT_CONNECTION_FEATURE_EVENT_IDENTITY_CHANGED`
  when a DPS assignment replaces a different hub or device id, before the assignment callback.
- `az_iot_connection_client_options::max_unconfirmed_delivery_timeout_ms` bounds how long an SDK
  message whose peer set no Message Expiry is kept for confirmation or resend. 0 selects
  `AZ_IOT_DEFAULT_MAX_UNCONFIRMED_DELIVERY_TIMEOUT_MS` (300000); it cannot be disabled. The Message Expiry sent is unchanged
  (none).
- Breaking: connection client entry counts are run-time limits
  (`az_iot_connection_client_options::limits`, 0 = default) instead of compile-time macros. The
  entries live in `options.workspace`, sized by `az_iot_connection_client_get_workspace_size()` or
  `AZ_IOT_CONNECTION_CLIENT_WORKSPACE_SIZE()`, which is required: `init()` returns
  `AZ_IOT_ERR_INVALID_ARG` without one. `AZ_IOT_MAX_PENDING_PUBACKS`, `AZ_IOT_MAX_PUBACK_RESERVATIONS`,
  `AZ_IOT_MAX_PERSISTENT_SUBS`, `AZ_IOT_MAX_SESSION_HANDLERS`, `AZ_IOT_MAX_FEATURE_STATE_OBSERVERS`,
  `AZ_IOT_MAX_APP_STATE_OBSERVERS`, `AZ_IOT_MAX_FEATURE_CLIENT_BINDS` and
  `AZ_IOT_MAX_INBOUND_HANDLERS` are removed. `AZ_IOT_DPS_HOST_BUF`,
  `AZ_IOT_DPS_DEVICE_ID_BUF`, `AZ_IOT_DPS_OPERATION_ID_MAX`, `AZ_IOT_CONNECTION_PROFILE_RAW_BUF`,
  `AZ_IOT_MQTT_USERNAME_BUF`, `AZ_IOT_PERSISTENT_SUB_TOPIC_MAX` and `AZ_IOT_DISPATCH_PREFIX_MAX` are
  fixed. Defining any of them before including the headers is a compile error, so no build of the
  application can lay out the client differently from the SDK. The defaults are compile-time
  values (`AZ_IOT_CONNECTION_CLIENT_DEFAULT_MAX_*`, `AZ_IOT_CONNECTION_CLIENT_CONFIG_FILE`, or the
  CMake options of the same names); `AZ_IOT_CONNECTION_CLIENT_DEFAULT_WORKSPACE_SIZE` sizes a static
  workspace for them.
- az_mqtt adapter in application memory: `az_iot_az_mqtt_get_adapter()` returns an
  `az_iot_mqtt_adapter` whose `init()` builds an MQTT 3.1.1 or MQTT 5 client in a workspace the
  application provides, with no allocation. Sizes are run-time values (`az_iot_mqtt_limits`): send
  and receive buffers, in-flight messages, message store and connect-string copies.
  `az_iot_az_mqtt_get_workspace_size()` gives the exact size; `AZ_IOT_AZ_MQTT_WORKSPACE_SIZE()` a
  constant-expression bound for static arrays. The client's `destroy()` wipes the workspace and
  frees nothing.
- Paho adapter in application memory: `az_iot_paho_get_adapter()` returns an
  `az_iot_mqtt_adapter` descriptor. The workspace holds the adapter's own state
  (`AZ_IOT_PAHO_WORKSPACE_SIZE`); Paho still allocates its internals and sizes its own buffers,
  so the limits are only checked. `destroy()` releases those internals, wipes the workspace
  and frees nothing else.
- az_mqtt adapter: `az_iot_mqtt_limits::message_store_size` may be 0, for devices that only use
  clean sessions: no message store is reserved, `az_iot_connection_client_init()` refuses it
  unless `session_continuity` is `AZ_IOT_SESSION_CONTINUITY_CLEAN` or `dps.provision_only` is set,
  and a connect whose session
  outlives the connection (`clean_start` false; MQTT 5: with `session_expiry_seconds` > 0) returns
  `AZ_IOT_ERR_NOT_SUPPORTED`.
- Retry policy: `az_iot_retry_policy::jitter_mode`. `AZ_IOT_RETRY_JITTER_ADDITIVE` only lengthens a
  delay (+0..`jitter_pct`); the default `AZ_IOT_RETRY_JITTER_SYMMETRIC` keeps the existing ± jitter.
- Initial preview of the C SDK for Azure IoT Hub (mqttv3).
- DPS assignment notification: `az_iot_connection_client_set_dps_assignment_callback()` reports the
  hub, device id and profile each time an assignment is adopted, so an application can persist it
  and connect directly after a restart. See
  [docs/connecting.md](docs/connecting.md#saved-hub-with-dps-fallback) and
  [samples/authentication/dps_persist_assignment](samples/authentication/dps_persist_assignment/README.md).
- DPS registration with TPM attestation over HTTPS (`az_iot_dps_tpm_client.h`): builds the
  requests and parses the responses, with no I/O or allocation; the application sends them and
  runs the TPM (key import, HMAC-SHA256). Also splits the DPS key into TPM command inputs and
  derives a registration ID from the endorsement key. Sample
  [authentication/dps_tpm](samples/authentication/dps_tpm/README.md), with a TPM simulated in
  software. See [docs/connecting.md](docs/connecting.md#authentication).
- az_mqtt MQTT adapter (`AZ_IOT_WITH_AZ_MQTT=ON`, `az_iot_adapter_az_mqtt.h`): MQTT 3.1.1 and 5
  over the bundled `deps/az_mqtt` client, with no adapter thread (connect and receive in
  `process_loop()`, sends written when called); TLS through OpenSSL 3
  or mbedTLS (`AZ_IOT_AZ_MQTT_TLS`), Schannel on Windows (X.509 and in-memory PEM included);
  WebSockets, HTTP CONNECT proxy, and key references through an OpenSSL 3 provider. Buffer sizes
  are run-time values (`az_iot_mqtt_limits`); the MQTT 5 CONNECT user property bound is set when
  compiled, with or without CMake: a definition or a config header. See [docs/client-configuration.md](docs/client-configuration.md#az_mqtt-adapter-limits).
- Logging for support diagnostics: rotating file sink (`az_iot_log_file_sink_open()`), lines
  `<UTC ISO 8601 time> [LEVEL] [component] [thread] [file:line] message` from the built-in sinks, connection configuration,
  state changes and retries at `INFO`, and a `...` marker on truncated messages.
- Breaking: log calls and sinks take a component. `AZ_IOT_LOG_*(component, msg)`,
  `az_iot_log_emit*(level, component, file, line, ...)` and `az_iot_log_sink_callback` gain a
  `component` argument; SDK values are the `AZ_IOT_LOG_COMPONENT_*` macros in `az_iot_log_components.h`. See [docs/logging.md](docs/logging.md).
- Breaking: every connection attempt now starts in the new `AZ_IOT_CONN_STATE_SETTING_UP`, so an
  attempt that fails on the device (credential load, SAS signing, MQTT client creation, buffer too
  small) produces a state event on each retry. Such failures carry the new error source
  `AZ_IOT_CONN_ERR_SRC_LOCAL`. `AZ_IOT_CONN_STATE_RECONNECTING` is renamed
  `AZ_IOT_CONN_STATE_RETRY_PENDING`. See [docs/connecting.md](docs/connecting.md#connection-states).
- SAS token renewal: at `renewal_percent` of a key-signed token's lifetime, the hub session, and a
  provisioning session kept open without a registration, reconnect with a new token; those state
  events carry `is_credential_renewal`. See
  [docs/connecting.md](docs/connecting.md#authentication).
- Breaking: certificate provider `release()` contract. The client releases material for a
  connection attempt only after the adapter's `connect()` has copied it (material loaded only to
  validate or probe certificates is released at once), and a provider should then wipe any
  in-memory private key or password it handed out. The PEM provider now reads the private key at
  `load()`, wipes it at the last `release()`, and wipes the key and its password at `deinit()`;
  each `client_key_pem` it returns is valid only until the matching `release()`, no longer until
  `deinit()`. Adapters must copy every `az_iot_mqtt_tls_options` string during `connect()`
  (`sign` and `sign_ctx` stay valid until the adapter client is destroyed). See
  [docs/how_to_byo_mqtt_client.md](docs/how_to_byo_mqtt_client.md).
- mqttv3 file upload for SAS-key devices: when the connection authenticates the hub with a SAS
  key, the HTTP transport hook receives a hub SAS token as `authorization`. Sized by
  `AZ_IOT_FILE_UPLOAD_AUTHORIZATION_MAX`.
- Breaking: the certificate provider's `load()` takes an index. A provider may offer several
  certificates per role (index 0, 1, ...; `AZ_IOT_ERR_NOT_FOUND` past the last); the client
  tries them in order when the service rejects one, before the SAS sources, and reports the index
  in `x509_index`. At most `AZ_IOT_MAX_CERTS_PER_ROLE` (default 4) per role. A provider with
  one certificate per role must return `AZ_IOT_ERR_NOT_FOUND` for `index > 0`.
- SAS token helpers (`az_iot_sas_token.h`): `az_iot_sas_token_string_to_sign()`,
  `az_iot_sas_token_from_signature()` (token from an HMAC computed elsewhere, e.g. in an HSM),
  `az_iot_sas_token_sign()` and `az_iot_sas_derive_device_key()`, for tokens supplied with
  `az_iot_connection_client_update_sas_token()`.
- Managed certificate provider: `AZ_IOT_MANAGED_KEY_EC_P384` generates an ECDSA P-384 operational
  key. The CSR digest now follows the key in use: for EC keys, SHA-256 up to 256 bits, SHA-384 up
  to 384 bits (P-384), SHA-512 above; SHA-256 for RSA (was always SHA-256). An unknown `key_type` fails init with
  `AZ_IOT_ERR_INVALID_ARG` (was treated as EC P-256). See
  [docs/connecting.md](docs/connecting.md#certificates).
- SAS tokens from the application: `on_sas_token_required` notifies, tried after the keys;
  `az_iot_connection_client_update_sas_token()` supplies a token for the request, or at any time
  (a connected session then renews with it). At renewal the token is requested while the
  session stays up; once supplied, the session disconnects and reconnects with it. See [docs/connecting.md](docs/connecting.md#authentication).
- DPS: a registration that ends `failed` with `deviceId` but no `assignedHub` (e.g. a failed
  reprovisioning) is now reported as `AZ_IOT_ERR_DPS`, with the service's `errorCode` and
  `errorMessage` in the state event's error detail, instead of `AZ_IOT_ERR_PROTOCOL`.
- DPS: `az_iot_connection_client_get_device_id()` returns the device id the client connects to
  the hub with: the DPS-assigned one once the assignment is adopted, which can differ from the
  registration id. `NULL` while no hub is established.
- Hub certificate renewal: `AZ_IOT_CSR_FAILED` now carries the hub's `info.credentialError`,
  `info.credentialMessage` and `trackingId` as `credential_error`, `credential_message` and
  `tracking_id` on `az_iot_csr_event` (never NULL; empty when absent).
- PUBACK reason codes: `az_iot_mqtt_puback_result()` maps a PUBACK code to a result. An MQTT 5
  refusal the broker will repeat (`0x87`, `0x90`, `0x99`), or `0x95` for a publish over the
  server's Maximum Packet Size, is the new `AZ_IOT_ERR_PUBLISH_REFUSED`, quota exceeded (`0x97`) is `AZ_IOT_ERR_BUSY`, other failures stay
  `AZ_IOT_ERR_MQTT`. The az_mqtt adapter uses it; publish callbacks receive it unchanged, and the
  connection client logs the code of a failed PUBACK.
- Breaking: the connection client no longer reads `AZ_IOT_DEVICE_ID` or
  `AZ_IOT_DPS_CONNECTION_PROFILE_OVERRIDE`. DPS now returns `connectionProfile`.
- Breaking: telemetry `send()` returns `AZ_IOT_ERR_NOT_CONNECTED` (was
  `AZ_IOT_ERR_NOT_INITIALIZED`) before a provisioning connection has a device id. The delivery contract is documented in [docs/telemetry.md](docs/telemetry.md).
- Build against installed dependencies: `AZ_IOT_USE_INSTALLED_AZURE_SDK_FOR_C` and
  `AZ_IOT_USE_INSTALLED_PAHO` (both `OFF`) use installed azure-sdk-for-c and Eclipse Paho C instead
  of fetching them. `c/vcpkg.json` has a `builtin-baseline` and an `installed-paho` feature. See
  [docs/client-configuration.md](docs/client-configuration.md#installed-dependencies).
- Breaking: the application owns MQTT factories. `az_iot_mqtt_factory` loses `destroy`, and
  `az_iot_connection_client_deinit()` no longer destroys registered factories: registration only
  lends a factory to the client. The factory API was later removed (see the first entry above).
- Breaking: mqttv3 C2D properties are read on demand instead of being decoded into SDK buffers.
  `az_iot_c2d_message` keeps `payload`/`payload_len` and gains `properties`, a read-only view of
  the topic property bag; `content_type`, the `properties` array, `properties_count`,
  `az_iot_c2d_property`, `az_iot_c2d_message_property()`, `AZ_IOT_C2D_MAX_PROPERTIES` and
  `AZ_IOT_C2D_PROPERTY_BUFFER` are removed. `az_iot_c2d_properties_next()` iterates,
  `az_iot_c2d_properties_find()` looks one up by name, and typed accessors read the system
  properties: `az_iot_c2d_message_get_message_id()` (`$.mid`), `_correlation_id()` (`$.cid`),
  `_expiry_time()` (`$.exp`), `_content_type()` (`$.ct`), `_content_encoding()` (`$.ce`) and
  `_user_id()` (`$.uid`). Each percent-decodes into a buffer the application provides and returns
  `AZ_IOT_ERR_NOT_ENOUGH_SPACE` (with the size needed), `AZ_IOT_ERR_PROTOCOL` or
  `AZ_IOT_ERR_NOT_FOUND`, so nothing is dropped silently and there is no limit on the number or
  size of properties. Adds `AZ_IOT_MSG_PROP_EXPIRY_TIME`.
- Breaking: `az_iot_connection_client_options` fields `host` and `client_id` are renamed
  `iothub_hostname` and `device_id`.
- Saved hub with DPS fallback: `iothub_hostname`, `device_id` and `connection_profile` set together with the
  `dps` fields connect to that hub first and use DPS only when a re-provision is due. The saved
  `connection_profile` is now declared, so a feature client for another profile fails
  with `AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH` instead of attaching to a session of the wrong
  profile. See [docs/connecting.md](docs/connecting.md#saved-hub-with-dps-fallback).
- Breaking: MQTT 5 user properties are no longer copied into fixed arrays or capped.
  `az_iot_mqtt_message.user_properties` is now an `az_iot_mqtt_user_properties` view, read with
  `az_iot_mqtt_user_properties_next()` / `_find()` (`az_iot_mqtt_user_property_view`: UTF-8 with
  lengths, not NUL-terminated); `user_properties_count` is removed, and
  `az_iot_mqtt_user_properties_from_array()` wraps an array. Inbound, every property reaches the
  SDK; the az_mqtt adapter dropped those past `AZ_IOT_AZ_MQTT_USER_PROPERTIES_MAX` (4 or 16), which
  now limits CONNECT user properties only.
  MQTT adapters, including your own, must implement and read the view: see
  [docs/how_to_byo_mqtt_client.md](docs/how_to_byo_mqtt_client.md). The vendored az_mqtt is
  patched for it (`deps/az_mqtt/VENDORED.md`).
- Threading contract documented: the SDK does not lock its state against other threads, so every
  SDK call in a process must come from one thread or be serialized by the application
  ([docs/architecture.md](docs/architecture.md)). `tls.sign` and the certificate provider's
  `sign()` are called only on that thread. Sample
  [unified/multithreaded_telemetry](samples/unified/multithreaded_telemetry/README.md) hands
  readings from other threads to the SDK thread.
- ESP-IDF components in [platforms/esp32](platforms/esp32/README.md): the SDK, azure-sdk-for-c and
  the mbedTLS crypto backend.
- az_mqtt adapter on ESP-IDF (`azure-iot-sdk-az-mqtt` component): with no trusted CA it verifies
  the server against the ESP-IDF certificate bundle.
- ESP-IDF certificate provider over the `esp_secure_cert` partition
  (`azure-iot-sdk-certificate-provider-esp-secure-cert`, `espressif/esp_secure_cert_mgr` 2.9.3):
  `az_iot_certificate_provider_esp_secure_cert_init()`. With the Digital Signature peripheral the
  key is a PSA key (`client_key_psa_id`); otherwise the partition's PEM key, read per connection.
- `eng/esp-idf-qemu.sh` boots a build in QEMU and waits for a log line, and CI boots the QEMU
  builds until they get an IPv4 address.
- ESP32 IoT Hub sample [unified/esp32](samples/unified/esp32/README.md): DPS with the
  `esp_secure_cert` identity, then telemetry, the `echo` direct method and desired-twin
  acknowledgements on the assigned mqttv3 hub. CI builds it for esp32, esp32c3,
  esp32s3 and esp32c6, and boots its QEMU build on esp32, esp32s3 and esp32c3.
- ESP32 end-to-end test: `ci-c-e2e` runs the ESP32 IoT Hub sample in QEMU on esp32 and esp32c3
  against an IoT Hub (telemetry, the `echo` method, a desired-version acknowledgement), with
  the esp32c3 key in the emulated Digital Signature peripheral. `eng/esp-idf-qemu-e2e.sh` runs it
  locally; `eng/esp-idf-qemu-identity.sh` writes a QEMU device identity
  ([docs](docs/eng/end-to-end-tests.md#esp32-in-qemu)).

### Bugs Fixed

- Paho: a QoS 1 publish whose write fails with a socket error no longer crashes the application.
  Paho 1.3.16 freed the message's topic and payload twice: when the session was cleaned, or in
  `MQTTAsync_destroy()` for the hub session. IoT Hub triggers it by resetting the connection on a
  publish over its Maximum Packet Size. The bundled Paho is patched with the upstream fix
  (eclipse-paho/paho.mqtt.c#1592); an installed Paho is not. The adapter also reports such a
  failure with the publish's packet id, where it reported packet id 0, which matched no publish.
  See [docs/dependencies.md](docs/dependencies.md#publish-freed-twice-after-a-failed-write).
- ESP32 sample `unified/esp32` builds with every ESP-IDF 6 release: it pinned
  `espressif/ethernet_init` to `==1.2.0`, while ESP-IDF v6.0.2 and later need
  `^1.3.0`. The version now comes from ESP-IDF's `protocol_examples_common`. CI builds the ESP32
  sample and runs the ESP32 QEMU e2e with ESP-IDF v6.1, and builds it with v6.0.
- ESP32 with the Digital Signature peripheral (esp32c3, esp32s3, esp32c6) connects with ESP-IDF
  v6.0.1 and later: the `esp_secure_cert` certificate provider imports the DS key in the layout
  those releases expect (an `esp_rsa_ds_opaque_key_t`; v6.0 keeps the old one), and az_mqtt's
  mbedTLS backend wraps a PSA key that cannot export its public key (TF-PSA-Crypto 1.1, ESP-IDF
  v6.0.3 and later) with the client certificate's public key.
- Connection client, events drained in one `process_loop()`: a failed acknowledgement (PUBACK,
  SUBACK, UNSUBACK) becomes connection error detail only when it fails the connection. A gated
  or DPS SUBACK drained after a session-ending event is ignored; a FAILS_SELF refusal is still
  reported. A later end event of an ended session does not replace the server DISCONNECT's code,
  and an identity refusal is acted on whatever came first. While `close()` disconnects, a late
  SUBACK does not announce CONNECTED, and a new filter waits for the next
  session.
- ESP32: the `azure-sdk-for-c` component implements `az_platform_clock_msec()` and
  `az_platform_sleep_msec()` with `esp_timer` and FreeRTOS, so the az_mqtt adapter's idle wait
  sleeps instead of returning at once.
- Paho and az_mqtt adapters: `AZ_IOT_MQTT_EVT_DISCONNECTED` carries `AZ_IOT_OK` only for a
  completed `disconnect()`. A lost connection (Paho) and a server DISCONNECT with reason 0x00
  (`az_iot_mqtt_disconnect_result()`) now report `AZ_IOT_ERR_NOT_CONNECTED`.
- Paho adapter: `process_loop()` returns as soon as an event is queued and dispatches it, instead
  of sleeping its whole timeout and dispatching it on the next call.
- Builds with OpenSSL 4.0 and 4.1 without deprecation warnings, which failed the build (Windows
  `/sdl`, Linux `-Werror`): the Paho key custody adapter uses `ASN1_STRING_set1_string()` on
  OpenSSL 4.1+, and the tests no longer use deprecated or now-const X.509 name accessors.
- Samples README: the documented OpenSSL range is 3.0 to 4.1 (the Windows installer it links
  installs 4.x), with the full (not Light) installer and its `bin` folder on `PATH`. New: building
  on Windows without OpenSSL (az_mqtt over Schannel) and what that leaves out.
- az_mqtt adapter: the bundled az_mqtt is updated to main `3a2c61b` (was `58a0799` plus selected
  changes; see `deps/az_mqtt/VENDORED.md`). Invalid options are refused with `AZ_ERROR_ARG` before
  anything is sent: Will, SUBSCRIBE and PUBLISH QoS above 2; MQTT 5 Retain Handling above 2, No
  Local on a shared subscription, Authentication Data without Authentication Method, and AUTH reason
  codes other than Success, Continue authentication and Re-authenticate. The connection is closed
  when a CONNACK reports Session Present after Clean Session (MQTT 3.1.1) or Clean Start (MQTT 5),
  and, in MQTT 5, as soon as a received packet's fixed header announces more than the advertised
  Maximum Packet Size (the receive buffer size), after DISCONNECT 0x95; that session ends with
  `AZ_IOT_ERR_NOT_ENOUGH_SPACE`, as before. An mbedTLS verify callback's rejection is reported as a
  TLS verification failure.
- Paho: the bundled v1.3.16 is patched with two upstream fixes for data races between the
  application thread and Paho's threads: `MQTTAsync_destroy()` against the send thread on the
  command queue (eclipse-paho/paho.mqtt.c#1709), and `MQTTAsync_connect()` against the timeout and
  retry checks on the client's state (#1714). An installed Paho is not patched. See
  [docs/dependencies.md](docs/dependencies.md#races-on-the-command-queue-and-client-state).
- az_mqtt adapter: the copies `connect()` makes of credentials (password or SAS token, PEM key and
  certificate, proxy credentials) are zeroed before they are freed, and `destroy()` zeroes the send
  buffer, which holds the CONNECT, before freeing it.
- az_mqtt adapter: a `connect()` refused after the busy check (invalid or unsupported options, no
  memory for the message store) now releases the copies of the previous connect's strings, including
  credentials; they were kept until the next accepted connect or `destroy()`.
- Paho adapter: `az_iot_mqtt_connect_options::connect_timeout_seconds` is passed to Paho; it was
  ignored, so every connect attempt used Paho's 30 s default. 0 still selects that default.
- Paho: the bundled Eclipse Paho C is now v1.3.16 (was v1.3.13), and an installed Paho must be
  1.3.14 or later. 1.3.13 leaked the topic and payload of publishes still queued when the client was
  destroyed. The adapter now passes the HTTP proxy as `http://...`, which Paho 1.3.14+ requires.
  With `proxy` unset, Paho now reads `http_proxy` / `https_proxy` only when
  `PAHO_C_CLIENT_USE_HTTP_PROXY=TRUE` is set (it then honours `no_proxy`); 1.3.13 read them always.
- Paho adapter: a refused MQTT 5 PUBACK is reported with its reason code, the status
  `az_iot_mqtt_puback_result()` gives it (`AZ_IOT_ERR_PUBLISH_REFUSED`, `AZ_IOT_ERR_BUSY` for quota)
  and its Reason String; it was `AZ_IOT_ERR_MQTT` with no code. UNSUBACKs are now reported
  (`AZ_IOT_MQTT_EVT_UNSUBSCRIBE_ACK`). The conformance suite requires both of every adapter.

- Builds without Paho (`AZ_IOT_WITH_PAHO=OFF`): an MQTT adapter's header is installed only with
  the adapter. `AZ_IOT_BUILD_E2E` without Paho or az_mqtt, `AZ_IOT_BUILD_PKCS11_TESTS` without
  Paho, and the `AZ_IOT_BUILD_E2E_*` options without `AZ_IOT_BUILD_E2E`, fail the configure step
  instead of building nothing.
- Samples: each sample that connects is built once per MQTT adapter, Paho (`az_iot_sample_<...>`)
  and az_mqtt (`az_iot_sample_<...>_az_mqtt`, in `az_mqtt/`); `main.c` selects the adapter with
  `AZ_IOT_SAMPLE_ADAPTER_PAHO` or `AZ_IOT_SAMPLE_ADAPTER_AZ_MQTT`. Samples no longer drop out
  without Paho; `AZ_IOT_BUILD_SAMPLES=ON` with neither adapter fails the configure step. The
  SAS-key samples use mbedTLS when the OpenSSL crypto backend is not built.
- End-to-end tests: each suite that connects is built and run over Paho and over az_mqtt
  (`<suite>_az_mqtt`).
