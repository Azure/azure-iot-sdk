# AMQP vs MQTT: Protocol Decision for the New Azure IoT C Client

## Executive Summary

The new Azure IoT C client should support **MQTT only**. All core IoT Hub and DPS features (telemetry, device twins, direct methods, cloud-to-device messages, provisioning) are fully available over MQTT. The AMQP-exclusive capabilities (connection multiplexing, seamless token refresh) serve niche gateway scenarios and do not justify the ~20× increase in protocol library complexity.

---

## Protocol Fundamentals

| Aspect | MQTT 3.1.1 / 5 | AMQP 1.0 |
|--------|-----------------|-----------|
| Design philosophy | Lightweight pub/sub for constrained devices | Enterprise messaging with rich semantics |
| Wire overhead | 2-byte minimum header | Variable framing + type system |
| Connection model | 1 device = 1 TCP connection | Multiplexed sessions/links over 1 connection |
| Security layer | TLS + username/password or X.509 | TLS + SASL (PLAIN, ANONYMOUS, CBS) |
| RAM footprint | Tens of KB | Hundreds of KB to MB |
| Specification size | ~80 pages | ~120 pages (core) + extensions |
| Default port | 8883 (or 443 via WebSocket) | 5671 (or 443 via WebSocket) |

---

## Azure IoT Hub & DPS Feature Matrix

Every feature a typical device needs works identically on both protocols:

| Feature | MQTT | AMQP | Notes |
|---------|:----:|:----:|-------|
| D2C telemetry | ✔ | ✔ | |
| C2D messages | ✔ | ✔ | |
| Device twin (get/patch) | ✔ | ✔ | |
| Direct methods | ✔ | ✔ | |
| DPS provisioning (symmetric key) | ✔ | ✔ | |
| DPS provisioning (X.509) | ✔ | ✔ | |
| File upload | — | — | HTTPS only; protocol-independent |
| IoT Plug and Play | ✔ | ✔ | |
| **Connection multiplexing** | ✘ | ✔ | Gateway scenarios only |
| **Seamless SAS token refresh (CBS)** | ✘ | ✔ | MQTT reconnects briefly every ~48 min |
| **Batch message sending** | ✘ | ✔ | Marginal throughput gain |

**Bottom line:** The three AMQP-exclusive features are relevant only to multi-device gateway deployments using SAS keys at high message rates.

---

## Implementation Cost Analysis

Data drawn from the existing Azure IoT C SDK (`azure-iot-sdk-c`):

| Metric | MQTT stack | AMQP stack | Ratio |
|--------|-----------|------------|-------|
| Protocol library source files | 3 (`umqtt/src/`) | 25 (`uamqp/src/`) | 8× |
| Protocol library headers | 4 | 81 | 20× |
| Protocol library LOC (approx.) | ~3,000 | ~55,000 | **18×** |
| IoT Hub transport layer files | 3 | 11 | 4× |
| IoT Hub transport layer LOC | ~6,000 | ~14,000 | 2.3× |
| **Total protocol + transport** | **~9,000 LOC** | **~69,000 LOC** | **~8×** |

### Cost to the new SDK (`azure-iot-sdk`)

The new client's architecture is **MQTT-native by design**:

- The adapter vtable (`az_iot_mqtt_iface_t`) exposes `connect`, `subscribe`, `publish`, `process_loop` — MQTT semantics.
- Topic-based dispatch routes inbound messages by parsing MQTT topic strings.
- Adding AMQP would require:
  1. A **parallel adapter interface** (link/session/sender/receiver semantics don't map to pub/sub).
  2. Bringing in or writing an **AMQP 1.0 library** (~55K LOC equivalent).
  3. A **second dispatch layer** — AMQP uses link addresses, not topic strings.
  4. **Multiplexing orchestrator** managing shared connections.

**Estimated effort:** 3–6 engineer-months for implementation + ongoing maintenance burden that equals or exceeds the rest of the SDK combined.

---

## Customer Impact: Moving from AMQP to MQTT

| Aspect | Impact | Severity |
|--------|--------|:--------:|
| Telemetry, twins, methods, C2D, DPS | **Fully retained** — no feature loss | — |
| Device footprint (RAM / flash) | **Reduced** — protocol library ~18× smaller | ✅ Gain |
| SDK update velocity | **Faster** — less code to validate and ship | ✅ Gain |
| Debugging & diagnostics | **Simpler** — 3 source files vs 25; topic-based flow easy to trace | ✅ Gain |
| Broader device support | **Improved** — runs on MCUs with <1 MB RAM | ✅ Gain |
| Network traversal (port 443) | **Equivalent** — MQTT-over-WebSocket supported | — |
| MQTT v5 features (new SDK) | **Gained** — user properties, request/response, topic aliases | ✅ Gain |
| Connection multiplexing | **Lost** — each device opens its own TLS connection | ⚠️ Loss |
| SAS token refresh continuity | **Not applicable** — new SDK uses X.509 only (no token expiry) | — |
| Batch message sending | **Lost** — messages sent individually (negligible for most workloads) | ⚠️ Minor |

### Who is affected by the losses?

| Lost capability | Affected scenario | Mitigation |
|-----------------|-------------------|------------|
| Multiplexing | Field gateways aggregating 100+ leaf devices | Use connection pooling at the gateway or retain the legacy SDK for gateway-only roles |
| Batch sending | Extremely high-throughput telemetry (>10K msg/s per device) | Aggregate payloads at the application layer before publishing |

---

## Recommendation

**Ship the new client with MQTT support only.** The engineering cost of adding AMQP (~55K LOC library + ~14K LOC transport + new architectural layers) is disproportionate to the narrow set of scenarios it enables. Customers requiring multiplexing can continue using the legacy SDK or adopt gateway-level connection management. For all other scenarios — which represent the vast majority of IoT deployments — MQTT delivers full feature parity at a fraction of the complexity.
