# az_mqtt (vendored)

MQTT 3.1.1 and MQTT 5 client libraries used by the az_mqtt adapter (`c/adapters/az_mqtt`).

- Source: az_mqtt commit `3a2c61b` (main), unmodified.
- Imported: `CMakeLists.txt`, `LICENSE`, `README.md`, `doc/`, `inc/`, `src/`, and the ESP-IDF
  component files `port/`, `Kconfig`, `idf_component.yml` (used by
  [platforms/esp32](../../platforms/esp32/README.md) with `AZ_MQTT_BUILD_AZ_CORE=n`).
- Not imported: samples, tests, CI, and the `deps/azure-sdk-for-c` submodule. The SDK's
  azure-sdk-for-c is used instead, and samples and tests are off when built from here.
- `port/esp-idf/component.cmake`, `Kconfig`: from ewertons/az_mqtt#57 (`5c425cb`), which drops the
  `deps/azure-sdk-for-c` include directory when `AZ_MQTT_BUILD_AZ_CORE=n`.

Built only with `AZ_IOT_WITH_AZ_MQTT=ON`.
