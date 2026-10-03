# Release History

## 1.0.0-preview (Unreleased)

### Features Added

- Initial preview of the C SDK for Azure IoT Hub (mqttv3 and mqttv5).
- Logging for support diagnostics: rotating file sink (`az_iot_log_file_sink_open()`), UTC
  timestamp and thread id on every built-in sink line, a `<component>: ` prefix on every SDK
  message, connection configuration, state changes and retries at `INFO`, and a `...` marker on
  truncated messages. See [docs/logging.md](docs/logging.md).
