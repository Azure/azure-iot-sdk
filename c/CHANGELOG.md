# Release History

## 1.0.0-preview (Unreleased)

### Features Added

- Initial preview of the C SDK for Azure IoT Hub (mqttv3 and mqttv5).
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
