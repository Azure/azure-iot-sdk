# az_mqtt

Zero-allocation MQTT client libraries in C for embedded and constrained environments, built on
the [Azure SDK for C](https://github.com/Azure/azure-sdk-for-c) span and platform abstractions.

## Libraries

| Library (CMake target) | Contents | Public headers |
|------------------------|----------|----------------|
| `az_mqtt_core` (`az_mqtt::core`) | Transport (TCP/TLS), packet framing, keep-alive, session state, wire primitives, result codes. Version-independent. | `az_mqtt/az_mqtt_core.h`, `az_mqtt_transport.h`, `az_mqtt_types.h` |
| `az_mqttv3` (`az_mqtt::mqttv3`) | MQTT 3.1.1 client API and codec. | `az_mqtt3/az_mqtt3_client.h`, `az_mqtt3_codec.h`, `az_mqtt3_types.h` |
| `az_mqttv5` (`az_mqtt::mqttv5`) | MQTT 5.0 client API and codec. | `az_mqtt5/az_mqtt5_client.h`, `az_mqtt5_codec.h`, `az_mqtt5_types.h` |

- Each version library links `az_mqtt_core`. An application links the version(s) it uses.
- A program using one version contains nothing of the other; CI checks this on every build.
- Both can be linked into one program: they share one core.
- Each API carries only its own protocol's fields: `az_mqtt3_*` types have no MQTT 5.0 properties.
- TLS: OpenSSL or mbedTLS (POSIX), Schannel (Windows), or none.

```c
#include <az_mqtt5/az_mqtt5_client.h>   /* or az_mqtt3/az_mqtt3_client.h */

az_mqtt5_client_options options = { 0 };
options.transport = transport;
options.hostname = AZ_SPAN_FROM_STR("broker.example.com");
options.port = 8883;
options.send_buffer = AZ_SPAN_FROM_BUFFER(send_buf);
options.receive_buffer = AZ_SPAN_FROM_BUFFER(recv_buf);
options.connect_options = az_mqtt5_connect_options_default();
/* ... TLS, callbacks, property buffers ... */
az_result rc = az_mqtt5_client_init(&client, &options);
```

### CMake options

| Option | Default | |
|--------|---------|-|
| `AZ_MQTT_ENABLE_MQTTV3` | `ON` | Build `az_mqttv3` |
| `AZ_MQTT_ENABLE_MQTTV5` | `ON` | Build `az_mqttv5` |
| `AZ_MQTT_TLS_BACKEND` | `auto` | `auto`, `openssl`, `mbedtls` or `none` |
| `AZ_MQTT_BUILD_SAMPLES` | `ON` | |
| `AZ_MQTT_BUILD_TESTS` | `ON` | |
| `AZ_MQTT_WARNINGS_AS_ERRORS` | `OFF` | |

### Migrating from the previous `az_mqtt5` / `az_mqtt3` trees

- Names common to both versions moved to the core. Rename `az_mqttN_` / `AZ_MQTTN_` to `az_mqtt_` / `AZ_MQTT_` for:
  - result codes (`AZ_MQTT_ERROR_*`), QoS (`az_mqtt_qos`), packet types, client state;
  - transport (`az_mqtt_transport_*`) and TLS options (`az_mqtt_tls_options*`).
- `az_mqttN_codec_decode_fixed_header` / `_encode_pingreq` are gone; the core handles both.
- mqttv5: otherwise unchanged. Includes stay `az_mqtt5/...`.
- mqttv3: the API now has MQTT 3.1.1 types only:
  - `connack.return_code` (`az_mqtt3_connack_return_code`);
  - `connect_options.clean_session`;
  - SUBACK `return_codes` is a view into the packet: no buffer to supply;
  - `az_mqtt3_ack_data` carries the packet id only, for PUBACK, PUBCOMP and UNSUBACK;
  - `az_mqtt3_client_disconnect(client)` takes no reason code;
  - removed: `buffers`, `on_disconnect`, properties, AUTH.
- mqttv3: `az_mqtt3_codec_decode_ack` rejects bytes after the packet identifier. An AUTH packet (reserved in 3.1.1) is a protocol error.
- CMake:
  - targets: `az_mqtt::mqttv5` / `az_mqtt::mqttv3` (were `az_mqtt5::client` / `az_mqtt3::client`);
  - options: `AZ_MQTT_*` (were `AZ_MQTT5_*` / `AZ_MQTT3_*`).

## Getting started

See [samples/README.md](samples/README.md): start a local broker, build, and run the connect
samples ([mqttv5](samples/az_mqtt5_sample_connect.c), [mqttv3](samples/az_mqtt3_sample_connect.c)).

## Documentation

| Document | Description |
|----------|-------------|
| [doc/eng/mqtt_v5_spec_compliance.md](doc/eng/mqtt_v5_spec_compliance.md) | MQTT 5.0 compliance matrix |
| [doc/eng/mqtt_v311_spec_compliance.md](doc/eng/mqtt_v311_spec_compliance.md) | MQTT 3.1.1 compliance matrix |
| [doc/eng/mqtt_v3_vs_v5_api.md](doc/eng/mqtt_v3_vs_v5_api.md) | Design decision: shared core, per-version libraries |
| [doc/api_design_decisions.md](doc/api_design_decisions.md) | Per-call vs per-session buffer design |
| [doc/thread_safety.md](doc/thread_safety.md) | Threading model |
| [doc/memory_footprint.md](doc/memory_footprint.md) | Memory footprint |

## CI and local validation

[.github/workflows/ci.yml](.github/workflows/ci.yml) runs on every pull request and on `main`.
Warnings in our code are errors in every job.

| Job | What it covers |
|-----|----------------|
| Linux | {OpenSSL, mbedTLS 3.6.7 / 4.1.1 / 4.2.0, no TLS} × {gcc, clang}: build, link-isolation check, all tests (mqttv3, mqttv5, and both linked together), including e2e against a local Mosquitto (plain and TLS) |
| Single version | Builds and tests with only mqttv3 or only mqttv5 enabled |
| Sanitizers | ASan + UBSan (+ leak check) over all tests |
| Hardened | Release build with `_FORTIFY_SOURCE=3`, stack protector, CET, full RELRO and PIE, verified on every executable, then all tests |
| Windows | MSVC `/W4 /WX`, Schannel, all tests |

The Linux steps are scripts, so the same run works locally:

```bash
eng/ci/start-broker.sh                                   # Mosquitto on 1883 (plain) and 8883 (TLS)
eng/ci/install-mbedtls.sh 3.6.7 "$PWD/build/mbedtls"     # only for the mbedTLS backend
MBEDTLS_PREFIX="$PWD/build/mbedtls" eng/ci/build-and-test.sh mbedtls   # [debug|asan|hardened]
```

## License

MIT — see [LICENSE](LICENSE).
