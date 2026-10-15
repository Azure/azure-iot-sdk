# Release History

## 1.0.0-preview (Unreleased)

### Features Added

- Initial preview of the C SDK for Azure IoT Hub (mqttv3 and mqttv5).
- mqttv5 custom topics (`az_iot_custom_topic_client.h`): publish to the topics an mqttv5 hub's topic
  templates allow, at QoS 0 or 1, with content type, user properties and message expiry.
  `az_iot_mqttv5_custom_topic_client_format_topic()` replaces `{deviceId}` with the device id.
  Topics the hub would refuse are rejected locally. Publish only. Sample:
  [samples/mqttv5/custom_topic_publisher](samples/mqttv5/custom_topic_publisher/README.md).
- az_mqtt MQTT adapter (`AZ_IOT_WITH_AZ_MQTT=ON`, `az_iot_adapter_az_mqtt.h`): MQTT 3.1.1 and 5
  over the bundled `deps/az_mqtt` client, with no adapter thread (connect and receive in
  `process_loop()`, sends written when called); TLS through OpenSSL 3
  or mbedTLS (`AZ_IOT_AZ_MQTT_TLS`), Schannel on Windows; WebSockets, HTTP CONNECT proxy, and
  key references through an OpenSSL 3 provider. Per-client sizes are set when compiled, with or
  without CMake: a footprint (`AZ_IOT_AZ_MQTT_FOOTPRINT`), separate send and receive sizes, or a
  config header. See [docs/client-configuration.md](docs/client-configuration.md#az_mqtt-adapter-sizes).
- Logging for support diagnostics: rotating file sink (`az_iot_log_file_sink_open()`), lines
  `<UTC ISO 8601 time> [LEVEL] [component] [thread] [file:line] message` from the built-in sinks, connection configuration,
  state changes and retries at `INFO`, and a `...` marker on truncated messages.
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

### Bugs Fixed

- MQTT over WebSockets to an mqttv5 hub: the hub session now uses the hub's path `/mqtt`
  (`AZ_IOT_MQTT_MQTTV5_WEBSOCKET_PATH`) when `websocket_path` is NULL. The mqttv5 hub refuses
  `/$iothub/websocket` (HTTP 412). DPS and the mqttv3 hub keep `/$iothub/websocket`.
