# Release History

## 1.0.0-preview (Unreleased)

### Features Added

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
- Software updates: the update request, file URL, workflow and install-result types are the SDK's
  own; `az_iot_su.h` no longer includes azure-sdk-for-c's `az_iot_adu_client.h`, and an installed
  azure-sdk-for-c no longer needs the `AZ_IOT_SU_MAX_*` limits. Breaking:
  `az_iot_su_client_install_result` has no `step_results` (steps are in `az_iot_su_step_result`),
  and `AZ_IOT_SU_CLIENT_SERVICE_ACTION_APPLY_DEPLOYMENT` is an `az_iot_su_client_service_action`
  enumerator.
- Software updates: `az_iot_su_download_handler` and `az_iot_su_client_config_options::download_handlers`
  register handlers that build a file from a related file (e.g. a delta), matched by the manifest's
  `downloadHandler` id. `az_iot_su_client_init()` validates them; the client does not call them yet.
- Software updates: the parsed manifest keeps each file's `mimeType`, `properties`,
  `downloadHandler` id and related files (`relatedFiles`, delta updates), and the request accepts
  their `fileUrls`. New `az_iot_su_limits::max_related_files` (default
  `AZ_IOT_SU_DEFAULT_MAX_RELATED_FILES`, 40) bounds the related files kept; more are left out with
  a warning. Breaking: `az_iot_su_client_update_request::file_urls` is a pointer to
  `AZ_IOT_SU_FILE_URL_CAPACITY()` entries, `az_iot_su_parse_update_request()` takes an
  `az_iot_su_update_request_storage`, and `AZ_IOT_SU_WORKSPACE_SIZE()` and
  `AZ_IOT_SU_STATE_BLOB_MAX_SIZE()` take `max_related_files`.
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
  `AZ_IOT_DEFAULT_MAX_UNCONFIRMED_DELIVERY_TIMEOUT_MS` (300000); it cannot be disabled. It applies
  to mqttv5 direct methods: a result for an exec with no Message Expiry now has a deadline (receipt
  time plus this value), and a queued abandon or probe-ack rejection with no budget is dropped
  after it. Both were kept and resent without limit. Such an exec is abandoned as insufficient time
  unless the timeout exceeds the method's declared minimum. The Message Expiry sent is unchanged
  (none).
- Software updates: compatibility-property strings are no longer capped at 256 bytes; the only limit
  is `az_iot_su_limits::device_properties_size`. The installed update ID is capped at
  `AZ_IOT_SU_APPLIED_UPDATE_ID_SIZE` (192 bytes, as before). A set that does not fit is refused with
  `AZ_IOT_ERR_NOT_ENOUGH_SPACE` and an `ERROR` log of the bytes needed; properties whose strings
  point into the client's copy are refused with `AZ_IOT_ERR_INVALID_ARG`. The update-check body
  is sized with the longest installed ID, fully escaped, so the applied ID adopted after a
  successful update always fits `max_message_size`. The update check's copy
  moves from `az_iot_su_client` (432 bytes smaller on 64-bit) into the workspace, which now
  counts `device_properties_size` twice plus `AZ_IOT_SU_APPLIED_UPDATE_ID_SIZE` (default: 1216
  bytes, was 512).
- Breaking: connection client entry counts are run-time limits
  (`az_iot_connection_client_options::limits`, 0 = default) instead of compile-time macros. The
  entries live in `options.workspace`, sized by `az_iot_connection_client_get_workspace_size()` or
  `AZ_IOT_CONNECTION_CLIENT_WORKSPACE_SIZE()`, which is required: `init()` returns
  `AZ_IOT_ERR_INVALID_ARG` without one. `AZ_IOT_MAX_PENDING_PUBACKS`, `AZ_IOT_MAX_PUBACK_RESERVATIONS`,
  `AZ_IOT_MAX_PERSISTENT_SUBS`, `AZ_IOT_MAX_SESSION_HANDLERS`, `AZ_IOT_MAX_FEATURE_STATE_OBSERVERS`,
  `AZ_IOT_MAX_APP_STATE_OBSERVERS`, `AZ_IOT_MAX_FEATURE_CLIENT_BINDS` and
  `AZ_IOT_MAX_INBOUND_HANDLERS` are removed. `AZ_IOT_MAX_MQTT_FACTORIES`, `AZ_IOT_DPS_HOST_BUF`,
  `AZ_IOT_DPS_DEVICE_ID_BUF`, `AZ_IOT_DPS_OPERATION_ID_MAX`, `AZ_IOT_CONNECTION_PROFILE_RAW_BUF`,
  `AZ_IOT_MQTT_USERNAME_BUF`, `AZ_IOT_PERSISTENT_SUB_TOPIC_MAX` and `AZ_IOT_DISPATCH_PREFIX_MAX` are
  fixed. Defining any of them before including the headers is a compile error, so no build of the
  application can lay out the client differently from the SDK. The defaults are compile-time
  values (`AZ_IOT_CONNECTION_CLIENT_DEFAULT_MAX_*`, `AZ_IOT_CONNECTION_CLIENT_CONFIG_FILE`, or the
  CMake options of the same names); `AZ_IOT_CONNECTION_CLIENT_DEFAULT_WORKSPACE_SIZE` sizes a static
  workspace for them.
