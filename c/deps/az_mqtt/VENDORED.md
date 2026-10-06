# az_mqtt (vendored)

MQTT 3.1.1 and MQTT 5 client libraries used by the az_mqtt adapter (`c/adapters/az_mqtt`).

- Source: az_mqtt commit `e2d75de` (main).
- Patched, pending merge upstream:
  - PINGRESP with a body, disallowed acknowledgement codes, bytes after MQTT 5 properties and
    invalid Topic Names, QoS 0 with DUP and undefined CONNACK codes rejected (ewertons/az_mqtt#36, `68b86aa`);
  - mbedTLS 4 fallback finds tfpsacrypto; key loading stops on a store error (ewertons/az_mqtt#37, `7085050`).
- Imported: `CMakeLists.txt`, `LICENSE`, `README.md`, `doc/`, `inc/`, `src/`.
- Not imported: samples, tests, CI, and the `deps/azure-sdk-for-c` submodule. The SDK's
  azure-sdk-for-c is used instead, and samples and tests are off when built from here.

Built only with `AZ_IOT_WITH_AZ_MQTT=ON`.
