# az_mqtt (vendored)

MQTT 3.1.1 and MQTT 5 client libraries used by the az_mqtt adapter (`c/adapters/az_mqtt`).

- Source: az_mqtt commit `de7d21e` (main).
- Patched (`src/`, `inc/`), pending merge upstream:
  - repeated single-use MQTT 5 properties rejected (ewertons/az_mqtt#34, `07bcabe`);
  - in-flight entries kept for the advertised Receive Maximum (ewertons/az_mqtt#35, `dab59e3`);
  - PINGRESP with a body and disallowed acknowledgement codes rejected (ewertons/az_mqtt#36, `8687cf4`).
- Imported: `CMakeLists.txt`, `LICENSE`, `README.md`, `doc/`, `inc/`, `src/`.
- Not imported: samples, tests, CI, and the `deps/azure-sdk-for-c` submodule. The SDK's
  azure-sdk-for-c is used instead, and samples and tests are off when built from here.

Built only with `AZ_IOT_WITH_AZ_MQTT=ON`.
