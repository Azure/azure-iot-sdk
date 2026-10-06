# az_mqtt (vendored)

MQTT 3.1.1 and MQTT 5 client libraries used by the az_mqtt adapter (`c/adapters/az_mqtt`).

- Source: az_mqtt commit `7605ae9` (main).
- Patched, pending merge upstream:
  - UTF-8 validation of received strings (ewertons/az_mqtt#29, `1e5e26a`);
  - inbound QoS 2 without a free in-flight entry ends the session (ewertons/az_mqtt#30, `93a6218`).
- Imported: `CMakeLists.txt`, `LICENSE`, `README.md`, `doc/`, `inc/`, `src/`.
- Not imported: samples, tests, CI, and the `deps/azure-sdk-for-c` submodule. The SDK's
  azure-sdk-for-c is used instead, and samples and tests are off when built from here.

Built only with `AZ_IOT_WITH_AZ_MQTT=ON`.
