# Release History

## 1.0.0-preview (Unreleased)

### Features Added

- Initial preview of the C SDK for Azure IoT Hub (mqttv3 and mqttv5).
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
- SAS token renewal: at `hub_auth.sas.renewal_percent` of a key-signed token's lifetime, the hub
  session reconnects with a new token; those state events carry `is_credential_renewal`. See
  [docs/connecting.md](docs/connecting.md#authentication).
- Breaking: the certificate provider's `load()` takes an index. A provider may offer several
  certificates per role (index 0, 1, ...; `AZ_IOT_ERR_NOT_FOUND` past the last); the client
  tries them in order when the service rejects one, before the SAS sources, and reports the index
  in `x509_index`. At most `AZ_IOT_MAX_CERTS_PER_ROLE` (default 4) per role. A provider with
  one certificate per role must return `AZ_IOT_ERR_NOT_FOUND` for `index > 0`.
- SAS tokens from the application: `on_sas_token_required` notifies, tried after the keys;
  `az_iot_connection_client_update_sas_token()` supplies a token for the request, or at any time
  (a connected hub session then renews with it). At renewal the token is requested while the hub
  session stays up; once supplied, the session disconnects and reconnects with it. See [docs/connecting.md](docs/connecting.md#authentication).