- Breaking: `az_iot_su_platform_hooks` fields `persist_state_fn` and `load_state_fn` are renamed
  `persist_workflow_state_fn` and `load_workflow_state_fn`.
- az_mqtt adapter in application memory: `az_iot_az_mqtt_get_adapter()` returns an
  `az_iot_mqtt_adapter` whose `init()` builds an MQTT 3.1.1 or MQTT 5 client in a workspace the
  application provides, with no allocation. Sizes are run-time values (`az_iot_mqtt_limits`): send
  and receive buffers, in-flight messages, message store and connect-string copies.
  `az_iot_az_mqtt_get_workspace_size()` gives the exact size; `AZ_IOT_AZ_MQTT_WORKSPACE_SIZE()` a
  constant-expression bound for static arrays. The client's `destroy()` wipes the workspace and
  frees nothing. The factories are unchanged; they now allocate one workspace per client.
- Paho adapter in application memory: `az_iot_paho_get_adapter()` returns an
  `az_iot_mqtt_adapter` descriptor. The workspace holds the adapter's own state
  (`AZ_IOT_PAHO_WORKSPACE_SIZE`); Paho still allocates its internals and sizes its own buffers,
  so the limits are only checked. `destroy()` releases those internals, wipes the workspace
  and frees nothing else. The factories are unchanged.
- az_mqtt adapter: `AZ_IOT_AZ_MQTT_MESSAGE_STORAGE_SIZE` may be 0, for devices that only use clean
  sessions: no message store is allocated, and a connect whose session outlives the connection
  (`clean_start` false; MQTT 5: with `session_expiry_seconds` > 0) returns `AZ_IOT_ERR_NOT_SUPPORTED`.
- Retry policy: `az_iot_retry_policy::jitter_mode`. `AZ_IOT_RETRY_JITTER_ADDITIVE` only lengthens a
  delay (+0..`jitter_pct`); the default `AZ_IOT_RETRY_JITTER_SYMMETRIC` keeps the existing ± jitter.
- Initial preview of the C SDK for Azure IoT Hub (mqttv3 and mqttv5).
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
- mqttv5 custom topics (`az_iot_custom_topic_client.h`): publish to the topics an mqttv5 hub's topic
  templates allow, at QoS 0 or 1, with content type, user properties and message expiry.
  `az_iot_mqttv5_custom_topic_client_format_topic()` replaces `{deviceId}` with the device id.
  Topics the hub would refuse are rejected locally. Publish only. At QoS 1 the completion
  (`az_iot_publish_result`) carries the hub's PUBACK reason code and Reason String. Sample:
  [samples/mqttv5/custom_topic_publisher](samples/mqttv5/custom_topic_publisher/README.md).
