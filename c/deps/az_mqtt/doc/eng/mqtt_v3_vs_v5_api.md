# MQTT 3.1.1 and 5.0: shared core, per-version libraries

## Decision

Three libraries:

| Library (CMake target) | Contents |
|---|---|
| `az_mqtt_core` (`az_mqtt::core`) | Transport (TCP/TLS), packet framing and receive buffering, send path, keep-alive, packet identifiers, session state and close handling, wire primitives, result codes. Version-independent. |
| `az_mqttv3` (`az_mqtt::mqttv3`) | MQTT 3.1.1 API (`az_mqtt3_*`), types, codec and packet handling. |
| `az_mqttv5` (`az_mqtt::mqttv5`) | MQTT 5.0 API (`az_mqtt5_*`), types, codec and packet handling. |

- Each version library links the core and calls it directly; there is no function table.
- The core references no version symbol.
- An application links the version(s) it uses. Both can be linked together, sharing one core.

This replaces two earlier layouts:
- **Two copies** (`az_mqtt5`, and `az_mqtt3` kept in sync as a copy): client, transport and types were duplicated, and mqttv3 carried MQTT 5.0 fields.
- **One client with a run-time codec table**: the table's indirect calls blocked inlining and kept every codec function linked, so single-version programs grew.

### Why MQTT 3.1.1 is not a subset of 5.0

| Concern | MQTT 5.0 | MQTT 3.1.1 |
|---|---|---|
| **Packet format** | Fixed header + properties block | Fixed header, no properties |
| **CONNACK** | reason code + properties | return code (0-5) |
| **SUBACK** | reason codes | return codes (0x00-0x02, 0x80) |
| **PUBACK/PUBREC/PUBREL/PUBCOMP** | reason code + properties | packet identifier only |
| **DISCONNECT** | both directions, reason code + properties | client only, empty |
| **AUTH** | exists | reserved packet type |
| **Session** | Clean Start + Session Expiry | Clean Session |

So each version has its own API and types. `az_mqtt3_*` types carry only MQTT 3.1.1 fields.
Framing, transport, keep-alive and session handling are identical, and live in the core.

### Layout

```
inc/az_mqtt/    az_mqtt_core.h (session state embedded in clients), az_mqtt_transport.h,
                az_mqtt_types.h (result codes, packet types, QoS, client state)
inc/az_mqtt3/   az_mqtt3_client.h, az_mqtt3_codec.h, az_mqtt3_types.h
inc/az_mqtt5/   az_mqtt5_client.h, az_mqtt5_codec.h, az_mqtt5_types.h
src/core/       az_mqtt_core.c, az_mqtt_codec_common.c, internal headers
src/mqtt3/      az_mqtt3_client.c, az_mqtt3_codec.c
src/mqtt5/      az_mqtt5_client.c, az_mqtt5_codec.c
src/platform/   transport backends (core)
```

The version client embeds `az_mqtt_core` as its first member. It encodes a packet into the send
buffer and asks the core to send it. The core frames received packets and hands each one to the
client's dispatch function.

### Size

Connect samples, x86-64, GCC 12, `-Os`, no TLS, against the previous `az_mqtt3` / `az_mqtt5`:

| | mqttv3 | mqttv5 |
|---|---|---|
| `.text` | −2,536 B | −84 B |
| Total (`size`), no LTO | −2,704 B | +68 B |
| Total, LTO | −1,698 B | −623 B |
| Total, no unwind tables | −2,600 B | −68 B |
| Client RAM | 248 B (was 432 B) | 432 B (unchanged) |

- The mqttv5 figures include the PUBLISH property-bounds fix (+67 B).
- The one positive figure (+68 B) is that fix plus unwind metadata (`.eh_frame`) for the extra out-of-line core functions; mqttv5 code (`.text`) is smaller.
- Both versions in one program: core + mqttv3 + mqttv5 is 20,894 B (`size` text of the archives), against 29,068 B for the previous two libraries.
