# Dependencies

Third-party libraries the C SDK builds against, and the known issues of each.

| Library | Version | Used by | Needed |
| --- | --- | --- | --- |
| [azure-sdk-for-c](https://github.com/Azure/azure-sdk-for-c) | 1.5.0 (`AZ_SDK_C_TAG`) | Core, mqttv3 and mqttv5 clients | Always |
| [Eclipse Paho MQTT C](https://github.com/eclipse-paho/paho.mqtt.c) | v1.3.16 (`PAHO_C_TAG`); installed: 1.3.14+ | Paho adapter | `AZ_IOT_WITH_PAHO` |
| [OpenSSL](https://www.openssl.org/) | Any Paho supports for Paho TLS; 3.0+ for Paho key custody, az_mqtt TLS (`AZ_IOT_AZ_MQTT_TLS=openssl`), the OpenSSL crypto backend and the managed certificate provider | Those components | Only for the components listed; az_mqtt can use Mbed TLS, or Schannel on Windows |
| [Mbed TLS](https://github.com/Mbed-TLS/mbedtls) | 3.6 LTS or 4.1+ | mbedTLS crypto backend, az_mqtt TLS (`AZ_IOT_AZ_MQTT_TLS=mbedtls`) | Optional |
| az_mqtt | Vendored in [deps/az_mqtt](../deps/az_mqtt/VENDORED.md) | az_mqtt adapter | `AZ_IOT_WITH_AZ_MQTT` |
| [cmocka](https://gitlab.com/cmocka/cmocka) | 1.1.7 (`CMOCKA_TAG`) | Tests only | `AZ_IOT_BUILD_TESTS` |
| [libcurl](https://curl.se/libcurl/) | Any; 7.55.0+ for the software update download sample (`CURLINFO_SIZE_DOWNLOAD_T`) | Samples only (file upload, DPS TPM, software update download) | Optional |

Exact commits are in [cgmanifest.json](../cgmanifest.json). How to use installed copies instead of
the fetched ones: [Installed dependencies](client-configuration.md#installed-dependencies).

## Eclipse Paho MQTT C

Checked against v1.3.16. Re-check on every `PAHO_C_TAG` change.

### Minimum version 1.3.14

1.3.13 leaks the topic and payload of every publish still queued when `MQTTAsync_destroy()` runs
(`MQTTAsync_NULLPublishCommands` drops the pointers, then only the command is freed). It also
misreads the `http://` proxy form the adapter sends on the TLS slot.

**Mitigation:** none needed with the bundled version. With `AZ_IOT_USE_INSTALLED_PAHO`, CMake and
vcpkg refuse anything older than 1.3.14.

### Proxy from the environment is opt-in

When `az_iot_connection_client_options.proxy` is unset, Paho reads the lowercase `http_proxy` /
`https_proxy` variables only if `PAHO_C_CLIENT_USE_HTTP_PROXY=TRUE` is set. It then honours
`no_proxy`, and `https_proxy` must start with `http://` or `https://`, or the connect fails.
Uppercase spellings are ignored. 1.3.13 read both variables always.

**Mitigation:** set `proxy` in the connection options. The adapter then passes
`http://[user:password@]host:port` to Paho on both the plain and TLS slots, independent of the
environment. See [Network: WebSockets and proxies](connecting.md#network-websockets-and-proxies).

### Proxy credentials

Paho splits the proxy string at the first `@` and decodes `%XX`. A `%` not followed by two hex
digits makes its decoder loop without advancing (`Proxy_specialChars`), and the connect hangs.

**Mitigation:** with `proxy` set in the connection options, none needed: the adapter percent-encodes
`%` and `@` in the user name and password, and refuses a `:` in the user name
(`AZ_IOT_ERR_INVALID_ARG`), which HTTP Basic cannot represent. A proxy URL taken from the
environment reaches Paho unchanged: write `%` as `%25` and `@` as `%40` in its credentials.

### Connect timeout

Paho computes `connectTimeout * 1000` in `int`, which overflows above `INT_MAX / 1000` seconds.

**Mitigation:** none needed. The adapter clamps `connect_timeout_seconds` to `INT_MAX / 1000`
(about 24.8 days). `0` keeps Paho's 30 s default.

### Threads and unlocked state

Paho runs its own send and receive threads. Some of their shared state is read and written without
a lock: `MQTTAsync_tostop` and `sendThread_state` (`MQTTAsync_connect`, `MQTTAsync_stop`,
`MQTTAsync_sendThread`) and `sendThread_id` / `receiveThread_id` (`MQTTAsync_inCallback`).
helgrind and DRD report these. Paho runs callbacks and `MQTTAsync_destroy()` under one global
mutex, so no callback reaches the adapter after `destroy()` returns.

**Mitigation:** call the SDK from one thread, as documented in
[Architecture](architecture.md). The adapter hands Paho's callbacks to that thread through a
locked queue. The race CI suppresses only these Paho frames
([drd.supp](../eng/sanitizers/drd.supp), [helgrind.supp](../eng/sanitizers/helgrind.supp)); see
[Race detection](eng/building.md#race-detection).

### Headers under `-pedantic`

Paho 1.3.16 headers use anonymous unions, which a strict C99 `-pedantic -Werror` build rejects.

**Mitigation:** include Paho's headers as system headers (`-isystem`), as the SDK's own targets do.

### Older tags and CMake 4

Paho before 1.3.14 declares `cmake_minimum_required` below 3.5, which CMake 4.0+ refuses. The SDK
sets `CMAKE_POLICY_VERSION_MINIMUM=3.5` when it fetches Paho, so an older `PAHO_C_TAG` still
configures.
