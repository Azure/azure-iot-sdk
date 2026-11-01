# az_mqtt (vendored)

MQTT 3.1.1 and MQTT 5 client libraries used by the az_mqtt adapter (`c/adapters/az_mqtt`).

- Source: az_mqtt commit `907a29a` (main), plus
  [ewertons/az_mqtt#58](https://github.com/ewertons/az_mqtt/pull/58) (`src/platform/tls_mbedtls.c`:
  wrap a PSA key that cannot export its public key, e.g. the ESP32 DS peripheral's on ESP-IDF
  v6.0.3 and later), not merged upstream yet. Re-vendor from upstream once it is.
- Imported: `CMakeLists.txt`, `LICENSE`, `README.md`, `doc/`, `inc/`, `src/`, and the ESP-IDF
  component files `port/`, `Kconfig`, `idf_component.yml` (used by
  [platforms/esp32](../../platforms/esp32/README.md) with `AZ_MQTT_BUILD_AZ_CORE=n`).
- Not imported: samples, tests, CI, and the `deps/azure-sdk-for-c` submodule. The SDK's
  azure-sdk-for-c is used instead, and samples and tests are off when built from here.

Built only with `AZ_IOT_WITH_AZ_MQTT=ON`.
