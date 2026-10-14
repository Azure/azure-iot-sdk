# az_mqtt (vendored)

MQTT 3.1.1 and MQTT 5 client libraries used by the az_mqtt adapter (`c/adapters/az_mqtt`).

- Source: az_mqtt commit `58a0799` (main).
- Patched, pending merge upstream:
  - MQTT 5 User Properties without an array: `az_mqtt5_publish_data.properties` (the raw
    Properties of a received PUBLISH, none dropped), `az_mqtt5_codec_next_user_property()`, and
    `az_mqtt5_publish_options.user_property_next` / `user_property_context` (encode from a
    reader) (ewertons/az_mqtt#44). Files: `inc/az_mqtt5/az_mqtt5_types.h`, `inc/az_mqtt5/az_mqtt5_codec.h`,
    `src/mqtt5/az_mqtt5_codec.c`, `src/core/az_mqtt_codec_internal.h`.
- Imported: `CMakeLists.txt`, `LICENSE`, `README.md`, `doc/`, `inc/`, `src/`.
- Not imported: samples, tests, CI, and the `deps/azure-sdk-for-c` submodule. The SDK's
  azure-sdk-for-c is used instead, and samples and tests are off when built from here.

Built only with `AZ_IOT_WITH_AZ_MQTT=ON`.
