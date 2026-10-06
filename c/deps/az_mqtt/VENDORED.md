# az_mqtt (vendored)

MQTT 3.1.1 and MQTT 5 client libraries used by the az_mqtt adapter (`c/adapters/az_mqtt`).

- Source: az_mqtt commit `7605ae9` (main).
- Patched, pending merge upstream:
  - UTF-8 validation of received and sent strings, MQTT 5 AUTH decoded (ewertons/az_mqtt#29, `72c5a7a`);
  - inbound QoS 2 without a free in-flight entry ends the session (ewertons/az_mqtt#30, `40d44e8`);
  - one WebSocket send deadline per frame (ewertons/az_mqtt#31, `5d6873b`);
  - malformed CONNACK flags and PUBLISH packet identifier 0 rejected (ewertons/az_mqtt#32, `220c9f4`).
- Imported: `CMakeLists.txt`, `LICENSE`, `README.md`, `doc/`, `inc/`, `src/`.
- Not imported: samples, tests, CI, and the `deps/azure-sdk-for-c` submodule. The SDK's
  azure-sdk-for-c is used instead, and samples and tests are off when built from here.

Built only with `AZ_IOT_WITH_AZ_MQTT=ON`.
