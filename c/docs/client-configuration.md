<!-- Copyright (c) Microsoft. All rights reserved.
     Licensed under the MIT license. See LICENSE file in the project root for full license information. -->

# Client configuration

What can be configured in the C SDK, and where: CMake options and header limits at compile time,
option structs, setters, logging and environment variables at run time.

## Compile time

### CMake options

Set with `-D<OPTION>=<value>` on the configure command, or with `set()` in a parent project before
it adds `c/`.

| Option | Default | Effect |
| --- | --- | --- |
| `AZ_IOT_WITH_PAHO` | `ON` | Build the Eclipse Paho C MQTT adapter. The samples need it. |
| `AZ_IOT_PAHO_KEY_CUSTODY` | `ON` | Let the Paho adapter use private keys held in hardware (key reference URI). Needs OpenSSL 3.0+; off without it. |
| `AZ_IOT_WITH_CERT_PROVIDER_MANAGED` | `ON` | Build the OpenSSL managed certificate provider (CSR and key handling). Built only when OpenSSL 3.0+ is found. |
| `AZ_IOT_WITH_CRYPTO_OPENSSL` | `ON` | Build the OpenSSL crypto backend, `az_iot_crypto_openssl()`. Built only when OpenSSL 3.0+ is found. |
| `AZ_IOT_WITH_CRYPTO_MBEDTLS` | `ON` | Build the mbedTLS crypto backend, `az_iot_crypto_mbedtls()`. Built only when mbedTLS 3.6 LTS or 4.1+ is found. |
| `AZ_IOT_WITH_AZ_MQTT` | `OFF` | Build the az_mqtt MQTT adapter (`az_iot_adapter_az_mqtt.h`) and the bundled `deps/az_mqtt` client. |
| `AZ_IOT_AZ_MQTT_TLS` | `openssl` | TLS backend of the az_mqtt adapter: `openssl` (3.0+, with key references) or `mbedtls`. Windows uses Schannel. |
| `AZ_IOT_AZ_MQTT_FOOTPRINT`, `AZ_IOT_AZ_MQTT_SEND_BUFFER_SIZE`, `AZ_IOT_AZ_MQTT_RECEIVE_BUFFER_SIZE`, `AZ_IOT_AZ_MQTT_INFLIGHT_MAX`, `AZ_IOT_AZ_MQTT_USER_PROPERTIES_MAX`, `AZ_IOT_AZ_MQTT_MESSAGE_STORAGE_SIZE`, `AZ_IOT_AZ_MQTT_STATIC_CLIENTS`, `AZ_IOT_AZ_MQTT_TRANSPORT_SIZE`, `AZ_IOT_AZ_MQTT_CONNECT_STRINGS_SIZE`, `AZ_IOT_AZ_MQTT_CONFIG_FILE` | empty | Per-client sizes of the az_mqtt adapter, and static clients (no heap). Empty: the macro's default: the footprint's value (`DEFAULT` or `CONSTRAINED`) for the send and receive sizes, in-flight and user-property limits; send size + 18 for the message store; 0 static clients; 200 KiB (Windows) or 8 KiB transport area; 8 KiB connect strings. Each is also a compile-time macro, so builds without CMake can set them. See [az_mqtt adapter sizes](#az_mqtt-adapter-sizes). |
| `AZ_IOT_WITH_RUST_MQTT` | `OFF` | Build the Rust MQTT adapter shell: a C adapter that forwards to a Rust MQTT client the application installs at run time. |
| `AZ_IOT_BUILD_SAMPLES` | `ON` | Build the samples. |
| `AZ_IOT_BUILD_TESTS` | `OFF` | Build the unit tests (the presets turn it on). |
| `AZ_IOT_INSTALL` | `ON` when `c/` is the top-level project | Generate install rules and the `azure-iot-sdk` CMake package. |
| `BUILD_SHARED_LIBS` | `OFF` | Build `core`, `mqttv3` and `mqttv5` as shared libraries. Adapters are always static. See [Struct versioning](struct_versioning.md). |
| `AZ_IOT_WARNINGS_AS_ERRORS` | `ON` | Treat compiler warnings as errors. |
| `AZ_IOT_ENABLE_HARDENING` | `ON` | Hardened compiler and linker flags: stack protector, FORTIFY, PIE and RELRO on GCC and Clang; `/guard:cf`, `/CETCOMPAT` and `/sdl` on MSVC. |
| `AZ_SDK_C_TAG`, `AZ_SDK_C_REPO` | `1.5.0`, GitHub | Version and source of azure-sdk-for-c, fetched at configure time. |
| `PAHO_C_TAG`, `PAHO_C_REPO` | `v1.3.13`, GitHub | Version and source of Eclipse Paho C, fetched at configure time. |

Options for the test suites, coverage and static analysis are described in [docs/eng](eng/).

### az_mqtt adapter sizes

Defaults are in [az_iot_az_mqtt_config.h](../adapters/az_mqtt/az_iot_az_mqtt_config.h). Each value
comes from, in order:

1. a definition: a compiler `-D`, the file named by `AZ_IOT_AZ_MQTT_CONFIG_FILE`, or CMake
   (which passes each option only when set);
2. the footprint, `AZ_IOT_AZ_MQTT_FOOTPRINT`;
3. the `DEFAULT` footprint.

| Macro | `DEFAULT` | `CONSTRAINED` | Effect |
| --- | --- | --- | --- |
| `AZ_IOT_AZ_MQTT_SEND_BUFFER_SIZE` | 270336 (264 KiB) | 8192 (8 KiB) | Largest outgoing packet. A larger connect or publish fails. |
| `AZ_IOT_AZ_MQTT_RECEIVE_BUFFER_SIZE` | 270336 (264 KiB) | 135168 (132 KiB) | Largest incoming packet. MQTT 5: the Maximum Packet Size advertised; the server drops larger messages. MQTT 3.1.1: a larger packet ends the session. |
| `AZ_IOT_AZ_MQTT_INFLIGHT_MAX` | 64 | 8 | QoS 1/2 exchanges in flight, 1-32767. MQTT 5: also the Receive Maximum advertised. |
| `AZ_IOT_AZ_MQTT_USER_PROPERTIES_MAX` | 16 | 4 | MQTT 5 user properties per packet. |
| `AZ_IOT_AZ_MQTT_MESSAGE_STORAGE_SIZE` | send size + 18 | send size + 18 | Store of unacknowledged QoS 1/2 PUBLISH, for sessions that outlive the connection. 0: a connect with `clean_start` false (MQTT 5: and `session_expiry_seconds` > 0) returns `AZ_IOT_ERR_NOT_SUPPORTED`. |

IoT Hub limits: device-to-cloud messages 256 KB, direct method payloads 128 KB, cloud-to-device
messages 64 KB, twin documents 32 KB. `CONSTRAINED` receives all of these, and sends packets up to
8 KiB.

Each client allocates, when created, the send and receive buffers, a buffer for the strings of a
received PUBLISH (receive size + 3 + 2 × `AZ_IOT_AZ_MQTT_USER_PROPERTIES_MAX` bytes) and the
transport. On its first connect whose session outlives the connection (`clean_start` false; MQTT 5:
also `session_expiry_seconds` > 0) it also allocates the message store, kept until the client is
destroyed. Per client: about 792 KiB with `DEFAULT` (1,056 KiB with such a session), 272 KiB with
`CONSTRAINED` (280 KiB).

#### Static clients

With `AZ_IOT_AZ_MQTT_STATIC_CLIENTS` N > 0 the adapter allocates nothing: each MQTT version has N
clients in static storage, and its factory is static.

| Macro | Default | Effect |
| --- | --- | --- |
| `AZ_IOT_AZ_MQTT_STATIC_CLIENTS` | 0 | Clients of each MQTT version. 0: allocated when created, as above. Otherwise `create()` returns NULL when all N are in use. |
| `AZ_IOT_AZ_MQTT_TRANSPORT_SIZE` | 204800 (200 KiB) on Windows, 8192 elsewhere | Bytes reserved for the transport. `create()` returns NULL, with an error log, when the transport needs more: under 1 KiB with OpenSSL, about 4-5 KiB with mbedTLS (depends on its version and configuration), about 197 KiB with Schannel. |
| `AZ_IOT_AZ_MQTT_CONNECT_STRINGS_SIZE` | 8192 | Bytes for the copies of a connect's strings, each with a NUL: host, client ID, credentials, TLS paths or PEM, proxy, will, user properties, WebSocket path. A connect whose copies do not fit returns `AZ_IOT_ERR_NOT_ENOUGH_SPACE`. |

- Each client takes, in `.bss`, for each MQTT version linked: send size + receive size + strings
  buffer + `AZ_IOT_AZ_MQTT_TRANSPORT_SIZE` + `AZ_IOT_AZ_MQTT_MESSAGE_STORAGE_SIZE` +
  `AZ_IOT_AZ_MQTT_CONNECT_STRINGS_SIZE`, plus the client's state and padding: about 1.5-2 KiB
  (MQTT 3.1.1) or 2-4 KiB (MQTT 5) on 64-bit Linux, growing with `AZ_IOT_AZ_MQTT_INFLIGHT_MAX` and
  `AZ_IOT_AZ_MQTT_USER_PROPERTIES_MAX`. For example `CONSTRAINED` on Linux, with an 8 KiB transport
  area and `AZ_IOT_AZ_MQTT_MESSAGE_STORAGE_SIZE=0`: 297,072 bytes (290 KiB) per MQTT 5 client.
- `create()` and `destroy()` must not run concurrently (the connection client calls them from its
  own calls).
- Every `az_iot_az_mqtt_factory_create_*()` call returns the same factory; destroying it does
  nothing.
- The copied connect strings are wiped when released (each connect, and `destroy()`), and
  `destroy()` wipes the whole client.
- The TLS library still allocates: OpenSSL and Schannel always; mbedTLS unless built with
  `MBEDTLS_MEMORY_BUFFER_ALLOC_C` and given a static pool (`mbedtls_memory_buffer_alloc_init()`).

The values apply where the adapter sources are compiled; defining them only for the application
has no effect. Without CMake, compile the adapter with them:

```sh
cc -DAZ_IOT_AZ_MQTT_FOOTPRINT=AZ_IOT_AZ_MQTT_FOOTPRINT_CONSTRAINED -DAZ_IOT_AZ_MQTT_RECEIVE_BUFFER_SIZE=36864 ...
cc -DAZ_IOT_AZ_MQTT_CONFIG_FILE='"my_az_mqtt_config.h"' ...   # a header on the include path
```

With CMake: `-DAZ_IOT_AZ_MQTT_FOOTPRINT=CONSTRAINED`, any option above, or
`-DAZ_IOT_AZ_MQTT_CONFIG_FILE=<header>` (an absolute path, or a name on the include path).

### Limits in the public headers

Buffer sizes, pool sizes and default timeouts are `#define`s that you can override at compile
time, for example `-DAZ_IOT_MAX_PERSISTENT_SUBS=12`. The sizes are fixed when compiled: they size
arrays inside caller-allocated structs, or buffers inside the SDK.

**Set every override for the whole build, with the same value for the SDK and the application.**

- Most of these size members of public structs. The application and the SDK each compile those
  structs from the headers, so different values give the two different layouts.
- The others are only read by SDK source files. Setting them for the application alone has no
  effect.
- Ways to apply one value to both:
  - The SDK built inside your project (`add_subdirectory()` or `FetchContent`): call
    `add_compile_definitions(AZ_IOT_MAX_PERSISTENT_SUBS=12)` before adding `c/`. Directory-level
    definitions apply to the SDK's targets and to yours.
  - An installed SDK: build the SDK with the definition (for example in `CMAKE_C_FLAGS`), and
    compile the application with the same one. The installed CMake package does not carry it.
- After changing a value, rebuild the SDK and the application. See
  [Struct versioning](struct_versioning.md).

When a limit is exceeded:

- by an application call: the call fails with an `az_iot_result` (usually
  `AZ_IOT_ERR_NOT_ENOUGH_SPACE` or `AZ_IOT_ERR_NOT_SUPPORTED`);
- by data from the service: depends on the limit. Usually the message, or the part that does not
  fit, is dropped with a `WARN` log; for some limits the operation it belongs to fails
  (provisioning, a software update).

The tables state each outcome that is not a failed call or a dropped message.

#### Connection client ([az_iot_connection_client.h](../inc/azure/iot/az_iot_connection_client.h))

| Macro | Default | Bounds |
| --- | --- | --- |
| `AZ_IOT_MAX_MQTT_FACTORIES` | 4 | MQTT adapter factories registered with one connection client. |
| `AZ_IOT_MAX_PENDING_PUBACKS` | 16 | QoS 1 publishes awaiting an acknowledgement with a completion callback, across all feature clients. When the caller's pool is full, nothing is sent and the call returns `AZ_IOT_ERR_BUSY`; retry once an acknowledgement arrives. See [In-flight QoS 1 publishes](#in-flight-qos-1-publishes). |
| `AZ_IOT_MAX_PUBACK_RESERVATIONS` | 2 | Feature clients holding an `AZ_IOT_MAX_PENDING_PUBACKS` reservation; an mqttv5 direct method client takes one; certificate renewal takes one when `opts.csr_payload_buffer` is set. At most 254. |
| `AZ_IOT_MAX_PERSISTENT_SUBS` | 8 | Topic filters re-subscribed on every session. A fully loaded mqttv3 device uses 5; mqttv5 feature clients use none. |
| `AZ_IOT_PERSISTENT_SUB_TOPIC_MAX` | 128 | Length of one such topic filter. |
| `AZ_IOT_MAX_SESSION_HANDLERS` | 4 | Feature clients told when a session ends. |
| `AZ_IOT_MAX_FEATURE_STATE_OBSERVERS` | 6 | Connection-state observers used by feature clients. |
| `AZ_IOT_MAX_APP_STATE_OBSERVERS` | 4 | Connection-state observers the application can register. |
| `AZ_IOT_MAX_FEATURE_CLIENT_BINDS` | 8 | Feature clients attached to one connection client. |
| `AZ_IOT_DPS_HOST_BUF` | 128 | Assigned IoT Hub host name, terminator included. A longer one fails the provisioning attempt with `AZ_IOT_ERR_NOT_SUPPORTED`. |
| `AZ_IOT_DPS_DEVICE_ID_BUF` | 128 | Assigned device ID, terminator included. A longer one fails the provisioning attempt with `AZ_IOT_ERR_NOT_SUPPORTED`. |
| `AZ_IOT_DPS_OPERATION_ID_MAX` | 64 | DPS operation ID. A longer one fails the provisioning attempt with `AZ_IOT_ERR_NOT_SUPPORTED`. |
| `AZ_IOT_DPS_TOPIC_BUF` | 256 | DPS publish topic. |
| `AZ_IOT_DPS_REGISTRATION_PAYLOAD_MAX` | 512 | Custom registration payload that `AZ_IOT_DPS_REGISTRATION_BODY_STORAGE()` leaves room for. Application-side only; you can also size the buffer yourself. |
| `AZ_IOT_CONNECTION_PROFILE_RAW_BUF` | 64 | Connection profile string returned by DPS. A longer value is reported truncated and fails the connection with `AZ_IOT_ERR_CONNECTION_PROFILE_UNSUPPORTED`. |
| `AZ_IOT_MQTT_USERNAME_BUF` | 256 | MQTT user name. |
| `AZ_IOT_PRESENCE_TOPIC_BUF` | 256 | mqttv5 presence topics. |

Defaults used when the matching option is left at 0:

| Macro | Default | Used for |
| --- | --- | --- |
| `AZ_IOT_DEFAULT_KEEP_ALIVE_SECONDS` | 30 | `keep_alive_seconds` |
| `AZ_IOT_DEFAULT_CONNECT_TIMEOUT_SECONDS` | 30 | `connect_timeout_seconds` |
| `AZ_IOT_DEFAULT_SUBSCRIPTION_ACK_TIMEOUT_SECONDS` | 60 | `subscription_ack_timeout_seconds` |
| `AZ_IOT_DEFAULT_SESSION_EXPIRY_SECONDS` | 3600 | `session_expiry_seconds` (mqttv5) |
| `AZ_IOT_DPS_HOLD_TIMEOUT_MS` | 60000 | `dps_hold_timeout_ms` |
| `AZ_IOT_DEFAULT_MAX_HUB_CONNECT_ATTEMPTS_BEFORE_REPROVISION` | 50 | `dps.max_hub_connect_attempts_before_reprovision`, set by `az_iot_connection_client_options_default()` |
| `AZ_IOT_DEFAULT_IDENTITY_RECOVERY_INITIAL_DELAY_MS` | 300000 | `identity_recovery.policy.initial_delay_ms`, set by `az_iot_connection_client_options_default()` |
| `AZ_IOT_DEFAULT_IDENTITY_RECOVERY_MAX_DELAY_MS` | 3600000 | `identity_recovery.policy.max_delay_ms`, set by `az_iot_connection_client_options_default()` |
| `AZ_IOT_DEFAULT_IDENTITY_RECOVERY_JITTER_PCT` | 25 | `identity_recovery.policy.jitter_pct`, set by `az_iot_connection_client_options_default()` |
| `AZ_IOT_PRESENCE_BIRTH_ACK_TIMEOUT_MS` | 60000 | mqttv5 presence handshake timeout (no option) |

#### Feature clients ([az_iot_message.h](../inc/azure/iot/az_iot_message.h) and the client headers)

| Macro | Default | Bounds |
| --- | --- | --- |
| `AZ_IOT_TWIN_MAX_PENDING` | 8 | Twin requests in flight per twin client. |
| `AZ_IOT_DM_MAX_INFLIGHT` | 4 | Direct-method invocations held per client (mqttv3; default for mqttv5). When full, a new mqttv3 invocation is dropped with a `WARN` log and gets no response. |
| `AZ_IOT_DM_METHOD_NAME_MAX` | 96 | Direct-method name. |
| `AZ_IOT_DM_RID_MAX` | 32 | mqttv3 request ID. |
| `AZ_IOT_DM_CORR_DATA_MAX` | 64 | mqttv5 correlation data. |
| `AZ_IOT_MQTTV3_DM_RESPONSE_TIMEOUT_SECONDS` | 300 | Default for `az_iot_mqttv3_direct_method_client_set_response_timeout()`. |
| `AZ_IOT_MQTTV5_DM_MAX_CONCURRENT` | `AZ_IOT_DM_MAX_INFLIGHT` | mqttv5 invocations probed or awaiting an answer. A probe past it is refused `DEVICE_BUSY`. The client reserves twice this many `AZ_IOT_MAX_PENDING_PUBACKS` slots. |
| `AZ_IOT_MQTTV5_DM_MAX_METHODS` | 8 | Method names one mqttv5 client can declare. |
| `AZ_IOT_MQTTV5_DM_RESULT_BODY_MAX` | 512 | mqttv5 direct-method result body. |
| `AZ_IOT_MQTTV5_DM_TOPIC_MAX` | 192 | mqttv5 direct-method topic. |
| `AZ_IOT_MQTTV5_TELEMETRY_MAX_USER_PROPERTIES` | 16 | User properties on one mqttv5 telemetry message, 2 of them added by the client. Extra properties are dropped with a warning. |
| `AZ_IOT_C2D_MAX_PROPERTIES` | 8 | Properties surfaced on one cloud-to-device message. Extra properties are dropped with a warning; the message is still delivered. |
| `AZ_IOT_C2D_PROPERTY_BUFFER` | 256 | Decoded property names and values of one cloud-to-device message. When they do not fit, the message is delivered with no properties and a `WARN` log. |
| `AZ_IOT_FILE_UPLOAD_BODY_MAX` | 512 | File-upload request body; bounds the blob name (about 495 bytes once JSON-escaped). |
| `AZ_IOT_FILE_UPLOAD_URL_MAX` | 512 | File-upload request URL. |
| `AZ_IOT_FILE_UPLOAD_SAS_URI_MAX` | 2048 | Blob SAS URI. |
| `AZ_IOT_FILE_UPLOAD_CORR_ID_MAX` | 192 | File-upload correlation ID. |

#### Software updates ([az_iot_su.h](../inc/azure/iot/az_iot_su.h))

| Macro | Default | Bounds |
| --- | --- | --- |
| `AZ_IOT_SU_MAX_ROOT_KEYS` | 4 | Root keys in the trust store. |
| `AZ_IOT_SU_REQUEST_BUFFER_SIZE` | 16384 | Copy of the update metadata (manifest, signature and file URLs) for the current deployment; a one-file offer is about 5 KiB. A larger deployment is refused: `AZ_IOT_SU_EVENT_UPDATE_REFUSED` is raised with `AZ_IOT_ERR_NOT_ENOUGH_SPACE`. Also sizes `AZ_IOT_SU_STATE_BLOB_MAX_SIZE` and the scratch the client verifies the signature in, so the client holds about twice this value. |
| `AZ_IOT_SU_VERIFY_SCRATCH_SIZE` | 8192 | Stack used by `az_iot_su_parse_update_request()` for the decoded signature (JWS header with the signing key, and the signatures). A signature that does not fit fails with `AZ_IOT_ERR_AUTH` and an `ERROR` log naming the part that is too large. |
| `AZ_IOT_SU_WORKFLOW_ID_SIZE` | 64 | Workflow ID kept for reporting, duplicate detection and persistence. A deployment with a longer ID is refused: nothing is processed or reported, and `AZ_IOT_SU_EVENT_UPDATE_REFUSED` is raised with `AZ_IOT_ERR_NOT_ENOUGH_SPACE`. |
| `AZ_IOT_SU_PERSIST_MAX_ATTEMPTS` | 5 | Consecutive failed state writes before the client stops retrying (1 to `0xFFFFFFFF`). |
| `AZ_IOT_SU_DEVICE_PROPERTIES_BUFFER_SIZE` | 512 | Default size of the device-properties buffer type. Application-side only; `az_iot_su_device_properties_buffer_size()` gives the exact size. |
| `AZ_IOT_MAX_SU_OBSERVERS` | 4 | Observers the application can register. |

#### Dispatch and logging

| Macro | Default | Bounds |
| --- | --- | --- |
| `AZ_IOT_MAX_INBOUND_HANDLERS` | 8 | Inbound topic handlers in one dispatch table. |
| `AZ_IOT_DISPATCH_PREFIX_MAX` | 128 | Topic prefix of one handler. |
| `AZ_IOT_LOG_MESSAGE_MAX` | 384 | One formatted log message, terminator included. Longer messages are truncated and end in `...`. |
| `AZ_IOT_LOG_FILE_PATH_MAX` | 256 | Log file path for the file sink, terminator included. |
| `AZ_IOT_LOG_FILE_DEFAULT_MAX_BYTES` | 1 MiB | Default size at which the file sink rotates. |
| `AZ_IOT_LOG_FILE_DEFAULT_MAX_FILES` | 3 | Default number of rotated log files kept. |

#### In-flight QoS 1 publishes

A QoS 1 publish sent with a completion callback holds one `AZ_IOT_MAX_PENDING_PUBACKS` slot until
its acknowledgement arrives or the session ends. A feature client may reserve slots for its own
publishes at init; the rest form a shared pool. Neither side uses the other's slots. A reservation
is granted only when usable at once: if tracked publishes in flight occupy its slots, it fails with
`AZ_IOT_ERR_BUSY`; retry once they are acknowledged.

| Feature client | QoS 1 publishes | Own bound | Slots |
| --- | --- | --- | --- |
| mqttv3 and mqttv5 telemetry `send` | 1 per call | Only the shared pool | Shared pool; the callback is required. |
| mqttv5 direct methods | Probe ack, result, abandon | `AZ_IOT_MQTTV5_DM_MAX_CONCURRENT` invocations. Not bounded: refused probes are also acknowledged, and an invocation ends at `respond()`, before its result is acknowledged. | Reserves `2 × AZ_IOT_MQTTV5_DM_MAX_CONCURRENT` at init; init fails with `AZ_IOT_ERR_NOT_ENOUGH_SPACE` if they do not fit, or `AZ_IOT_ERR_BUSY` as above. When all are in use, sends without one. |
| Certificate renewal | 1 request | 1 operation at a time | Reserves 1 at init when `opts.csr_payload_buffer` is set. Cancel and timeout give it back at once. |

Not counted: twin and mqttv3 direct methods (QoS 0, bounded by `AZ_IOT_TWIN_MAX_PENDING` and
`AZ_IOT_DM_MAX_INFLIGHT`), and the provisioning session (DPS registration, software updates).

With the defaults, telemetry gets all 16 slots, 8 when an mqttv5 direct method client is
attached, and one fewer when `opts.csr_payload_buffer` is set. To size it, add the telemetry sends you keep in flight to the reservations. A slot is
16 bytes on 32-bit targets and 32 bytes on 64-bit targets; a reservation entry is 8 or 16 bytes.

## Run time

### Option structs

Most structs configure a client once, at `init()`; `az_iot_mqttv5_twin_get_options` is passed per
request. Start from the `_default()` function where one exists: a zero-initialized struct is not
always the same thing.

| Struct | Configures | Reference |
| --- | --- | --- |
| `az_iot_connection_client_options` (`az_iot_connection_client_options_default()`) | DPS or direct hub connection, certificates, crypto backend, reconnection and identity recovery policies, timeouts, WebSockets, HTTP proxy, MQTT session terms, Last Will, mqttv5 twin push | [Connecting a device](connecting.md), [az_iot_connection_client.h](../inc/azure/iot/az_iot_connection_client.h) |
| `az_iot_retry_policy` (`az_iot_connection_client_get_default_retry_policy()`, `_get_disabled_retry_policy()`, `_get_fixed_interval_retry_policy()`) | Backoff and retry limit | [Connecting a device](connecting.md#reconnection), [az_iot_retry_policy.h](../inc/azure/iot/az_iot_retry_policy.h) |
| `az_iot_certificate_provider_pem_options` (`az_iot_certificate_provider_pem_options_default()`) | Certificate, key and CA files | [az_iot_certificate_provider_pem.h](../inc/azure/iot/az_iot_certificate_provider_pem.h) |
| `az_iot_certificate_provider_managed_options` | Bootstrap and operational certificate and key files, key type (EC P-256 or RSA 2048) | [az_iot_certificate_provider_managed.h](../adapters/cert_openssl/az_iot_certificate_provider_managed.h), [sample](../samples/authentication/dps_csr_managed/README.md) |
| `az_iot_su_client_config_options` (`az_iot_su_client_config_options_default()`) | Platform hooks, root keys, device properties. Crypto comes from the connection client. | [az_iot_su.h](../inc/azure/iot/az_iot_su.h), [samples](../samples/software_update/pc/simulated_onboarding/README.md) |
| `az_iot_mqttv5_twin_get_options` (`az_iot_mqttv5_twin_get_options_default()`) | Per request to `az_iot_mqttv5_twin_client_get_with_options()`: sections to fetch, and versions to skip if unchanged | [az_iot_twin_client.h](../inc/azure/iot/mqttv5/az_iot_twin_client.h) |

### Setters on feature clients

| Function | Configures | Default |
| --- | --- | --- |
| `az_iot_mqttv3_direct_method_client_set_response_timeout()` | How long an unanswered mqttv3 invocation is held | 300 s |
| `az_iot_mqttv5_direct_method_client_register_method()` | A method the device answers, and the time it needs | none declared |
| `az_iot_mqttv5_direct_method_client_set_probe_handler()` | Whether to accept an invocation before its arguments arrive | accept when timing and capacity allow |
| `az_iot_mqttv5_twin_client_set_request_timeout()` | How long a twin request may wait for its answer | 60 s |
| `az_iot_mqttv5_twin_client_set_encode_buffer()` | Buffer reported patches are encoded into | required before patching |
| `az_iot_mqttv3_file_upload_client_init()` (`http_transport`) | The HTTP client used for the IoT Hub file-upload calls | required |

### Logging

The SDK logs through one process-wide sink. It logs nothing until you install one.

```c
az_iot_log_sink sink = az_iot_log_stderr_sink(AZ_IOT_LOG_LEVEL_INFO); /* or your own callback */
az_iot_log_set_global_sink(&sink);
```

Levels: `TRACE`, `DEBUG`, `INFO`, `WARN`, `ERROR`, `OFF`. A custom sink is an
`az_iot_log_sink` with your callback, a context pointer and a minimum level. A rotating
file sink is built in (`az_iot_log_file_sink_open()`,
[az_iot_log_file.h](../inc/azure/iot/az_iot_log_file.h)). See [Logging](logging.md) for the
line format, component prefixes and what to collect for support, and
[az_iot_log.h](../inc/azure/iot/az_iot_log.h).

### Environment variables

Supported environment variables. The samples read their own; each sample's README lists them.

| Variable | Read by | Effect |
| --- | --- | --- |
| `AZ_IOT_PAHO_TRACE` | Paho adapter | Turns on Paho's trace and detailed OpenSSL errors on a failed TLS handshake. Values, least to most verbose: `error`, `protocol`, `minimum` (or `min`), `medium`, `maximum` (or `max`); any other non-empty value means `minimum`. The trace is logged at `TRACE`, so set the log sink to `AZ_IOT_LOG_LEVEL_TRACE` to see it. Proxy credentials are redacted. |
| `TMPDIR` | Paho adapter, key custody (not Windows) | Directory for the short-lived key-reference file. Default `/tmp`. |
| `http_proxy`, `https_proxy` | Eclipse Paho C | HTTP proxy used when the connection options set none. Lowercase only. Set `proxy` in the connection options to avoid this; see [Connecting a device](connecting.md#network-websockets-and-proxies). |
