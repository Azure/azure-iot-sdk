# az_mqtt (vendored)

MQTT 3.1.1 and MQTT 5 client libraries used by the az_mqtt adapter (`c/adapters/az_mqtt`).

- Source: az_mqtt commit `febea09` (main).
- Patched (`src/`, `inc/`), pending merge upstream:
  - inbound QoS 2 without a free in-flight entry ends the session (ewertons/az_mqtt#30, `aa56b7a`);
  - one WebSocket send deadline per frame (ewertons/az_mqtt#31, `09e1514`);
  - malformed CONNACK flags and packet identifier 0 rejected (ewertons/az_mqtt#32, `80c3917`).
- Imported: `CMakeLists.txt`, `LICENSE`, `README.md`, `doc/`, `inc/`, `src/`.
- Not imported: samples, tests, CI, and the `deps/azure-sdk-for-c` submodule. The SDK's
  azure-sdk-for-c is used instead, and samples and tests are off when built from here.

Built only with `AZ_IOT_WITH_AZ_MQTT=ON`.
