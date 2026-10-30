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
| `AZ_IOT_AZ_MQTT_FOOTPRINT`, `AZ_IOT_AZ_MQTT_SEND_BUFFER_SIZE`, `AZ_IOT_AZ_MQTT_RECEIVE_BUFFER_SIZE`, `AZ_IOT_AZ_MQTT_INFLIGHT_MAX`, `AZ_IOT_AZ_MQTT_USER_PROPERTIES_MAX`, `AZ_IOT_AZ_MQTT_CONFIG_FILE` | empty | Per-client sizes of the az_mqtt adapter. Empty: the value of the footprint (`DEFAULT` or `CONSTRAINED`). Each is also a compile-time macro, so builds without CMake can set them. See [az_mqtt adapter sizes](#az_mqtt-adapter-sizes). |
| `AZ_IOT_BUILD_SAMPLES` | `ON` | Build the samples. |
| `AZ_IOT_BUILD_TESTS` | `OFF` | Build the unit tests (the presets turn it on). |
| `AZ_IOT_INSTALL` | `ON` when `c/` is the top-level project | Generate install rules and the `azure-iot-sdk` CMake package. |
| `BUILD_SHARED_LIBS` | `OFF` | Build `core`, `mqttv3` and `mqttv5` as shared libraries. Adapters are always static. See [Struct versioning](struct_versioning.md). |
| `AZ_IOT_WARNINGS_AS_ERRORS` | `ON` | Treat compiler warnings as errors. |
| `AZ_IOT_ENABLE_HARDENING` | `ON` | Hardened compiler and linker flags: stack protector, FORTIFY, PIE and RELRO on GCC and Clang; `/guard:cf`, `/CETCOMPAT` and `/sdl` on MSVC. |
| `AZ_SDK_C_TAG`, `AZ_SDK_C_REPO` | `1.5.0`, GitHub | Version and source of azure-sdk-for-c, fetched at configure time. |
| `PAHO_C_TAG`, `PAHO_C_REPO` | `v1.3.16`, GitHub | Version and source of Eclipse Paho C, fetched at configure time. |
| `AZ_IOT_USE_INSTALLED_AZURE_SDK_FOR_C` | `OFF` | Use an installed azure-sdk-for-c 1.5.0+ instead of fetching it. See [Installed dependencies](#installed-dependencies). |
| `AZ_IOT_USE_INSTALLED_PAHO` | `OFF` | Use an installed Eclipse Paho C 1.3.14+ (`eclipse-paho-mqtt-c` CMake package) instead of fetching it. See [Installed dependencies](#installed-dependencies). |
| `AZ_IOT_SU_MAX_STEP_COUNT`, `AZ_IOT_SU_MAX_FILE_COUNT`, `AZ_IOT_SU_MAX_FILE_COUNT_PER_STEP` | 10, 10, empty (the file count) | Software updates manifest limits; the defaults accept every manifest Device Update accepts. See [Software updates manifest limits](#software-updates-manifest-limits). |

Options for the test suites, coverage and static analysis are described in [docs/eng](eng/).

### Installed dependencies

azure-sdk-for-c and Eclipse Paho C are fetched and built from source by default. Versions and known
issues: [Dependencies](dependencies.md). To use installed
copies instead (e.g. from vcpkg or a package feed), turn on `AZ_IOT_USE_INSTALLED_AZURE_SDK_FOR_C`
and/or `AZ_IOT_USE_INSTALLED_PAHO` and put their prefixes on `CMAKE_PREFIX_PATH`:

- azure-sdk-for-c ships no CMake package. [Findazure-sdk-for-c.cmake](../cmake/Findazure-sdk-for-c.cmake)
  looks for `azure/core/az_version.h` under `include` or `include/azure-sdk-for-c`, and for the
  `az_core`, `az_iot_common`, `az_iot_hub`, `az_iot_provisioning` and platform (`az_posix`,
  `az_win32` or `az_noplatform`) libraries under `lib`. Debug builds link libraries of the same
  names from `debug/lib` (vcpkg layout) when present. Consumers of the installed SDK need exactly the
  azure-sdk-for-c version it was built with.
  It must also be built with the [software updates manifest limits](#software-updates-manifest-limits)
  of this build; configuration fails otherwise (checked by running a small program, so not when
  cross-compiling).
  [eng/install-deps.sh](../eng/install-deps.sh) (POSIX) builds such a prefix, and Paho's, from the pinned
  commits.
- Paho is found with `find_package(eclipse-paho-mqtt-c CONFIG)`; its static, SSL-capable target
  is preferred. With vcpkg, add the `installed-paho` feature:
  `-DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake -DVCPKG_MANIFEST_FEATURES=installed-paho -DAZ_IOT_USE_INSTALLED_PAHO=ON`.
  On Windows, use a static triplet (e.g. `x64-windows-static`) to match the SDK's static C runtime;
  with Ninja, also set `-DVCPKG_APPLOCAL_DEPS=OFF` (no DLLs to copy; parallel copy steps can collide).

`c/vcpkg.json` pins its port versions with `builtin-baseline`. An installed dependency is not
copied into the `azure-iot-sdk` package: its config file finds it again, so consumers need the
dependency's prefix on `CMAKE_PREFIX_PATH` too. With MSVC, installed libraries must use the
static C runtime (`/MT`, `/MTd` for Debug), as the SDK does.

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
| `AZ_IOT_AZ_MQTT_USER_PROPERTIES_MAX` | 16 | 4 | MQTT 5 user properties on CONNECT; more is refused. PUBLISH user properties are not limited (sent and received in place). |
| `AZ_IOT_AZ_MQTT_MESSAGE_STORAGE_SIZE` | send size + 18 | send size + 18 | Store of unacknowledged QoS 1/2 PUBLISH, for sessions that outlive the connection. 0: no store; a connect with `clean_start` false (MQTT 5: and `session_expiry_seconds` > 0) returns `AZ_IOT_ERR_NOT_SUPPORTED`. Macro only. |

IoT Hub limits: device-to-cloud messages 256 KB, direct method payloads 128 KB, cloud-to-device
messages 64 KB, twin documents 32 KB. `CONSTRAINED` receives all of these, and sends packets up to
8 KiB.

Each client allocates, when created, the send and receive buffers, a buffer for the strings of a
received PUBLISH (receive size + 3 bytes; user properties are read in place) and the
transport. On its first connect whose session outlives the connection (`clean_start` false; MQTT 5:
also `session_expiry_seconds` > 0) it also allocates the message store, kept until the client is
destroyed. Per client: about 792 KiB with `DEFAULT` (1,056 KiB with such a session), 272 KiB with
`CONSTRAINED` (280 KiB).

The values apply where the adapter sources are compiled; defining them only for the application
has no effect. Without CMake, compile the adapter with them:

```sh
cc -DAZ_IOT_AZ_MQTT_FOOTPRINT=AZ_IOT_AZ_MQTT_FOOTPRINT_CONSTRAINED -DAZ_IOT_AZ_MQTT_RECEIVE_BUFFER_SIZE=36864 ...
cc -DAZ_IOT_AZ_MQTT_CONFIG_FILE='"my_az_mqtt_config.h"' ...   # a header on the include path
```

With CMake: `-DAZ_IOT_AZ_MQTT_FOOTPRINT=CONSTRAINED`, any option above, or
`-DAZ_IOT_AZ_MQTT_CONFIG_FILE=<header>` (an absolute path, or a name on the include path).

#### Clients in application memory

`az_iot_az_mqtt_get_adapter()` builds clients in a workspace the application provides (static,
stack or heap), with no allocation by the adapter. The sizes above become run-time values in
`az_iot_mqtt_limits`:

| Field | Meaning |
| --- | --- |
| `send_buffer_size` | Largest outgoing packet. |
| `receive_buffer_size` | Largest incoming packet; MQTT 5: the Maximum Packet Size advertised. |
| `max_in_flight_messages` | QoS 1/2 exchanges at once, 1-32767; MQTT 5: the Receive Maximum advertised. |
| `message_store_size` | Copies for resend on a resumed session; 0: clean sessions only. |
| `connect_strings_size` | Copies a connect makes of its strings (host, credentials, TLS paths or PEM, proxy, will, user properties), one NUL each. |

- `az_iot_az_mqtt_get_workspace_size(&limits)`: the exact size, for the largest of the MQTT 3.1.1
  and MQTT 5 clients, with this build's TLS backend.
- `AZ_IOT_AZ_MQTT_WORKSPACE_SIZE(send, receive, in_flight, store, connect_strings, max_tls_state)`:
  a constant-expression bound for static arrays. `max_tls_state` must be at least the TLS backend's
  state (measured: OpenSSL 560 B, mbedTLS 4.2-4.6 KB, Schannel about 197 KB).
- `init()` returns `AZ_IOT_ERR_NOT_ENOUGH_SPACE` if the workspace is too small and logs the size
  needed. A connect whose string copies do not fit `connect_strings_size` returns the same.
- The client's `destroy()` wipes the whole workspace (credentials included) and frees nothing.
- `AZ_IOT_AZ_MQTT_USER_PROPERTIES_MAX` stays a compile-time value.

The Paho adapter has the same descriptor (`az_iot_paho_get_adapter()`; bound
`AZ_IOT_PAHO_WORKSPACE_SIZE`). Its workspace holds only the adapter's state: Paho allocates its
own internals and sizes its own buffers, so the limits are only checked (the ranges az_mqtt accepts).

### Limits in the public headers

Buffer sizes, pool sizes and default timeouts are `#define`s that you can override at compile
time, for example `-DAZ_IOT_DEFAULT_KEEP_ALIVE_SECONDS=120`. Connection client entry counts are
run-time limits instead (below). The sizes are fixed when compiled: they size
arrays inside caller-allocated structs, or buffers inside the SDK.

**Set every override for the whole build, with the same value for the SDK and the application.**

- Most of these size members of public structs. The application and the SDK each compile those
  structs from the headers, so different values give the two different layouts.
- The others are only read by SDK source files. Setting them for the application alone has no
  effect.
- Ways to apply one value to both:
  - The SDK built inside your project (`add_subdirectory()` or `FetchContent`): call
    `add_compile_definitions(AZ_IOT_DEFAULT_KEEP_ALIVE_SECONDS=120)` before adding `c/`. Directory-level
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

Entry counts are run-time values in `az_iot_connection_client_options::limits`
(`az_iot_connection_client_limits`); 0 selects the default. The entries live in
`options.workspace` (static, stack or heap), owned by the client until `deinit()`, which wipes it:

- `az_iot_connection_client_get_workspace_size(&limits)`: bytes needed, any alignment.
- `AZ_IOT_CONNECTION_CLIENT_WORKSPACE_SIZE(...)`: the same as a constant expression, for static
  arrays. `AZ_IOT_CONNECTION_CLIENT_DEFAULT_WORKSPACE_SIZE`: the same for the defaults.
- With `options.workspace` NULL, the client uses its own area, sized for the built-in defaults
  below; a limit above one of them then fails `init()` with `AZ_IOT_ERR_INVALID_ARG`. A workspace
  that is too small fails it with `AZ_IOT_ERR_NOT_ENOUGH_SPACE` and logs the size needed.
- Defining one of the former macros (`AZ_IOT_MAX_PENDING_PUBACKS`, `AZ_IOT_DPS_HOST_BUF`, ...)
  before including the header is a compile error.

The defaults are compile-time values
([az_iot_connection_client_config.h](../inc/azure/iot/az_iot_connection_client_config.h)):
`AZ_IOT_CONNECTION_CLIENT_DEFAULT_MAX_<LIMIT>`, for example
`AZ_IOT_CONNECTION_CLIENT_DEFAULT_MAX_PENDING_PUBLISHES`. Set them as definitions, in a header
named by `AZ_IOT_CONNECTION_CLIENT_CONFIG_FILE` (as written in a quoted `#include`), or with the
CMake options of the same names, which also apply them to targets linking `az_iot_core`. An
installed SDK exports the numeric options but not the config file: compile the application with
the same header. The values do not change the layout of `az_iot_connection_client`: if the SDK
and the application use different values, `init()` refuses a static workspace too small for the
SDK's.

```c
static uint8_t workspace[AZ_IOT_CONNECTION_CLIENT_DEFAULT_WORKSPACE_SIZE];
opts.workspace = workspace;
opts.workspace_size = sizeof(workspace);
```

| Limit | Default | Bounds |
| --- | --- | --- |
| `max_pending_publishes` | 16 | QoS 1 publishes awaiting an acknowledgement with a completion callback, across all feature clients. When the caller's pool is full, nothing is sent and the call returns `AZ_IOT_ERR_BUSY`; retry once an acknowledgement arrives. See [In-flight QoS 1 publishes](#in-flight-qos-1-publishes). At most 65535. |
| `max_publish_reservations` | 2 | Feature clients holding a reservation of `max_pending_publishes`; an mqttv5 direct method client takes one; certificate renewal takes one when `opts.csr_payload_buffer` is set. At most 254. |
| `max_persistent_subscriptions` | 8 | Topic filters re-subscribed on every session. A fully loaded mqttv3 device uses 5; mqttv5 feature clients use none. At most 255. |
| `max_feature_observers` | 10 | Feature-client observers of connection state, session end, publish completion and the `do_work()` tick. A loaded device uses one per feature client. |
| `max_application_state_observers` | 4 | Connection-state observers the application can register. |
| `max_feature_clients` | 8 | Feature clients attached to one connection client. |
| `max_inbound_handlers` | 8 | Inbound topic handlers. |

Fixed sizes:

| Constant | Value | Bounds |
| --- | --- | --- |
| `AZ_IOT_MAX_MQTT_FACTORIES` | 4 | MQTT adapter factories registered with one connection client. |
| `AZ_IOT_CONNECTION_CLIENT_PUBLISH_COMPLETION_OBSERVERS` | 2 | Feature clients told after each hub PUBACK, to retry publishes held for adapter capacity. The mqttv5 direct method client uses one. |
| `AZ_IOT_PERSISTENT_SUB_TOPIC_MAX` | 128 | Length of one persistent topic filter. |
| `AZ_IOT_DPS_HOST_BUF` | 128 | Assigned IoT Hub host name, terminator included. A longer one fails the provisioning attempt with `AZ_IOT_ERR_NOT_SUPPORTED`. |
| `AZ_IOT_DPS_DEVICE_ID_BUF` | 128 | Assigned device ID, terminator included. A longer one fails the provisioning attempt with `AZ_IOT_ERR_NOT_SUPPORTED`. |
| `AZ_IOT_DPS_OPERATION_ID_MAX` | 64 | DPS operation ID. A longer one fails the provisioning attempt with `AZ_IOT_ERR_NOT_SUPPORTED`. |
| `AZ_IOT_CONNECTION_PROFILE_RAW_BUF` | 64 | Connection profile string returned by DPS. A longer value is reported truncated and fails the connection with `AZ_IOT_ERR_CONNECTION_PROFILE_UNSUPPORTED`. |
| `AZ_IOT_MQTT_USERNAME_BUF` | 256 | MQTT user name. |

Compile-time macros, only read by SDK source files:

| Macro | Default | Bounds |
| --- | --- | --- |
| `AZ_IOT_DPS_TOPIC_BUF` | 256 | DPS publish topic. |
| `AZ_IOT_DPS_REGISTRATION_PAYLOAD_MAX` | 512 | Custom registration payload that `AZ_IOT_DPS_REGISTRATION_BODY_STORAGE()` leaves room for. Application-side only; you can also size the buffer yourself. |
| `AZ_IOT_PRESENCE_TOPIC_BUF` | 256 | mqttv5 presence topics. |

Defaults used when the matching option is left at 0:

| Macro | Default | Used for |
| --- | --- | --- |
| `AZ_IOT_DEFAULT_KEEP_ALIVE_SECONDS` | 30 | `keep_alive_seconds` |
| `AZ_IOT_DEFAULT_CONNECT_TIMEOUT_SECONDS` | 30 | `connect_timeout_seconds` |
| `AZ_IOT_DEFAULT_SUBSCRIPTION_ACK_TIMEOUT_SECONDS` | 60 | `subscription_ack_timeout_seconds` (also the mqttv5 presence SUBACK) |
| `AZ_IOT_DEFAULT_SESSION_EXPIRY_SECONDS` | 3600 | `session_expiry_seconds` (mqttv5) |
| `AZ_IOT_DEFAULT_MAX_UNCONFIRMED_DELIVERY_TIMEOUT_MS` | 300000 | `max_unconfirmed_delivery_timeout_ms`: how long an SDK message with no Message Expiry is kept for confirmation or resend (mqttv5 direct method results and notices); cannot be disabled |
| `AZ_IOT_DPS_HOLD_TIMEOUT_MS` | 60000 | `dps_hold_timeout_ms` |
| `AZ_IOT_DEFAULT_MAX_HUB_CONNECT_ATTEMPTS_BEFORE_REPROVISION` | 50 | `dps.max_hub_connect_attempts_before_reprovision`, set by `az_iot_connection_client_options_default()` |
| `AZ_IOT_DEFAULT_IDENTITY_RECOVERY_INITIAL_DELAY_MS` | 300000 | `identity_recovery.policy.initial_delay_ms`, set by `az_iot_connection_client_options_default()` |
| `AZ_IOT_DEFAULT_IDENTITY_RECOVERY_MAX_DELAY_MS` | 3600000 | `identity_recovery.policy.max_delay_ms`, set by `az_iot_connection_client_options_default()` |
| `AZ_IOT_DEFAULT_IDENTITY_RECOVERY_JITTER_PCT` | 25 | `identity_recovery.policy.jitter_pct`, set by `az_iot_connection_client_options_default()` |

#### Feature clients ([az_iot_message.h](../inc/azure/iot/az_iot_message.h) and the client headers)

| Macro | Default | Bounds |
| --- | --- | --- |
| `AZ_IOT_TWIN_MAX_PENDING` | 8 | Twin requests in flight per twin client. |
| `AZ_IOT_DM_MAX_INFLIGHT` | 4 | Direct-method invocations held per client (mqttv3; default for mqttv5). When full, a new mqttv3 invocation is dropped with a `WARN` log and gets no response. |
| `AZ_IOT_DM_METHOD_NAME_MAX` | 96 | Direct-method name. |
| `AZ_IOT_DM_RID_MAX` | 32 | mqttv3 request ID. |
| `AZ_IOT_DM_CORR_DATA_MAX` | 64 | mqttv5 correlation data. |
| `AZ_IOT_MQTTV3_DM_RESPONSE_TIMEOUT_SECONDS` | 300 | Default for `az_iot_mqttv3_direct_method_client_set_response_timeout()`. |
| `AZ_IOT_MQTTV5_DM_MAX_CONCURRENT` | `AZ_IOT_DM_MAX_INFLIGHT` | mqttv5 invocations probed or awaiting an answer. A probe past it is refused `DEVICE_BUSY`. The client reserves twice this many `max_pending_publishes` slots. |
| `AZ_IOT_MQTTV5_DM_MAX_METHODS` | 8 | Method names one mqttv5 client can declare. |
| `AZ_IOT_MQTTV5_DM_NOTICE_QUEUE_MAX` | `AZ_IOT_MQTTV5_DM_MAX_CONCURRENT` | mqttv5 probe rejections and abandons kept until their PUBACK, to be published again after a reconnect. Past it, one is sent untracked while `CONNECTED` and dropped otherwise. |
| `AZ_IOT_MQTTV5_DM_RESULT_BODY_MAX` | 512 | mqttv5 direct-method result body. The client holds one result buffer per `AZ_IOT_MQTTV5_DM_MAX_CONCURRENT` slot, so a result can wait for `CONNECTED` and be published again if its PUBACK is lost with the connection. |
| `AZ_IOT_MQTTV5_DM_TOPIC_MAX` | 192 | mqttv5 direct-method topic. |
| `AZ_IOT_FILE_UPLOAD_BODY_MAX` | 512 | File-upload request body; bounds the blob name (about 495 bytes once JSON-escaped). |
| `AZ_IOT_FILE_UPLOAD_URL_MAX` | 512 | File-upload request URL. |
| `AZ_IOT_FILE_UPLOAD_SAS_URI_MAX` | 2048 | Blob SAS URI. |
| `AZ_IOT_FILE_UPLOAD_CORR_ID_MAX` | 192 | File-upload correlation ID. |
| `AZ_IOT_FILE_UPLOAD_AUTHORIZATION_MAX` | 1024 | Hub SAS token passed to the file-upload HTTP hook; at least `AZ_IOT_SAS_TOKEN_SIZE()` of the hub host and device ID lengths (267 bytes together by default). |

#### Software updates ([az_iot_su.h](../inc/azure/iot/az_iot_su.h))

Only the manifest limits (`AZ_IOT_SU_MAX_STEP_COUNT`, `AZ_IOT_SU_MAX_FILE_COUNT`,
`AZ_IOT_SU_MAX_FILE_COUNT_PER_STEP`, see below) change the layout of `az_iot_su_client`, which the
application allocates. `az_iot_su_client_init()` returns `AZ_IOT_ERR_INVALID_ARG`, with an `ERROR`
log, when `sizeof(az_iot_su_client)` or any of them differs between the application and SDK builds.
The macros in this table only set defaults. `AZ_IOT_SU_WORKFLOW_ID_SIZE` (128) and
`AZ_IOT_SU_RETRY_TIMESTAMP_SIZE` (64) are fixed; defining them, or the removed
`AZ_IOT_SU_MAX_ROOT_KEYS`, `AZ_IOT_MAX_SU_OBSERVERS` and `AZ_IOT_SU_MAX_WORKFLOW_ID_LEN`, is a compile
error.

| Macro | Default | Bounds |
| --- | --- | --- |
| `AZ_IOT_SU_DEFAULT_MAX_ROOT_KEYS` | 8 | Default `az_iot_su_limits::max_root_keys`. |
| `AZ_IOT_SU_DEFAULT_MAX_OBSERVERS` | 4 | Default `az_iot_su_limits::max_observers`. |
| `AZ_IOT_SU_DEFAULT_MAX_UPDATE_SIZE` | 65536 | Default `az_iot_su_limits::max_update_size`. |
| `AZ_IOT_SU_DEFAULT_MAX_MESSAGE_SIZE` | `AZ_IOT_SU_MIN_MESSAGE_SIZE` (3456) | Default `az_iot_su_limits::max_message_size`. |
| `AZ_IOT_SU_DEFAULT_RESULT_DETAILS_SIZE` | 128 | Default `az_iot_su_limits::result_details_size`. |
| `AZ_IOT_SU_VERIFY_SCRATCH_SIZE` | 8192 | Stack used by `az_iot_su_parse_update_request()` for the decoded signature (JWS header with the signing key, and the signatures), and the default `az_iot_su_limits::scratch_size`. A signature that does not fit fails with `AZ_IOT_ERR_AUTH` and an `ERROR` log naming the part that is too large. |
| `AZ_IOT_SU_WORKFLOW_ID_SIZE` (fixed) | 128 | Workflow ID kept for reporting, duplicate detection and persistence. A deployment with a longer ID is refused: nothing is processed or reported, and `AZ_IOT_SU_EVENT_UPDATE_REFUSED` is raised with `AZ_IOT_ERR_NOT_ENOUGH_SPACE`. |
| `AZ_IOT_SU_RETRY_TIMESTAMP_SIZE` (fixed) | 64 | Copy of the deployment's `retryTimestamp` (ISO-8601, about 28 characters; not persisted). A longer one is treated as absent. |
| `AZ_IOT_SU_PERSIST_MAX_ATTEMPTS` | 5 | Consecutive failed state writes before the client stops retrying (1 to `0xFFFFFFFF`). |
| `AZ_IOT_SU_DEVICE_PROPERTIES_BUFFER_SIZE` | 512 | Default `az_iot_su_limits::device_properties_size`; `az_iot_su_device_properties_buffer_size()` gives the exact size. |
| `AZ_IOT_SU_APPLIED_UPDATE_ID_SIZE` | 192 | Installed or applied update ID (provider, name, version), NUL terminators included. Not overridable. |

The software updates client's variable-size memory is not set by macros. The application passes one
`workspace` buffer at `az_iot_su_client_init()`, sized with `az_iot_su_workspace_size()` (or
`AZ_IOT_SU_WORKSPACE_SIZE()` for static allocation) from `az_iot_su_limits`:

| Limit | Default | Bounds |
| --- | --- | --- |
| `max_update_size` | 65536 | Copy of the update metadata (manifest, signature and file URLs); a one-file offer is about 5 KiB. A larger one is refused: `AZ_IOT_SU_EVENT_UPDATE_REFUSED` with `AZ_IOT_ERR_NOT_ENOUGH_SPACE`. Also bounds the persisted state, `AZ_IOT_SU_STATE_BLOB_MAX_SIZE(max_update_size)`. |
| `scratch_size` | 8192 | Signature verification and string decoding. A signature part that does not fit fails verification; a file URL that does not fit is refused with `AZ_IOT_ERR_NOT_ENOUGH_SPACE`. |
| `max_message_size` | 3456 | Update-check and status-report bodies. At least `AZ_IOT_SU_MIN_MESSAGE_SIZE` (2176 plus 128 per `AZ_IOT_SU_MAX_STEP_COUNT`; 3456 by default), which fits the largest status report the client builds, so no report is refused for its size. |
| `device_properties_size` | 512 | Device-property strings (UTF-8 bytes, NUL terminators included), the only limit on compatibility properties besides their count. Size it with `az_iot_su_device_properties_buffer_size()` of the largest set the application will pass, including later `az_iot_su_client_update_device_properties()` calls; a set that does not fit is refused with `AZ_IOT_ERR_NOT_ENOUGH_SPACE` and an `ERROR` log of the bytes needed. Counted twice, plus `AZ_IOT_SU_APPLIED_UPDATE_ID_SIZE`: the client's copy and the update check's, which may also carry the applied update ID. |
| `max_root_keys` | 8 | Root keys `root_keys` may hold; more fail `az_iot_su_client_init()` with `AZ_IOT_ERR_NOT_ENOUGH_SPACE`. At most `AZ_IOT_SU_MAX_LIMIT_COUNT` (1024). |
| `max_observers` | 4 | Observers `az_iot_su_client_add_observer()` can register; one more returns `AZ_IOT_ERR_NOT_ENOUGH_SPACE`. At most `AZ_IOT_SU_MAX_LIMIT_COUNT` (1024). |
| `result_details_size` | 128 | `resultDetails` kept per step (`az_iot_su_client_set_step_result_details()`); at most 1024. Counted five times per step: the details, both persisted records and two copies of the reports sent. |

The workspace also holds `2 * AZ_IOT_SU_PERSIST_OVERHEAD` bytes for the persisted-state records and
up to `sizeof(az_iot_su_workspace_alignment) - 1` bytes to align the root keys and observers. These
values do not change the layout of `az_iot_su_client`.

#### Software updates manifest limits

`AZ_IOT_SU_MAX_STEP_COUNT`, `AZ_IOT_SU_MAX_FILE_COUNT` and `AZ_IOT_SU_MAX_FILE_COUNT_PER_STEP` bound
an update: steps in its manifest, files in its manifest and `fileUrls`, and files one step
references. The defaults (10, 10, 10) are the Device Update limits, so no update it accepts is
refused for its step or file count. An update over a limit is not installed and the limit is named
in an `ERROR` log:

- more `fileUrls` than `AZ_IOT_SU_MAX_FILE_COUNT`: refused, nothing reported,
  `AZ_IOT_SU_EVENT_UPDATE_REFUSED` with `AZ_IOT_ERR_NOT_ENOUGH_SPACE`;
- a manifest over a limit, or with more than 2 hashes for a file: the workflow fails after
  manifest verification and is reported failed. `az_iot_su_parse_update_request()` returns
  `AZ_IOT_ERR_NOT_ENOUGH_SPACE`.

They size structures that azure-sdk-for-c parses into and that `az_iot_su_client` embeds, so
azure-sdk-for-c, the SDK and the application must be built with the same values. Set them as CMake
options of the SDK build; it applies them to the azure-sdk-for-c parser (`az_iot_hub`) and to
`az_iot_core` as public compile definitions, which reach the application through the `azure::iot::core` target and the
`azure-iot-sdk-core` pkg-config file. `az_iot_su.h` refuses to compile with values that differ from
azure-sdk-for-c's, and `az_iot_su_client_init()` refuses an application built with values that
differ from the SDK's. An installed azure-sdk-for-c must be built with them (see
[Installed dependencies](#installed-dependencies)).

Smaller values save memory on constrained devices, for example
`-DAZ_IOT_SU_MAX_STEP_COUNT=2 -DAZ_IOT_SU_MAX_FILE_COUNT=2`. Sizes on a 64-bit build:

| | 2 steps, 2 files | 10 steps, 10 files (default) |
| --- | --- | --- |
| `az_iot_su_client` | 3952 | 7920 |
| Default workspace (`AZ_IOT_SU_WORKSPACE_DEFAULT_SIZE`) | 80591 | 87311 |
| `az_iot_su_client_update_manifest` (also on the stack of `az_iot_su_parse_update_request()`) | 464 | 3216 |
| `AZ_IOT_SU_MIN_MESSAGE_SIZE` | 2432 | 3456 |

#### Dispatch and logging

| Macro | Default | Bounds |
| --- | --- | --- |
| `AZ_IOT_DISPATCH_PREFIX_MAX` | 128 | Topic prefix of one handler. Fixed. |
| `AZ_IOT_LOG_MESSAGE_MAX` | 384 | One formatted log message, terminator included. Longer messages are truncated and end in `...`. |
| `AZ_IOT_LOG_FILE_PATH_MAX` | 256 | Log file path for the file sink, terminator included. |
| `AZ_IOT_LOG_FILE_DEFAULT_MAX_BYTES` | 1 MiB | Default size at which the file sink rotates. |
| `AZ_IOT_LOG_FILE_DEFAULT_MAX_FILES` | 3 | Default number of rotated log files kept. |

#### In-flight QoS 1 publishes

A QoS 1 publish sent with a completion callback holds one `max_pending_publishes` slot until
its acknowledgement arrives or the session ends. A feature client may reserve slots for its own
publishes at init; the rest form a shared pool. Neither side uses the other's slots. A reservation
is granted only when usable at once: if tracked publishes in flight occupy its slots, it fails with
`AZ_IOT_ERR_BUSY`; retry once they are acknowledged.

| Feature client | QoS 1 publishes | Own bound | Slots |
| --- | --- | --- | --- |
| mqttv3 and mqttv5 telemetry `send` | 1 per call | Only the shared pool | Shared pool; the callback is required. |
| mqttv5 direct methods | Probe ack, result, abandon | `AZ_IOT_MQTTV5_DM_MAX_CONCURRENT` invocations. Not bounded: refused probes are also acknowledged, and refused probes past `AZ_IOT_MQTTV5_DM_NOTICE_QUEUE_MAX` are sent untracked. A result keeps its slot until acknowledged. | Reserves `2 × AZ_IOT_MQTTV5_DM_MAX_CONCURRENT` at init; init fails with `AZ_IOT_ERR_NOT_ENOUGH_SPACE` if they do not fit, or `AZ_IOT_ERR_BUSY` as above. Probe acks and results never go untracked: when all are in use they wait for the next PUBACK. Probe rejections and abandons leave one slot free per invocation that could still need one, and past that are sent untracked. |
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
| `az_iot_certificate_provider_managed_options` | Bootstrap and operational certificate and key files, key type (EC P-256, EC P-384 or RSA 2048) | [az_iot_certificate_provider_managed.h](../adapters/cert_openssl/az_iot_certificate_provider_managed.h), [sample](../samples/authentication/dps_csr_managed/README.md) |
| `az_iot_su_client_config_options` (`az_iot_su_client_config_options_default()`) | Platform hooks, root keys, device properties, replacement policy. Crypto comes from the connection client. | [az_iot_su.h](../inc/azure/iot/az_iot_su.h), [samples](../samples/software_update/pc/simulated_onboarding/README.md) |
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
| `PAHO_C_CLIENT_USE_HTTP_PROXY` | Eclipse Paho C | `TRUE` lets Paho read `http_proxy` / `https_proxy` when the connection options set no proxy. See [Dependencies](dependencies.md#proxy-from-the-environment-is-opt-in). |
| `http_proxy`, `https_proxy`, `no_proxy` | Eclipse Paho C | HTTP proxy used when the connection options set none and `PAHO_C_CLIENT_USE_HTTP_PROXY=TRUE`. Lowercase only; `https_proxy` needs an `http://` or `https://` scheme. Set `proxy` in the connection options to avoid this; see [Connecting a device](connecting.md#network-websockets-and-proxies). |