- az_mqtt MQTT adapter (`AZ_IOT_WITH_AZ_MQTT=ON`, `az_iot_adapter_az_mqtt.h`): MQTT 3.1.1 and 5
  over the bundled `deps/az_mqtt` client, with no adapter thread (connect and receive in
  `process_loop()`, sends written when called); TLS through OpenSSL 3
  or mbedTLS (`AZ_IOT_AZ_MQTT_TLS`), Schannel on Windows (X.509 and in-memory PEM included);
  WebSockets, HTTP CONNECT proxy, and key references through an OpenSSL 3 provider. Per-client sizes are set when compiled, with or
  without CMake: a footprint (`AZ_IOT_AZ_MQTT_FOOTPRINT`), separate send and receive sizes, or a
  config header. See [docs/client-configuration.md](docs/client-configuration.md#az_mqtt-adapter-sizes).
- Logging for support diagnostics: rotating file sink (`az_iot_log_file_sink_open()`), lines
  `<UTC ISO 8601 time> [LEVEL] [component] [thread] [file:line] message` from the built-in sinks, connection configuration,
  state changes and retries at `INFO`, and a `...` marker on truncated messages.
- Breaking: software updates has no device-side cancel. `az_iot_su_is_cancelled()` and
  `AZ_IOT_SU_RESULT_CANCELLED` are removed; a cancel is service-side only.
  `AZ_IOT_SU_OUTCOME_CANCELED` stays as a reported value.
- Breaking: log calls and sinks take a component. `AZ_IOT_LOG_*(component, msg)`,
  `az_iot_log_emit*(level, component, file, line, ...)` and `az_iot_log_sink_callback` gain a
  `component` argument; SDK values are the `AZ_IOT_LOG_COMPONENT_*` macros in `az_iot_log_components.h`. See [docs/logging.md](docs/logging.md).
- Breaking: every connection attempt now starts in the new `AZ_IOT_CONN_STATE_SETTING_UP`, so an
  attempt that fails on the device (credential load, SAS signing, missing factory, buffer too
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
- PUBACK reason codes: `az_iot_mqtt_puback_result()` maps a PUBACK code to a result. An mqttv5
  refusal the broker will repeat (`0x87`, `0x90`, `0x99`), or `0x95` for a publish over the
  server's Maximum Packet Size, is the new `AZ_IOT_ERR_PUBLISH_REFUSED`, quota exceeded (`0x97`) is `AZ_IOT_ERR_BUSY`, other failures stay
  `AZ_IOT_ERR_MQTT`. The az_mqtt adapter uses it; publish callbacks receive it unchanged, and the
  connection client logs the code of a failed PUBACK.
- Software updates: `az_iot_su_client_set_step_result_details()` sets the `resultDetails` reported
  for a step, from a hook or between `do_work()` calls. The text is copied into the workspace
  (`az_iot_su_limits::result_details_size`, default 128 bytes per step, at most 1024), persisted with
  the step (persisted-state format 3; earlier-format records are ignored) and reported with the terminal
  result; a report over `max_message_size` is sent without step details.
  `AZ_IOT_SU_EVENT_REPORT_ACCEPTED` carries the accepted terminal report, as sent, in `event->report`.
  `AZ_IOT_SU_WORKSPACE_SIZE()` and `AZ_IOT_SU_STATE_BLOB_MAX_SIZE()` take `result_details_size`.
- Breaking: software updates memory comes from one application-provided workspace.
  `az_iot_su_client_config_options` replaces `device_properties_buffer` /
  `device_properties_buffer_size` with `limits` (`az_iot_su_limits`: `max_update_size`,
  `scratch_size`, `max_message_size`, `device_properties_size`) and `workspace` / `workspace_size`,
  sized with `az_iot_su_workspace_size()` or `AZ_IOT_SU_WORKSPACE_SIZE()`. The update copy, the
  persisted-state records, the verification scratch (was the client's persist scratch), the
  message body and the property strings live there; `az_iot_su_client` drops from
  about 39 KiB to about 3.5 KiB and no longer depends on them. `max_update_size` defaults to
  65536, the service's response ceiling (was `AZ_IOT_SU_REQUEST_BUFFER_SIZE`, 16384). `max_message_size` is at
  least `AZ_IOT_SU_MIN_MESSAGE_SIZE` (2048), which fits the largest status report. The workflow
  record is serialized in place around the update copy, so the separate persist scratch is gone.
  Removed: `AZ_IOT_SU_REQUEST_BUFFER_SIZE`, `AZ_IOT_SU_PERSIST_BLOB_SIZE`,
  `AZ_IOT_SU_DEVICE_PROPERTIES_STORAGE()`. `AZ_IOT_SU_STATE_BLOB_MAX_SIZE(max_update_size)` takes the
  limit. `az_iot_su_client_resume()` returns `AZ_IOT_ERR_BUSY` while a workflow is in progress. A
  file URL longer than `scratch_size` is refused with `AZ_IOT_ERR_NOT_ENOUGH_SPACE`.
- Software updates: `AZ_IOT_SU_REQUEST_BUFFER_SIZE` defaults to 16384 (was 4096), so offers above
  4 KiB are no longer refused; the client struct grows by about 24 KiB. The manifest signature is
  decoded into the client's own scratch, mostly in place, rather than fixed stack buffers: a
  signature that fits the request buffer verifies unless its SJWK header alone exceeds a quarter
  of it, and the verifier's stack drops from about 6.5 KiB to under 1 KiB.
  `az_iot_su_parse_update_request()` uses `AZ_IOT_SU_VERIFY_SCRATCH_SIZE` (8192) bytes of stack
  instead. A part that does not fit is logged as too large, not as invalid. The ESP32 sample sets
  the request buffer to 8192 and its NVS partition to 36 KB (erase the flash to move to it).
- Software updates events: entering Failed carries `reason` and `extended_result_code`; a manifest
  verification failure carries an `AZ_IOT_SU_MANIFEST_ERROR_*` sub-code (also in the report,
  was 0). New `AZ_IOT_SU_EVENT_NO_UPDATE` (check answered with no update),
  `AZ_IOT_SU_EVENT_UPDATE_DUPLICATE` (redelivery of the current workflow ignored) and
  `AZ_IOT_SU_EVENT_REPORT_ACCEPTED` (`report_terminal` marks the final report). An undecodable
  update now raises `AZ_IOT_SU_EVENT_UPDATE_REFUSED` with `AZ_IOT_ERR_PROTOCOL`.
- Software updates: the onboarding update check
  (`az_iot_su_client_request_onboarding_update()`) now sends `installedUpdateId` when the device
  properties set one, as the regular check does, so the service does not offer an update that is
  already installed. It is still omitted when nothing is installed.
- Breaking: software updates root keys and observers are run-time limits in the workspace:
  `az_iot_su_limits::max_root_keys` and `max_observers` (defaults
  `AZ_IOT_SU_DEFAULT_MAX_ROOT_KEYS` 8 and `AZ_IOT_SU_DEFAULT_MAX_OBSERVERS` 4, at most
  `AZ_IOT_SU_MAX_LIMIT_COUNT`). `AZ_IOT_SU_WORKSPACE_SIZE()` takes them as two more arguments.
  `AZ_IOT_SU_MAX_ROOT_KEYS`, `AZ_IOT_MAX_SU_OBSERVERS` and the unused `AZ_IOT_SU_MAX_WORKFLOW_ID_LEN`
  are removed; `AZ_IOT_SU_WORKFLOW_ID_SIZE` (now 128, the service's maximum; was 64) and
  `AZ_IOT_SU_RETRY_TIMESTAMP_SIZE` (64) are fixed. Defining any of the five is a compile error.
  `AZ_IOT_SU_MIN_MESSAGE_SIZE` and the default `max_message_size` grow to 2176 + 128 per step (3456)
  so the largest status report still fits.
- Software updates: `az_iot_su_client_init()` is now inline and returns `AZ_IOT_ERR_INVALID_ARG`,
  with an `ERROR` log and the client untouched, when `sizeof(az_iot_su_client)` or any of
  `AZ_IOT_SU_MAX_STEP_COUNT`, `AZ_IOT_SU_MAX_FILE_COUNT` or `AZ_IOT_SU_MAX_FILE_COUNT_PER_STEP`
  differs between the application and SDK builds. The exported symbol is
  `az_iot_su_client_init_checked()`, taking an `az_iot_su_client_layout`.
- Breaking: the connection client no longer reads `AZ_IOT_HUB_MQTT_V5_MOCK_ENDPOINT`,
  `AZ_IOT_DEVICE_ID` or `AZ_IOT_DPS_CONNECTION_PROFILE_OVERRIDE`. DPS now returns
  `connectionProfile`; to connect to an mqttv5 hub without DPS, set `opts.iothub_hostname`,
  `opts.device_id`, `opts.port` and `opts.connection_profile = AZ_IOT_CONNECTION_PROFILE_MQTT_V5`. The local mqttv5 hub mock
  scripts (`tests/manual`) are removed.
- Breaking: mqttv5 telemetry sends `$.cid` as the native MQTT v5 Correlation Data (UTF-8, at
  most 65535 bytes, else `AZ_IOT_ERR_INVALID_ARG`) instead of a user property; the hub reads the
  correlation id only from there. Telemetry `send()` on both generations returns
  `AZ_IOT_ERR_NOT_CONNECTED` (was `AZ_IOT_ERR_NOT_INITIALIZED`) before a provisioning connection
  has a device id. The delivery contract is documented in [docs/telemetry.md](docs/telemetry.md).
- Build against installed dependencies: `AZ_IOT_USE_INSTALLED_AZURE_SDK_FOR_C` and
  `AZ_IOT_USE_INSTALLED_PAHO` (both `OFF`) use installed azure-sdk-for-c and Eclipse Paho C instead
  of fetching them. `c/vcpkg.json` has a `builtin-baseline` and an `installed-paho` feature. See
  [docs/client-configuration.md](docs/client-configuration.md#installed-dependencies).
- Breaking: the application owns MQTT factories. `az_iot_mqtt_factory` loses `destroy`, and
  `az_iot_connection_client_deinit()` no longer destroys registered factories: registration only
  lends a factory to the client. Destroy each factory (e.g. `az_iot_paho_factory_destroy()`) after
  `az_iot_connection_client_deinit()`. See [docs/connecting.md](docs/connecting.md#mqtt-factory-ownership).
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
  `connection_profile` is now declared, so a feature client for the other hub generation fails
  with `AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH` instead of attaching to a session of the wrong
  generation. See [docs/connecting.md](docs/connecting.md#saved-hub-with-dps-fallback).
- Breaking: mqttv5 user properties are no longer copied into fixed arrays or capped.
  `az_iot_mqtt_message.user_properties` is now an `az_iot_mqtt_user_properties` view, read with
  `az_iot_mqtt_user_properties_next()` / `_find()` (`az_iot_mqtt_user_property_view`: UTF-8 with
  lengths, not NUL-terminated); `user_properties_count` is removed, and
  `az_iot_mqtt_user_properties_from_array()` wraps an array. Inbound, every property reaches the
  SDK; the az_mqtt adapter dropped those past `AZ_IOT_AZ_MQTT_USER_PROPERTIES_MAX` (4 or 16), which
  now limits CONNECT user properties only. mqttv5 telemetry sends every application property;
  `AZ_IOT_MQTTV5_TELEMETRY_MAX_USER_PROPERTIES` (16, extras dropped with a warning) is removed.
  MQTT adapters, including your own, must implement and read the view: see
  [docs/how_to_byo_mqtt_client.md](docs/how_to_byo_mqtt_client.md). The vendored az_mqtt is
  patched for it (`deps/az_mqtt/VENDORED.md`).
- Software updates: a `FAILED` workflow offered again under the same `workflowId` is
  retried after 1 h, doubling per consecutive failure up to 24 h, plus up to 10% jitter. An earlier
  offer raises the new `AZ_IOT_SU_EVENT_UPDATE_RETRY_WAIT` (`retry_after_ms`). The wait survives a
  restart; the persisted format moves to version 2, so version-1 records are ignored.
- Threading contract documented: the SDK does not lock its state against other threads, so every
  SDK call in a process must come from one thread or be serialized by the application
  ([docs/architecture.md](docs/architecture.md)). `tls.sign` and the certificate provider's
  `sign()` are called only on that thread. Sample
  [unified/multithreaded_telemetry](samples/unified/multithreaded_telemetry/README.md) hands
  readings from other threads to the SDK thread.
- ESP-IDF components in [platforms/esp32](platforms/esp32/README.md): the SDK, azure-sdk-for-c,
  the mbedTLS crypto backend and the software updates OTA platform hooks
  (`az_iot_su_esp32_ota.h`: `az_iot_su_esp32_ota_context`,
  `az_iot_su_esp32_ota_get_platform_hooks()`, `az_iot_su_esp32_ota_mark_running_image_valid()`).
- az_mqtt adapter on ESP-IDF (`azure-iot-sdk-az-mqtt` component): with no trusted CA it verifies
  the server against the ESP-IDF certificate bundle. The ESP32 software updates sample uses it
  instead of its own esp-mqtt adapter, and no longer depends on `espressif/mqtt`.
- ESP-IDF certificate provider over the `esp_secure_cert` partition
  (`azure-iot-sdk-certificate-provider-esp-secure-cert`, `espressif/esp_secure_cert_mgr` 2.9.3):
  `az_iot_certificate_provider_esp_secure_cert_init()`. With the Digital Signature peripheral the
  key is a PSA key (`client_key_psa_id`); otherwise the partition's PEM key, read per connection.
  The ESP32 software updates sample uses it instead of a certificate and key compiled into the
  image; its partition tables gain `esp_secure_cert` (0xD000; 0x11000 in the security profile),
  so devices running an earlier build of the sample must be reflashed over serial.
- ESP32 software updates sample: the network comes from ESP-IDF's `protocol_examples_common`
  (`example_connect()`), replacing the sample's own Wi-Fi code. The Wi-Fi settings are now
  `EXAMPLE_WIFI_SSID` / `EXAMPLE_WIFI_PASSWORD` (were `SU_WIFI_SSID` / `SU_WIFI_PASSWORD`).
  `sdkconfig.qemu` runs it in QEMU (esp32, esp32s3, esp32c3) over OpenCores Ethernet;
  `eng/esp-idf-qemu.sh` boots a build in QEMU and waits for a log line, and CI boots the QEMU
  builds until they get an IPv4 address.
- ESP32 IoT Hub sample [unified/esp32](samples/unified/esp32/README.md): DPS with the
  `esp_secure_cert` identity, then telemetry, the `echo` direct method and desired-twin
  acknowledgements on the assigned hub (mqttv3 or mqttv5). CI builds it for esp32, esp32c3,
  esp32s3 and esp32c6, and boots its QEMU build on esp32, esp32s3 and esp32c3.
- ESP32 end-to-end test: `ci-c-e2e` runs the ESP32 IoT Hub sample in QEMU on esp32 and esp32c3
  against an IoT Hub (telemetry, the `echo` method, a desired-version acknowledgement), with
  the esp32c3 key in the emulated Digital Signature peripheral. `eng/esp-idf-qemu-e2e.sh` runs it
  locally; `eng/esp-idf-qemu-identity.sh` writes a QEMU device identity
  ([docs](docs/eng/end-to-end-tests.md#esp32-in-qemu)).

### Bugs Fixed

- Connection client, events drained in one `process_loop()`: a failed acknowledgement (PUBACK,
  SUBACK, UNSUBACK) becomes connection error detail only when it fails the connection. A presence,
  gated or DPS SUBACK drained after a session-ending event is ignored; a FAILS_SELF refusal is still
  reported. A later end event of an ended session does not replace the server DISCONNECT's code,
  and an identity refusal is acted on whatever came first. While `close()` disconnects, a late
  SUBACK neither announces CONNECTED nor publishes the birth, and a new filter waits for the next
  session.
- ESP32: the `azure-sdk-for-c` component implements `az_platform_clock_msec()` and
  `az_platform_sleep_msec()` with `esp_timer` and FreeRTOS, so the az_mqtt adapter's idle wait
  sleeps instead of returning at once. The software updates sample's connect loop waits out each
  50 ms tick, so a DPS retry backoff no longer spins it through its bound, starving the idle task.
- Paho and az_mqtt adapters: `AZ_IOT_MQTT_EVT_DISCONNECTED` carries `AZ_IOT_OK` only for a
  completed `disconnect()`. A lost connection (Paho) and a server DISCONNECT with reason 0x00
  (`az_iot_mqtt_disconnect_result()`) now report `AZ_IOT_ERR_NOT_CONNECTED`.
- Paho adapter: `process_loop()` returns as soon as an event is queued and dispatches it, instead
  of sleeping its whole timeout and dispatching it on the next call.
- mqttv5: the connection's `process_loop()` wait ends by the presence SUBACK and birth-ack
  deadlines, so an ack that arrives in time is not taken for a timeout.
- mqttv5 twin: with `twin_push.push_desired` or `push_reported` set, a session whose twin-push
  (expected for the requested sections the birth-ack shows out of sync) does not arrive within the mqttv5 defensive timeout of hub CONNECTED (5 to 15 minutes, longer
  after consecutive misses, as for the birth-ack) is now ended with a DISCONNECT and reconnected (or
  faulted with `AZ_IOT_ERR_TIMEOUT` without a reconnection policy). Before, no timeout was armed and
  a lost push left the device without desired patches until the next reconnect.
- mqttv5: a malformed birth-ack is ignored instead of completing the handshake with partly
  decoded twin versions; the birth-ack timeout ends the session.
- mqttv5: a presence SUBACK or birth-ack timeout now ends the session with a DISCONNECT, as a SAS
  token renewal does, instead of dropping the connection.
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
- Software updates: updates with up to 10 steps and 10 files, the Device Update limits, are
  accepted; the azure-sdk-for-c limits they depend on defaulted to 2 and nothing set them. CMake
  options `AZ_IOT_SU_MAX_STEP_COUNT`, `AZ_IOT_SU_MAX_FILE_COUNT` and
  `AZ_IOT_SU_MAX_FILE_COUNT_PER_STEP` (10, 10, the file count) set them for azure-sdk-for-c,
  `az_iot_core` and, through its CMake and pkg-config packages, the application; smaller values
  save memory. An installed azure-sdk-for-c built with other values fails configuration. A manifest
  with more steps, or more hashes for a file, than its parsed form holds was written past those
  arrays; now any manifest over a limit fails the workflow with an `ERROR` log naming the limit, and
  more `fileUrls` than `AZ_IOT_SU_MAX_FILE_COUNT` raises `AZ_IOT_SU_EVENT_UPDATE_REFUSED` with
  `AZ_IOT_ERR_NOT_ENOUGH_SPACE` (was `AZ_IOT_ERR_PROTOCOL`). A file `mimeType` that is not a string
  fails the workflow as malformed (the parser would read its members as file properties).
  `AZ_IOT_SU_MIN_MESSAGE_SIZE` and the default `max_message_size` are 1792 + 128 per step (3072).
  `az_iot_su_client` grows from 4616 to 8584 bytes and the default workspace from 78920 to 85640
  (64-bit).
- Paho: the bundled v1.3.16 is patched with two upstream fixes for data races between the
  application thread and Paho's threads: `MQTTAsync_destroy()` against the send thread on the
  command queue (eclipse-paho/paho.mqtt.c#1709), and `MQTTAsync_connect()` against the timeout and
  retry checks on the client's state (#1714). An installed Paho is not patched. See
  [docs/dependencies.md](docs/dependencies.md#races-on-the-command-queue-and-client-state).
- mqttv5 direct methods: a result is no longer published at or after its deadline -- the exec's
  receipt time plus its budget, less the safety margin. `az_iot_mqttv5_direct_method_respond()`
  past it publishes nothing, releases the slot and returns `AZ_IOT_ERR_TIMEOUT`; a held result is
  dropped. Results were sent while a whole second of budget was left, ignoring the margin.
- mqttv5: the birth-ack timeout follows the presence schedule instead of a fixed 60 s. Armed after
  the birth PUBLISH, it waits 5–10 min, then 6–11, 8–13 and 10–15 min for consecutive misses (base
  plus uniform 0–5 min jitter); on expiry the client disconnects and reconnects. `open()` and a
  birth-ack in time reset it. `AZ_IOT_PRESENCE_BIRTH_ACK_TIMEOUT_MS` is removed; the presence
  `dev/#` SUBACK now uses `subscription_ack_timeout_seconds`
  (`AZ_IOT_DEFAULT_SUBSCRIPTION_ACK_TIMEOUT_SECONDS`, 60 s).
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
- mqttv5 direct methods: a probe-ack, result or abandon that cannot be published because the hub is
  not `CONNECTED` (for example between CONNACK and the birth-ack) is held with its ready token or
  slot and published on the next `CONNECTED`, instead of being dropped. `respond()` returns
  `AZ_IOT_OK` for a held result. The client now holds one result buffer per
  `AZ_IOT_MQTTV5_DM_MAX_CONCURRENT` slot.
- mqttv5 direct methods: a probe-ack, result or abandon that ends without a PUBACK (for example,
  the connection closed) is published again on the next `CONNECTED`, until its Message Expiry runs
  out (for a result, until its result deadline); one the broker refuses
  (`AZ_IOT_ERR_PUBLISH_REFUSED`) is not. A failed re-publish stays queued and is retried on the
  next `CONNECTED`, hub PUBACK or method message, never from the PUBACK that reported its own
  failure. A result keeps its slot until its PUBACK, and a probe-ack or
  result waits for a free pending-PUBACK slot instead of being sent untracked. Probe rejections and
  abandons are kept in a queue of `AZ_IOT_MQTTV5_DM_NOTICE_QUEUE_MAX` entries (default
  `AZ_IOT_MQTTV5_DM_MAX_CONCURRENT`). All are dropped when the hub or device id changes (for
  example after DPS re-provisioning).
- Software updates: a failed workflow or step now reports a non-positive `resultCode` (`-facility`,
  e.g. `-5` for an install failure), as the service contract requires; it was `700 - facility`.
- Software updates error handling: `UPDATE_ACCOUNT_ACCESS_DENIED` is handled like
  `UPDATE_ACCOUNT_NOT_LINKED`; `DEVICE_NOT_PROVISIONED`, `MAX_DEVICE_CLASSES_EXCEEDED` and
  `FORBIDDEN` stop the request. An untyped `409000` on a status report now raises
  `AZ_IOT_SU_EVENT_OPERATION_ABANDONED` instead of being taken as delivered.
  `OUTDATED_SERVICE_CONFIG` is no longer special-cased: the service never returns it.
- Software updates: a status report rejected as conflicting (`REPORT_CONFLICT` or an untyped
  `409000`) now makes the client check for updates once, as the service requires, after raising
  `AZ_IOT_SU_EVENT_OPERATION_ABANDONED`.
- Software updates: an update check requested while a workflow's terminal report is owed now waits
  for that report's verdict (sending it again if nothing else would), so the check no longer goes
  out before the service has the outcome. The wait counts against the check's timeout. If the
  report then loses its session for good, the check is abandoned (`AZ_IOT_ERR_NOT_CONNECTED`).
- Software updates: after a SUCCEEDED workflow the client adopts the applied update ID as the
  installed one (also on resuming its terminal record), so later checks advertise it without
  `az_iot_su_client_update_device_properties()`. That call no longer re-sends a settled terminal
  report; the next check carries the new properties.
- Software updates: new `AZ_IOT_SU_EVENT_WORKFLOW_COMPLETED`, raised once when a workflow's terminal
  report is settled (accepted, or retired by the service), with the report and the verdict. An
  application waiting for the end of a workflow no longer has to combine `REPORT_ACCEPTED` and
  `OPERATION_ABANDONED`.
- Software updates: new `az_iot_su_client_get_reboot_state()` tells whether `install_fn` or
  `apply_fn` asked for a reboot and whether storage already holds what `az_iot_su_client_resume()`
  needs (`AZ_IOT_SU_REBOOT_NONE` / `_PENDING` / `_READY`), so an application no longer tracks
  `PERSIST_FAILED` / `PERSIST_RECOVERED` itself to decide when to reboot. `PENDING` also covers
  storage behind with no reboot asked for, so it gates any planned exit.

- MQTT over WebSockets to an mqttv5 hub: the hub session now uses the hub's path `/mqtt`
  (`AZ_IOT_MQTT_HUB_V5_WEBSOCKET_PATH`) when `websocket_path` is NULL. The mqttv5 hub refuses
  `/$iothub/websocket` (HTTP 412). DPS and the mqttv3 hub keep `/$iothub/websocket`.
- Builds without Paho (`AZ_IOT_WITH_PAHO=OFF`): an MQTT adapter's header is installed only with
  the adapter. `AZ_IOT_BUILD_E2E` without Paho or az_mqtt, `AZ_IOT_BUILD_PKCS11_TESTS` without
  Paho, and the `AZ_IOT_BUILD_E2E_*` options without `AZ_IOT_BUILD_E2E`, fail the configure step
  instead of building nothing.
- Samples: each sample that connects is built once per MQTT adapter, Paho (`az_iot_sample_<...>`)
  and az_mqtt (`az_iot_sample_<...>_az_mqtt`, in `az_mqtt/`); `main.c` selects the adapter with
  `AZ_IOT_SAMPLE_ADAPTER_PAHO` or `AZ_IOT_SAMPLE_ADAPTER_AZ_MQTT`. Samples no longer drop out
  without Paho; `AZ_IOT_BUILD_SAMPLES=ON` with neither adapter fails the configure step. The
  SAS-key and Software Update samples use mbedTLS when the OpenSSL crypto backend is not built.
- End-to-end tests: each suite that connects is built and run over Paho and over az_mqtt
  (`<suite>_az_mqtt`).
