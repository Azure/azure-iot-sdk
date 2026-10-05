# MQTT v3.1.1 Spec Compliance

This table maps every **MUST**, **SHOULD**, and **SHALL** client-side requirement from the
[OASIS MQTT Version 3.1.1 specification](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html)
to the current implementation status of `az_mqttv3` (on `az_mqtt_core`).

> Requirement IDs follow the notation used in the specification, e.g. `[[MQTT-3.1.2-1]](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html)`.
> Where the spec does not assign an explicit tag, the relevant section is cited instead.

| # | Requirement | Req ID | Supported | Notes | References |
|---|-------------|--------|-----------|-------|------------|
| **General / Fixed Header ([§2](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html))** |||||
| 1 | Fixed header: packet type in bits 7-4, flags in bits 3-0 | [§2.1.1](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html) | Yes | | [src/core/az_mqtt_codec_common.c](../../src/core/az_mqtt_codec_common.c) |
| 2 | Remaining Length encoded as Variable Byte Integer (1–4 bytes) | [§2.2.3](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html) | Yes | VBI encoder / decoder in packet read/write | [src/core/az_mqtt_codec_common.c](../../src/core/az_mqtt_codec_common.c), [src/core/az_mqtt_core.c](../../src/core/az_mqtt_core.c) |
| 3 | Maximum remaining length is 268,435,455 bytes | [§2.2.3](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html) | Yes | 4-byte VBI cap enforced in decode loop | [src/core/az_mqtt_core.c](../../src/core/az_mqtt_core.c) |
| 4 | String data preceded by 2-byte big-endian length prefix | [§2.3.1](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html) | Yes | `_az_mqtt_read_binary_data` / `_az_mqtt_write_binary_data` | [src/core/az_mqtt_codec_common.c](../../src/core/az_mqtt_codec_common.c) |
| 5 | UTF-8 strings must be valid UTF-8 (informative) | [§2.3.2](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html) | No | No UTF-8 validation is performed on encoded or decoded strings | |
| **CONNECT ([§3.1](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html))** |||||
| 6 | Protocol Name field must be `MQTT` (bytes: 0x00, 0x04, M, Q, T, T) | [§3.1.1](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html) | Yes | Hard-coded 4-byte literal in encoder | [src/mqtt3/az_mqtt3_codec.c](../../src/mqtt3/az_mqtt3_codec.c) |
| 7 | Protocol Version byte must be `4` (0x04) | [§3.1.1](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html) | Yes | `0x04` written in `az_mqtt3_codec_encode_connect` | [src/mqtt3/az_mqtt3_codec.c](../../src/mqtt3/az_mqtt3_codec.c) |
| 8 | CONNECT must be the first packet sent after the Network Connection is opened | [§3.1](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html) | Yes | `az_mqtt3_client_connect` opens transport then immediately sends CONNECT | [src/core/az_mqtt_core.c](../../src/core/az_mqtt_core.c) |
| 9 | Clean Session flag in Connect Flags | [§3.1.2.4](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html) | Yes | Flags byte bit set from `connect_options.clean_session` | [src/mqtt3/az_mqtt3_codec.c](../../src/mqtt3/az_mqtt3_codec.c) |
| 10 | Will Flag, Will QoS, Will Retain encoded in Connect Flags | [§3.1.2.4](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html) | Yes | `opts->will` checked; flags byte set accordingly | [src/mqtt3/az_mqtt3_codec.c](../../src/mqtt3/az_mqtt3_codec.c) |
| 11 | If Will Flag is 0, Will QoS and Will Retain must be 0 | [§3.1.2.4](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html) | No | Flags byte derived from `will` pointer being non-NULL; will QoS/retain bits not zeroed when will=NULL | |
| 12 | Will QoS must be 0, 1, or 2 | [§3.1.2.4](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html) | No | No validation; caller can pass invalid QoS without error | |
| 13 | Username and Password flags set appropriately | [§3.1.2.6](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html) | Yes | Flags byte tested against `username`/`password` span emptiness | [src/mqtt3/az_mqtt3_codec.c](../../src/mqtt3/az_mqtt3_codec.c) |
| 14 | Keep Alive field (0 = disabled; 1–65535 seconds) | [§3.1.2.10](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html) | Yes | `connect_options.keep_alive_seconds` | [inc/az_mqtt3/az_mqtt3_types.h](../../inc/az_mqtt3/az_mqtt3_types.h) |
| 15 | Client Identifier: may be 1–23 bytes (or empty for server assignment) | [§3.1.3.1](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html) | Partial | No length validation; accepts any span size | |
| 16 | Will Properties: Will Topic (if Will Flag=1) | [§3.1.3.2](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html) | Yes | `will_options.topic` written | [src/mqtt3/az_mqtt3_codec.c](../../src/mqtt3/az_mqtt3_codec.c) |
| 17 | Will Properties: Will Message (if Will Flag=1) | [§3.1.3.2](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html) | Yes | `will_options.payload` written | [src/mqtt3/az_mqtt3_codec.c](../../src/mqtt3/az_mqtt3_codec.c) |
| **CONNACK ([§3.2](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html))** |||||
| 18 | Reserved bits in Flags must be 0 | [§3.2.1](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html) | Yes | Decoder validates flags byte == 0 | [src/mqtt3/az_mqtt3_codec.c](../../src/mqtt3/az_mqtt3_codec.c) |
| 19 | Session Present flag decoded | [§3.2.2.1](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html) | Yes | `connack_data.session_present` | [src/mqtt3/az_mqtt3_codec.c](../../src/mqtt3/az_mqtt3_codec.c) |
| 20 | Connect Return code decoded (0x00 = success, 0x01–0x05 = various failures) | [§3.2.2.3](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html) | Yes | Stored verbatim in `connack_data.return_code` (`az_mqtt3_connack_return_code`) | [src/mqtt3/az_mqtt3_codec.c](../../src/mqtt3/az_mqtt3_codec.c), [src/mqtt3/az_mqtt3_client.c](../../src/mqtt3/az_mqtt3_client.c) |
| 21 | If return code is non-zero, server closes the Network Connection | [§3.2.2.2](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html) | Yes | `_handle_connack` leaves state DISCONNECTED when return code ≥ 0x01 | [src/core/az_mqtt_core.c](../../src/core/az_mqtt_core.c) |
| **PUBLISH ([§3.3](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html))** |||||
| 22 | PUBLISH encoded with DUP, QoS, RETAIN in first byte flags | [§3.3.1](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html) | Yes | `az_mqtt3_codec_encode_publish` | [src/mqtt3/az_mqtt3_codec.c](../../src/mqtt3/az_mqtt3_codec.c) |
| 23 | DUP flag must be 0 for QoS 0 | [§3.3.1](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html) | No | No validation; caller may set DUP via raw flags | |
| 24 | QoS must be 0, 1, or 2 | [§3.3.1](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html) | No | No validation | |
| 25 | Topic Name field: must not contain wildcard characters in send | [§3.3.2.1](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html) | No | No topic validation on outgoing PUBLISH | |
| 26 | Packet Identifier present when QoS > 0 | [§3.3.2.2](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html) | Yes | Encoder writes `packet_id` when QoS>0 | [src/mqtt3/az_mqtt3_codec.c](../../src/mqtt3/az_mqtt3_codec.c) |
| 27 | Packet Identifier must not be 0 for QoS > 0 | [§3.3.2.2](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html) | Yes | The next identifier skips 0 | [src/core/az_mqtt_core.c](../../src/core/az_mqtt_core.c) |
| 28 | Packet Identifier must be unique across all in-flight packets | [§3.3.2.2](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html) | Yes | An identifier is skipped while an outgoing request holds it in the caller's `inflight_control_buffer` entries; with no free entry, QoS > 0 PUBLISH, SUBSCRIBE and UNSUBSCRIBE fail with `AZ_MQTT_ERROR_FLOW_CONTROL` | [src/core/az_mqtt_core.c](../../src/core/az_mqtt_core.c) |
| 29 | DUP flag on received PUBLISH decoded | [§3.3.1](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html) | Yes | `publish_data.dup` | [src/mqtt3/az_mqtt3_codec.c](../../src/mqtt3/az_mqtt3_codec.c) |
| 30 | Retain flag encoded/decoded | [§3.3.1](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html) | Yes | `publish_options.retain` / `publish_data.retain` | [src/mqtt3/az_mqtt3_codec.c](../../src/mqtt3/az_mqtt3_codec.c) |
| 31 | Retain flag must be 0 when forwarding (server responsibility; client receives them correctly) | [§3.3.1](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html) | Yes | Client only receives; retain bit from server in received PUBLISH | [src/mqtt3/az_mqtt3_codec.c](../../src/mqtt3/az_mqtt3_codec.c) |
| 32 | QoS 1: client sends PUBACK in response to received PUBLISH | [§3.4](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html) | Yes | `_handle_publish` auto-sends PUBACK | [src/mqtt3/az_mqtt3_client.c](../../src/mqtt3/az_mqtt3_client.c) |
| 33 | QoS 1: PUBACK contains packet identifier of the PUBLISH | [§3.4](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html) | Yes | `packet_id` from decoded PUBLISH forwarded | [src/mqtt3/az_mqtt3_client.c](../../src/mqtt3/az_mqtt3_client.c) |
| 34 | QoS 2 (receive): client sends PUBREC | [§3.5](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html) | Yes | `_handle_publish` sends PUBREC for QoS 2 | [src/mqtt3/az_mqtt3_client.c](../../src/mqtt3/az_mqtt3_client.c) |
| 35 | QoS 2 (receive): client sends PUBCOMP in response to PUBREL | [§3.7](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html) | Yes | `_handle_pubrel` sends PUBCOMP | [src/mqtt3/az_mqtt3_client.c](../../src/mqtt3/az_mqtt3_client.c) |
| 36 | QoS 2 (receive): client must not deliver the application message a second time after sending PUBREC | [§3.5](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html) | Partial | Held in an `inflight_control_buffer` entry until PUBREL; a resent PUBLISH is acknowledged, not delivered. With no free entry it is delivered without duplicate detection | [src/core/az_mqtt_core.c](../../src/core/az_mqtt_core.c), [src/mqtt3/az_mqtt3_client.c](../../src/mqtt3/az_mqtt3_client.c) |
| 37 | QoS 2 (send): client sends PUBREL in response to PUBREC | [§3.6](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html) | Yes | `_handle_pubrec` sends PUBREL | [src/mqtt3/az_mqtt3_client.c](../../src/mqtt3/az_mqtt3_client.c) |
| 38 | QoS 1/2 (send): on a resumed session the client resends each unacknowledged PUBLISH (DUP=1) and PUBREL (flags 0010, no DUP) with its original packet identifier | [§4.4](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html) | Yes | On CONNACK Session Present: PUBREL resent; then PUBLISH, in order, with DUP and its packet identifier from its copy in `inflight_message_buffer` (required for QoS 1/2 on a session that outlives the connection). Only a message given an expiry interval may be dropped instead, once it elapsed (opt-in) | [src/core/az_mqtt_core.c](../../src/core/az_mqtt_core.c) |
| **PUBACK ([§3.4](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html))** |||||
| 39 | PUBACK encoded with packet identifier (2-byte fixed form) | [§3.4](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html) | Yes | `az_mqtt3_codec_encode_puback` | [src/mqtt3/az_mqtt3_codec.c](../../src/mqtt3/az_mqtt3_codec.c) |
| 40 | PUBACK decoded — packet identifier only (fixed 2-byte form) | [§3.4](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html) | Yes | `az_mqtt3_codec_decode_ack` handles PUBACK; trailing bytes are rejected as malformed | [src/mqtt3/az_mqtt3_codec.c](../../src/mqtt3/az_mqtt3_codec.c) |
| **PUBREC / PUBREL / PUBCOMP ([§3.5](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html)–3.7)** |||||
| 41 | PUBREC encoded with packet identifier (2-byte fixed form) | [§3.5](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html) | Yes | `az_mqtt3_codec_encode_pubrec` | [src/mqtt3/az_mqtt3_codec.c](../../src/mqtt3/az_mqtt3_codec.c) |
| 42 | PUBREL encoded with reserved flags = 0x02 and packet identifier (2-byte fixed form) | [§3.6](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html) | Yes | `_encode_simple_ack` called with flags=0x02 | [src/mqtt3/az_mqtt3_codec.c](../../src/mqtt3/az_mqtt3_codec.c) |
| 43 | PUBCOMP encoded with packet identifier (2-byte fixed form) | [§3.7](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html) | Yes | `az_mqtt3_codec_encode_pubcomp` | [src/mqtt3/az_mqtt3_codec.c](../../src/mqtt3/az_mqtt3_codec.c) |
| 44 | PUBREC / PUBREL / PUBCOMP decoded — packet identifier only (fixed 2-byte form) | [§3.5–3.7](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html) | Yes | `az_mqtt3_codec_decode_ack` handles all three | [src/mqtt3/az_mqtt3_codec.c](../../src/mqtt3/az_mqtt3_codec.c) |
| **SUBSCRIBE ([§3.8](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html))** |||||
| 45 | SUBSCRIBE encoded with fixed flags = 0x02 | [§3.8.1](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html) | Yes | `0x02` hard-coded in `az_mqtt3_codec_encode_subscribe` | [src/mqtt3/az_mqtt3_codec.c](../../src/mqtt3/az_mqtt3_codec.c) |
| 46 | SUBSCRIBE contains at least one Topic Filter | [§3.8.3](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html) | No | No validation that `sub_count > 0` | |
| 47 | Subscription Options: QoS (0, 1, or 2) | [§3.8.3.1](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html) | Yes | `subscription.qos` | [src/mqtt3/az_mqtt3_codec.c](../../src/mqtt3/az_mqtt3_codec.c) |
| 48 | Subscription Options: Reserved bits must be 0 | [§3.8.3.1](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html) | Yes | Only QoS bits (0–1) set; bits 2–7 left as 0 | [src/mqtt3/az_mqtt3_codec.c](../../src/mqtt3/az_mqtt3_codec.c) |
| **SUBACK ([§3.9](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html))** |||||
| 49 | SUBACK decoded — packet identifier and return codes (one per topic) | [§3.9](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html) | Yes | `az_mqtt3_codec_decode_suback` | [src/mqtt3/az_mqtt3_codec.c](../../src/mqtt3/az_mqtt3_codec.c) |
| 50 | SUBACK return codes: 0x00 (max QoS 0), 0x01 (max QoS 1), 0x02 (max QoS 2), 0x80 (failure) | [§3.9.3](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html) | Yes | Decoded and returned to caller | [src/mqtt3/az_mqtt3_codec.c](../../src/mqtt3/az_mqtt3_codec.c) |
| **UNSUBSCRIBE ([§3.10](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html))** |||||
| 51 | UNSUBSCRIBE encoded with fixed flags = 0x02 | [§3.10.1](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html) | Yes | `0x02` hard-coded | [src/mqtt3/az_mqtt3_codec.c](../../src/mqtt3/az_mqtt3_codec.c) |
| 52 | UNSUBSCRIBE must contain at least one Topic Filter | [§3.10.3](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html) | No | No validation that `filter_count > 0` | |
| **UNSUBACK ([§3.11](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html))** |||||
| 53 | UNSUBACK decoded — packet identifier only (2-byte fixed form) | [§3.11](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html) | Yes | `az_mqtt3_codec_decode_ack` | [src/mqtt3/az_mqtt3_codec.c](../../src/mqtt3/az_mqtt3_codec.c) |
| **PINGREQ / PINGRESP ([§3.12](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html)–3.13)** |||||
| 54 | PINGREQ encoded (2-byte packet, no payload) | [§3.12](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html) | Yes | `_az_mqtt_encode_pingreq` | [src/core/az_mqtt_codec_common.c](../../src/core/az_mqtt_codec_common.c) |
| 55 | Client must send PINGREQ when no packet sent within Keep Alive interval | [§3.1.2.10](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html) | Yes | `az_mqtt3_client_process_loop` checks `last_send_time_ms` vs `keep_alive_seconds` | [src/core/az_mqtt_core.c](../../src/core/az_mqtt_core.c) |
| 56 | PINGRESP received and handled (no action required by client) | [§3.13](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html) | Yes | `_dispatch_packet` returns `AZ_OK` on PINGRESP | [src/core/az_mqtt_core.c](../../src/core/az_mqtt_core.c) |
| 57 | Client should close Network Connection if no PINGRESP within reasonable time | [§3.1.2.10](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html) | Yes | `process_loop` closes the session with `AZ_MQTT_ERROR_KEEP_ALIVE_TIMEOUT` when a PINGREQ gets no packet back within the keep-alive | [src/core/az_mqtt_core.c](../../src/core/az_mqtt_core.c) |
| 58 | Keep Alive of 0 disables the mechanism | [§3.1.2.10](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html) | Yes | Timer check is guarded by `keep_alive_seconds > 0` | [src/core/az_mqtt_core.c](../../src/core/az_mqtt_core.c) |
| **DISCONNECT ([§3.14](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html))** |||||
| 59 | DISCONNECT encoded (2-byte fixed form: no variable header, no payload) | [§3.14](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html) | Yes | `az_mqtt3_codec_encode_disconnect`; `az_mqtt3_client_disconnect` | [src/mqtt3/az_mqtt3_codec.c](../../src/mqtt3/az_mqtt3_codec.c), [src/mqtt3/az_mqtt3_client.c](../../src/mqtt3/az_mqtt3_client.c) |
| 60 | DISCONNECT is client-to-server only | [§3.14](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html) | Yes | A received DISCONNECT is a protocol error (session closed with `AZ_MQTT_ERROR_PROTOCOL`) | [src/mqtt3/az_mqtt3_client.c](../../src/mqtt3/az_mqtt3_client.c) |
| **Operational Behavior ([§4](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html))** |||||
| 61 | A client must not send any packet other than CONNECT before CONNACK is received | [§3.1](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html) | Yes | State machine: `AZ_MQTT_CLIENT_STATE_CONNECTING` prevents publish/subscribe | [src/mqtt3/az_mqtt3_client.c](../../src/mqtt3/az_mqtt3_client.c) |
| 62 | Keep Alive: client must send PINGREQ if no packet within Keep Alive interval | [§3.1.2.10](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html) | Yes | `process_loop` timer check | [src/core/az_mqtt_core.c](../../src/core/az_mqtt_core.c) |
| 63 | On receipt of CONNACK with return code 0x00, connection successful | [§3.2.2.3](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html) | Yes | `_handle_connack` checks return code and sets state to CONNECTED | [src/mqtt3/az_mqtt3_client.c](../../src/mqtt3/az_mqtt3_client.c) |
| 64 | On receipt of CONNACK with non-zero return code, connection fails and server closes | [§3.2.2.2](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html) | Yes | `_handle_connack` leaves state DISCONNECTED; `az_mqtt3_client_connect` closes transport | [src/mqtt3/az_mqtt3_client.c](../../src/mqtt3/az_mqtt3_client.c) |
| 65 | Payload of PUBLISH must be preceded by Topic Name (2-byte length + data) | [§3.3.2](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html) | Yes | `_az_mqtt_read_binary_data` / `_az_mqtt_write_binary_data` | [src/mqtt3/az_mqtt3_codec.c](../../src/mqtt3/az_mqtt3_codec.c) |
| 66 | Topic wildcards (+, #) permitted only in SUBSCRIBE, never in PUBLISH | [§3.3.2.1, 4.7.2](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html) | No | No topic validation on outgoing PUBLISH | |
| 67 | Client must accept packets up to 268 MB (client resource permitting) | [§4.6](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html) | Yes | Receive buffer is caller-provided and can be arbitrarily large | [src/mqtt3/az_mqtt3_client.c](../../src/mqtt3/az_mqtt3_client.c) |
| 68 | Client must close Network Connection on receipt of a Malformed Packet | [§4.8.3.1](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html) | Yes | `process_loop` closes the transport and reports the error via `on_connection_closed` | [src/core/az_mqtt_core.c](../../src/core/az_mqtt_core.c) |
| 69 | Client must close Network Connection on receipt of a Protocol Error | [§4.8.3.2](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html) | Yes | `process_loop` closes the transport and reports the error via `on_connection_closed` | [src/core/az_mqtt_core.c](../../src/core/az_mqtt_core.c) |

---

## Summary

| Status | Count |
|--------|-------|
| Yes | 57 |
| Partial | 2 |
| No | 10 |

### Key gaps (client-facing impact)

| Gap | Spec reference |
|-----|---------------|
| QoS 2 receive duplicate detection needs a free `inflight_control_buffer` entry | [§3.5](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html) |
| A QoS 1/2 PUBLISH given an expiry interval is dropped instead of resent once it elapsed (opt-in; MQTT itself would resend it) | [§3.3.1, 3.6](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html) |
| Topic wildcards not validated in PUBLISH | [§3.3.2.1](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html) |
| UTF-8 string validation not performed | [§2.3.2](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html) |
| Will flag consistency validation not enforced | [§3.1.2.4](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html) |
