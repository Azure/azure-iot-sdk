# az_mqtt (vendored)

MQTT 3.1.1 and MQTT 5 client libraries used by the az_mqtt adapter (`c/adapters/az_mqtt`).

- Source: az_mqtt commit `e2d75de` (main).
- Patched, pending merge upstream:
  - PINGRESP with a body, disallowed acknowledgement codes, bytes after MQTT 5 properties,
    invalid Topic Names and Filters (including wildcard placement), QoS 0 with DUP, undefined
    CONNACK codes, a CONNACK after connecting, an MQTT 5 AUTH, and SUBACK/UNSUBACK code counts
    other than the filters requested, and control characters in proxy and WebSocket hosts
    rejected (ewertons/az_mqtt#36, `0c802f0`);
  - mbedTLS 4 fallback finds tfpsacrypto; key loading stops on a store error (ewertons/az_mqtt#37, `7085050`).
- Imported: `CMakeLists.txt`, `LICENSE`, `README.md`, `doc/`, `inc/`, `src/`.
- Not imported: samples, tests, CI, and the `deps/azure-sdk-for-c` submodule. The SDK's
  azure-sdk-for-c is used instead, and samples and tests are off when built from here.

Built only with `AZ_IOT_WITH_AZ_MQTT=ON`.
