# az_mqtt

Zero-allocation MQTT client libraries in C for embedded and constrained environments, built on
the [Azure SDK for C](https://github.com/Azure/azure-sdk-for-c) span and platform abstractions.

## Libraries

| Library (CMake target) | Contents | Public headers |
|------------------------|----------|----------------|
| `az_mqtt_core` (`az_mqtt::core`) | Transport (TCP/TLS, WebSocket layer), packet framing, keep-alive, session state, wire primitives, result codes. Version-independent. | `az_mqtt/az_mqtt_core.h`, `az_mqtt_transport.h`, `az_mqtt_websocket.h`, `az_mqtt_types.h` |
| `az_mqttv3` (`az_mqtt::mqttv3`) | MQTT 3.1.1 client API and codec. | `az_mqtt3/az_mqtt3_client.h`, `az_mqtt3_codec.h`, `az_mqtt3_types.h` |
| `az_mqttv5` (`az_mqtt::mqttv5`) | MQTT 5.0 client API and codec. | `az_mqtt5/az_mqtt5_client.h`, `az_mqtt5_codec.h`, `az_mqtt5_types.h` |

- Each version library links `az_mqtt_core`. An application links the version(s) it uses.
- A program using one version contains nothing of the other; CI checks this on every build.
- Both can be linked into one program: they share one core.
- Each API carries only its own protocol's fields: `az_mqtt3_*` types have no MQTT 5.0 properties.
- TLS: OpenSSL or mbedTLS (POSIX), Schannel (Windows), or none.
- Connect is blocking (`az_mqttN_client_connect`) or not (`az_mqttN_client_connect_start`, then
  `az_mqttN_client_process_loop` until CONNECTED; only name resolution may block).
- Requests awaiting acknowledgement are tracked in caller storage
  (`options.inflight_control_buffer`, 6 B per `az_mqtt_inflight_entry`): unique packet identifiers,
  QoS 2 duplicate detection, acknowledgements for unknown identifiers ignored. With no free entry a
  request fails with `AZ_MQTT_ERROR_FLOW_CONTROL`.
- Session resumption: with a session that outlives the connection (mqttv3 Clean Session 0;
  mqttv5 Clean Start 0 and Session Expiry Interval > 0), each QoS 1/2 PUBLISH is stored in caller
  storage (`options.inflight_message_buffer`: its size + `AZ_MQTT_INFLIGHT_MESSAGE_OVERHEAD`, until
  PUBACK or PUBREC; required for QoS 1/2 then, else `AZ_MQTT_ERROR_INVALID_CONFIG`; full:
  `AZ_MQTT_ERROR_OUT_OF_STORAGE`). Clean sessions need none of it.
  If the next accepted CONNACK has Session Present, the client resends as MQTT requires: PUBRELs,
  then each PUBLISH, oldest first, with DUP and its original packet identifier (mqttv5: within the
  new Receive Maximum, the rest as acknowledgements free room). Dropped instead, and reported to
  `on_puback` / `on_pubcomp` with `ack->status`: one whose message expiry interval elapsed (opt-in,
  `AZ_MQTT_ERROR_MESSAGE_EXPIRED`), mqttv5 one over the new Maximum Packet Size
  (`AZ_MQTT_ERROR_PACKET_TOO_LARGE`), and, without Session Present, every outgoing QoS 1/2 exchange
  (`AZ_MQTT_ERROR_SESSION_NOT_RESUMED`). SUBSCRIBE/UNSUBSCRIBE in flight are abandoned when the
  connection ends. Session state is kept in memory only, not across a restart.
- mqttv5 enforces the CONNACK Receive Maximum, Maximum QoS, Retain Available, Topic Alias Maximum
  and Maximum Packet Size.
- Transport failures have distinct results: `AZ_MQTT_ERROR_NAME_RESOLUTION`,
  `_CONNECTION_REFUSED`, `_TLS_HANDSHAKE`, `_TLS_VERIFY`, `_CONNECTION_CLOSED`, `_TIMEOUT`
  (`_TRANSPORT` for anything else). The client option `on_transport_error` receives each platform
  error behind them, in order (errno/WSA, EAI_*, OpenSSL/mbedTLS/Schannel, certificate
  verification), with the result it leads to and its connect attempt: every address that failed,
  every queued OpenSSL error. The first with the returned result is the cause.
- HTTP CONNECT proxy: `options.proxy_options` (or `az_mqtt_transport_set_proxy()`): host, port,
  optional Basic credentials. TLS runs inside the tunnel and verifies the server, not the proxy.
  Never taken from the environment; a proxy that cannot be reached or refuses the tunnel fails the
  connect (`AZ_MQTT_ERROR_PROXY`, `_PROXY_AUTH` for 407), never falling back to a direct
  connection. CMake `AZ_MQTT_ENABLE_PROXY=OFF` compiles it out.
- Transports are pluggable: `az_mqtt_transport` is an `az_mqtt_transport_vtable` implementation
  (connect, send, receive, shutdown, close, proxy, error callback); `az_mqtt_transport_*` calls
  dispatch to it. Layers wrap another transport. The platform transport is a stack of layers in
  the caller's storage: TLS (OpenSSL, mbedTLS or Schannel) over the HTTP CONNECT proxy over the
  socket. A TLS session still usable is ended with close_notify on any close (orderly disconnect
  or a failed session, e.g. keep-alive timeout); errors met doing so go to `on_transport_error`.
  A connection that failed is closed as is.
  The platform port provides `az_mqtt_transport_init()`, `_sizeof()`, `_clock_ms()` and
  `_random()`.
- MQTT over WebSockets (RFC 6455, subprotocol `mqtt`) is such a layer: `az_mqtt_websocket_init()`
  over the platform transport, then `options.transport = az_mqtt_websocket_get_transport(&ws)`.
  Path default `/mqtt` (Azure IoT Hub: `AZ_MQTT_WEBSOCKET_PATH_IOT_HUB`); TLS and proxy are the
  platform transport's; `port` is the WebSocket listener (typically 443 or 80). A refused or
  invalid upgrade, or a frame RFC 6455 forbids, fails with `AZ_MQTT_ERROR_WEBSOCKET`;
  `on_transport_error` gets the HTTP status or close code (`AZ_MQTT_NATIVE_ERROR_WEBSOCKET`).
  CMake `AZ_MQTT_ENABLE_WEBSOCKETS=OFF` compiles it out.
- Logging uses azure-sdk-for-c `az_log` (`az_log_set_message_callback`), classifications
  `AZ_LOG_MQTT_CONNECTION` and `AZ_LOG_MQTT_PACKET`: host, port, packet types and lengths, results
  and native codes; never credentials, keys, certificates, topics or payloads. azure-sdk-for-c
  `LOGGING=OFF` compiles it out.

```c
#include <az_mqtt5/az_mqtt5_client.h>   /* or az_mqtt3/az_mqtt3_client.h */

az_mqtt5_client_options options = { 0 };
options.transport = transport;
options.hostname = AZ_SPAN_FROM_STR("broker.example.com");
options.port = 8883;
options.send_buffer = AZ_SPAN_FROM_BUFFER(send_buf);
options.receive_buffer = AZ_SPAN_FROM_BUFFER(recv_buf);
options.inflight_control_buffer = az_span_create((uint8_t*)inflight_entries, (int32_t)sizeof(inflight_entries));
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
| `AZ_MQTT_ENABLE_PROXY` | `ON` | HTTP CONNECT proxy support |
| `AZ_MQTT_ENABLE_WEBSOCKETS` | `ON` | MQTT over WebSockets |
| `AZ_MQTT_BUILD_SAMPLES` | `ON`; `OFF` as a subproject | |
| `AZ_MQTT_BUILD_TESTS` | `ON`; `OFF` as a subproject | |
| `AZ_MQTT_WARNINGS_AS_ERRORS` | `OFF` | |
| `AZ_MQTT_BUILD_FUZZERS` | `OFF` | libFuzzer targets and their drop folder ([tests/fuzz](tests/fuzz/README.md)): clang (ASan, UBSan) or MSVC (ASan) |

In another CMake project, `add_subdirectory()` this directory and link `az_mqtt::mqttv3` and/or
`az_mqtt::mqttv5`. If the project already builds azure-sdk-for-c (target `az_core`), az_mqtt uses
it instead of `deps/azure-sdk-for-c`.

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
- mqttv5: a received AUTH packet is a protocol error (enhanced authentication is not implemented).
- mqttv5: `options.buffers` (7 spans) is replaced by `options.decode_user_properties`
  (`az_mqtt5_user_property[]`) and `options.decode_codes` (`int32_t[]`: subscription identifiers or
  reason codes), each shared by every received packet type.
- `az_mqttN_client_process_loop` called from a callback of a received packet of the same session
  returns `AZ_MQTT_ERROR_INVALID_STATE` (that packet is still being handled). Reconnecting from a
  callback is unaffected.
- `az_mqtt_inflight_entry` is 6 B (was 4 B): it keeps the filter count of a SUBSCRIBE or
  UNSUBSCRIBE. A SUBACK (MQTT 5: or UNSUBACK) with a different number of codes is a protocol error.
- Transport failures that were `AZ_MQTT_ERROR_TRANSPORT` may now be one of the specific results
  above.
- `options.inflight_control_buffer` is required for QoS 1/2 publish, subscribe and unsubscribe;
  without it they fail with `AZ_MQTT_ERROR_FLOW_CONTROL`. `on_puback`, `on_pubcomp`, `on_suback` and `on_unsuback`
  fire only for identifiers in flight.
- mqttv5: publish fails with `AZ_MQTT_ERROR_NOT_SUPPORTED` above the server's Maximum QoS, when
  it is retained and Retain Available is 0, or when `topic_alias` exceeds the server's Topic Alias
  Maximum (0 if not sent); with `AZ_MQTT_ERROR_FLOW_CONTROL` at its Receive Maximum; and with
  `AZ_MQTT_ERROR_PACKET_TOO_LARGE` above its Maximum Packet Size (also subscribe and unsubscribe;
  an acknowledgement above it closes the session with that result).
- CMake:
  - targets: `az_mqtt::mqttv5` / `az_mqtt::mqttv3` (were `az_mqtt5::client` / `az_mqtt3::client`);
  - options: `AZ_MQTT_*` (were `AZ_MQTT5_*` / `AZ_MQTT3_*`).

## Getting started

See [samples/README.md](samples/README.md): start a local broker, build, and run the samples
(TCP, QoS 0/1/2, TLS and mutual TLS, WebSockets, HTTP proxy, non-blocking connect; mqttv5
request/response), for [mqttv5](samples/az_mqtt5_sample_connect.c) and
[mqttv3](samples/az_mqtt3_sample_connect.c).

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
| Subproject | `add_subdirectory()` from a parent project, with the parent's azure-sdk-for-c and with the bundled one; fails if the parent's cache changes ([tests/subproject](tests/subproject/CMakeLists.txt)) |
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
