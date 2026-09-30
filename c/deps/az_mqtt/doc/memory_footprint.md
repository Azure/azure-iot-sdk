# Memory Footprint

## Current layout

GCC 12.2, `-O3 -DNDEBUG`, x86-64, Debian 12, no TLS. An application links `az_mqtt_core` plus
`az_mqttv3`, `az_mqttv5`, or both.

| Module | Library | .text | .rodata | .data | .bss |
|---|---|---:|---:|---:|---:|
| az_mqtt_core.c | core | 2,806 B | 0 | 0 | 0 |
| az_mqtt_codec_common.c | core | 1,399 B | 0 | 0 | 0 |
| transport_posix.c | core | 1,361 B | 0 | 0 | 0 |
| az_mqtt_socket_posix.c | core | 3,215 B | 7 B | 0 | 0 |
| az_mqtt3_client.c | mqttv3 | 2,208 B | 56 B | 0 | 0 |
| az_mqtt3_codec.c | mqttv3 | 2,651 B | 37 B | 0 | 0 |
| az_mqtt5_client.c | mqttv5 | 2,894 B | 64 B | 0 | 0 |
| az_mqtt5_codec.c | mqttv5 | 9,993 B | 299 B | 0 | 0 |
| **mqttv3 application** | core + mqttv3 | **13,640 B** | **100 B** | **0** | **0** |
| **mqttv5 application** | core + mqttv5 | **21,668 B** | **370 B** | **0** | **0** |
| **Both versions** | core + mqttv3 + mqttv5 | **26,527 B** | **463 B** | **0** | **0** |

RAM per client: `az_mqtt3_client` 248 B, `az_mqtt5_client` 432 B (x86-64).

## Before the shared core (az_mqtt5 library)

The figures below were measured on the earlier MQTT 5.0-only library, with GCC 13.3,
`-O3 -DNDEBUG`, x86-64, Ubuntu 24.04, no TLS. Identifiers are as they were then.

### az_mqtt5_client Library

| Module | .text (code) | .rodata | .data | .bss | Total |
|---|---:|---:|---:|---:|---:|
| az_mqtt5_codec.c | 19,736 B | 292 B | 0 | 0 | 20,028 B |
| az_mqtt5_client.c | 4,964 B | 64 B | 0 | 0 | 5,028 B |
| transport_posix.c | 1,324 B | 3 B | 0 | 0 | 1,327 B |
| **Total** | **26,024 B** | **359 B** | **0** | **0** | **~26 KB** |

- Archive on disk: **47 KB** (includes ELF metadata, `.eh_frame`, relocations — not loaded at runtime)
- Zero `.data` and `.bss`: no mutable global state; all buffers are caller-provided via `az_span`
- Zero dynamic memory allocations

#### Largest Symbols

| Symbol | Size (bytes) | Module |
|---|---:|---|
| `az_mqtt5_codec_encode_connect` | 3,988 | codec |
| `az_mqtt5_codec_decode_connack` | 1,893 | codec |
| `az_mqtt5_codec_decode_publish` | 1,526 | codec |
| `az_mqtt5_codec_encode_publish` | 1,829 | codec |
| `az_mqtt5_client_process_loop` | 1,702 | client |
| `az_mqtt5_client_connect` | 697 | client |
| `az_mqtt5_client_publish` | 367 | client |

### Comparison with Paho MQTT C v1.3.13

Identical build conditions: GCC 13.3, `-O3 -DNDEBUG`, x86-64, Ubuntu 24.04, static library, no TLS.

#### Summary

| Metric | az_mqtt5_client | Paho MQTT C (sync) | Ratio |
|---|---:|---:|---|
| .text (code) | 26,024 B | 149,372 B | **5.7x smaller** |
| .data (initialized globals) | 0 B | 2,068 B | — |
| .bss (uninitialized globals) | 0 B | 617,848 B | — |
| Total loaded in RAM | 26,024 B | 769,288 B | **29.6x smaller** |
| Archive on disk | 47 KB | 340 KB | **7.2x smaller** |
| Object files | 3 | 25 | |
| Dynamic allocations | 0 | Heavy | |

#### Paho MQTT C Breakdown

| Paho Module | .text | .data | .bss | Purpose |
|---|---:|---:|---:|---|
| StackTrace.c | 2,592 | 0 | 616,148 | Debug stack trace buffer |
| MQTTClient.c | 30,215 | 52 | 616 | Client logic + thread sync |
| Socket.c | 14,709 | 0 | 152 | Socket abstraction |
| MQTTProtocolClient.c | 13,375 | 0 | 0 | Protocol handling |
| MQTTPacket.c | 11,912 | 288 | 16 | Packet encode/decode |
| MQTTPersistence.c | 10,873 | 0 | 0 | File-based persistence |
| MQTTPersistenceDefault.c | 6,458 | 0 | 0 | Default persistence backend |
| WebSocket.c | 10,018 | 0 | 48 | WebSocket support |
| Tree.c | 6,365 | 0 | 0 | Tree data structure (malloc) |
| MQTTProperties.c | 6,028 | 432 | 0 | MQTT 5 properties |
| MQTTPacketOut.c | 5,074 | 0 | 0 | Outbound packets |
| SocketBuffer.c | 4,442 | 0 | 56 | Socket buffering |
| Log.c | 4,051 | 28 | 676 | Logging subsystem |
| Heap.c | 4,000 | 0 | 136 | Heap tracking |
| MQTTProtocolOut.c | 3,813 | 0 | 0 | Outbound protocol |
| LinkedList.c | 3,500 | 0 | 0 | Linked list (malloc) |
| Thread.c | 3,067 | 0 | 0 | Threading |
| Messages.c | 1,897 | 376 | 0 | Message strings |
| Base64.c | 1,471 | 0 | 0 | Base64 codec |
| SHA1.c | 1,430 | 64 | 0 | SHA-1 hash |
| MQTTReasonCodes.c | 1,097 | 720 | 0 | Reason code strings |
| Proxy.c | 1,116 | 0 | 0 | Proxy support |
| utf-8.c | 971 | 108 | 0 | UTF-8 validation |
| MQTTTime.c | 746 | 0 | 0 | Time utilities |
| Clients.c | 152 | 0 | 0 | Client registry |

#### Key Differences

- **Zero `.data` and `.bss`** — az_mqtt5_client has no mutable global state; all buffers are caller-provided via `az_span`. Paho uses 600+ KB of `.bss` for its StackTrace module alone.
- **No dynamic allocations** — az_mqtt5_client operates entirely on caller-owned stack/static buffers. Paho relies on `malloc` through Heap.c, Tree.c, and LinkedList.c.
- **No bundled infrastructure** — Paho ships persistence, threading, WebSocket, Base64/SHA1, heap tracking, and logging subsystems. az_mqtt5_client delegates these concerns to the caller and platform layer.
- **Code density** — az_mqtt5_client's entire `.text` footprint (~26 KB) is smaller than Paho's single `MQTTClient.c` object (30 KB).
