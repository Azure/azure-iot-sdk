# az_mqtt thread safety

## What is needed to use az_mqtt in a thread-safe way?

The az_mqtt client object is not internally synchronized.

Why:

- az_mqtt3_client and az_mqtt5_client contain mutable shared state (next_packet_id, recv_buf_pos, timestamps, state, and caller-provided send/receive buffers).
- Public API calls mutate that state and perform I/O.
- The implementation has no internal mutex/lock around client state.

### Practical rule

Treat one az_mqtt3_client / az_mqtt5_client instance as single-thread-owned unless your application adds synchronization.

### Recommended integration patterns

1. Single-owner I/O thread (preferred)

- One dedicated thread owns the client and is the only code path that calls:
  - az_mqtt{3,5}_client_connect
  - az_mqtt{3,5}_client_process_loop
  - az_mqtt{3,5}_client_publish
  - az_mqtt{3,5}_client_subscribe
  - az_mqtt{3,5}_client_unsubscribe
  - az_mqtt{3,5}_client_disconnect
- Other threads communicate desired operations through a thread-safe queue.
- Incoming callback work is handed off quickly to worker threads via queue; keep callbacks short.

2. External lock around every client call

- Protect every API call touching the same client instance with the same mutex.
- Also protect callback interactions with shared app state.
- Avoid calling az_mqtt API recursively from callbacks unless you have designed for reentrancy.

### Additional notes

- Caller-owned buffers (send/receive and property arrays) must stay valid for the whole client lifetime and be protected by the same ownership/locking model.
- If you need parallel publishing from many threads, use a command queue into the owner thread rather than calling publish concurrently.
- If you need true concurrent MQTT sessions, create one client instance per thread/session, each with its own buffers and transport.

## Quick checklist

- [ ] Exactly one synchronization domain per client instance.
- [ ] No concurrent API calls on the same client without external locking.
- [ ] az_mqtt{3,5}_client_process_loop runs regularly in the owner thread.
- [ ] Callbacks do minimal work and hand off to worker threads.
- [ ] All caller-provided buffers outlive the client and are not concurrently mutated.

## Evidence pointers in this repo

- Client state layout (caller-owned buffers and mutable fields): inc/az_mqtt/az_mqtt_core.h, inc/az_mqtt3/az_mqtt3_client.h, inc/az_mqtt5/az_mqtt5_client.h
- Shared mutable client state and I/O processing: src/core/az_mqtt_core.c
