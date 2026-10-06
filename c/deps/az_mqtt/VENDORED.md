# az_mqtt (vendored)

MQTT 3.1.1 and MQTT 5 client libraries used by the az_mqtt adapter (`c/adapters/az_mqtt`).

- Source: az_mqtt commit `41369a1` (main).
- Patched (`src/`, `inc/`), pending merge upstream:
  - CONNACK, packet identifier 0 and MQTT 5 property value checks (ewertons/az_mqtt#32, `de7e89c`);
  - thread-safe one-time WSAStartup (ewertons/az_mqtt#33, `39f742f`).
- Imported: `CMakeLists.txt`, `LICENSE`, `README.md`, `doc/`, `inc/`, `src/`.
- Not imported: samples, tests, CI, and the `deps/azure-sdk-for-c` submodule. The SDK's
  azure-sdk-for-c is used instead, and samples and tests are off when built from here.

Built only with `AZ_IOT_WITH_AZ_MQTT=ON`.
