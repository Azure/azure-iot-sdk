// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* In-process, for-testing TCP proxy: fault injection and traffic inspection.
 *
 * This is deliberately NOT a fake MQTT adapter (that tier is owned by
 * c/tests/support/mock_mqtt_iface). The whole value of this proxy is that the
 * genuine MQTT client + TLS stack sits in the path: the proxy interposes a raw
 * TCP passthrough between the client and a real upstream broker so the
 * adapter's connect / reconnect / network-error paths become deterministically
 * drivable (drop after N bytes or N MQTT packets, not on a timer).
 *
 * It lives beside the adapter-agnostic conformance suite and reuses the same
 * env-var broker discovery, so BYO-adapter customers can exercise the same
 * failure injection against their own client.
 *
 * Layers, in the order they were added: plaintext TCP passthrough plus
 * deterministic connection drops (C1); TLS termination with a runtime-generated
 * CA (C2); synthetic CONNACK/DISCONNECT and keep-alive starvation (C3); and
 * per-direction network impairment -- latency, jitter, bandwidth,
 * fragmentation and stalls -- scheduled in userspace so no OS-specific traffic
 * control is needed on any CI leg (C4); and rules that act on parsed MQTT
 * packets, to inject refusals a real broker will not produce on demand (C5).
 *
 * Threading: the proxy owns one background thread that accepts a single client
 * connection at a time, opens an upstream connection, and pumps bytes in both
 * directions. All control and observability entry points are thread-safe and
 * may be called before or after a client connects.
 *
 * Deliberate non-capabilities. This is a test fixture, not an interception
 * tool, and is kept that way on purpose:
 *   - It ALWAYS binds to 127.0.0.1 on an ephemeral port. The listen address is
 *     not configurable, so it cannot be exposed to another host.
 *   - It cannot intercept traffic that was not aimed at it. There is no
 *     ARP/DNS spoofing and no transparent-proxy mode; a client reaches it only
 *     because the test explicitly connects to the port az_iot_test_proxy_start()
 *     returns.
 *   - A CA it generates is self-signed, lives only in memory for the life of
 *     the process, and is never added to any trust store. Only the certificate
 *     is exported; the private key never leaves the process.
 *   - It never records forwarded traffic. Bytes are counted, never logged and
 *     never written to disk.
 * Please keep it that way.
 */
#ifndef AZ_IOT_TEST_PROXY_H
#define AZ_IOT_TEST_PROXY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

  /* Opaque proxy handle. */
  typedef struct az_iot_test_proxy az_iot_test_proxy;

  typedef struct az_iot_test_proxy_options
  {
    /* Upstream (real) broker the proxy forwards to. Required. */
    const char* upstream_host;
    uint16_t upstream_port;
  } az_iot_test_proxy_options;

  az_iot_test_proxy_options az_iot_test_proxy_options_default(void);

  /* Bind a listener on 127.0.0.1 (fixed; see the note above) on an ephemeral
   * port and start the pump thread. On success returns 0, writes the owning
   * handle to *out_proxy and the chosen port to *out_port (point a client at
   * 127.0.0.1:*out_port). Returns non-zero on failure (nothing is allocated). */
  int az_iot_test_proxy_start(
      const az_iot_test_proxy_options* options,
      az_iot_test_proxy** out_proxy,
      uint16_t* out_port);

  /* Deterministic drop controls. Each is one-shot: once it fires, the proxy
   * hard-resets the active connection and disarms, so a subsequent reconnect
   * succeeds (the reconnect-after-drop scenario). `bytes`/`packets` count the
   * client->broker direction within the current connection; 0 disarms. */
  void az_iot_test_proxy_reset_after_bytes(az_iot_test_proxy* proxy, uint64_t bytes);
  void az_iot_test_proxy_reset_after_packets(az_iot_test_proxy* proxy, uint32_t packets);

  /* Immediately hard-reset the active connection (best-effort; no-op if none). */
  void az_iot_test_proxy_drop_now(az_iot_test_proxy* proxy);

  /* Write shaping, applied to BOTH directions. Shorthand for setting the
   * matching field of az_iot_test_proxy_impairment on each direction; see that
   * struct for the full set (bandwidth, jitter, stall).
   *
   * These used to be documented as applying to both directions but only ever
   * shaped client->broker, which is the less interesting half: it is the
   * broker->client direction that exercises a client's partial-read and
   * reassembly paths. */
  void az_iot_test_proxy_set_write_delay_ms(az_iot_test_proxy* proxy, unsigned delay_ms);
  void az_iot_test_proxy_set_fragment(az_iot_test_proxy* proxy, size_t max_chunk);

  /* --- Network impairment: the proxy is a userspace relay, so added latency,
   * jitter, a bandwidth ceiling and write fragmentation are all just scheduling
   * of buffered bytes. That means they behave identically on every CI leg with
   * no tc/netem, no WinDivert, no kernel modules and no administrator rights.
   *
   * Deliberately absent, because a TCP relay cannot honestly provide them:
   *   - Packet loss. Below this proxy the kernel retransmits; dropping bytes
   *     here corrupts the stream instead of simulating loss. What loss actually
   *     looks like to an MQTT client -- delay spikes, stalls, resets -- is
   *     covered by base_delay_ms/jitter_ms, az_iot_test_proxy_stall() and the
   *     existing reset controls.
   *   - Reordering and duplication. TCP delivers a byte stream in order, so
   *     real reordering is invisible above it and reordering bytes here would
   *     simply corrupt the stream. Duplication that means anything to a client
   *     is packet-level (a PUBLISH with DUP set), which belongs to control
   *     packet injection rather than to byte shaping.
   * --- */
  typedef enum az_iot_test_proxy_direction
  {
    AZ_IOT_TEST_PROXY_C2B = 0, /* client -> broker */
    AZ_IOT_TEST_PROXY_B2C = 1 /* broker -> client */
  } az_iot_test_proxy_direction;

  typedef struct az_iot_test_proxy_impairment
  {
    /* Added latency before a chunk is released, plus a uniform random extra in
     * [0, jitter_ms]. Order is always preserved: a jittered chunk never
     * overtakes one queued before it. */
    unsigned base_delay_ms;
    unsigned jitter_ms;
    /* Split each forwarded buffer into pieces of at most this many bytes.
     * 0 disables fragmentation. */
    size_t fragment_max;
    /* Token-bucket ceiling in bytes per second; 0 is unlimited. */
    uint32_t bytes_per_sec;
    /* Seeds the jitter sequence so a failure reproduces. 0 selects a fixed
     * default seed rather than something time-derived. */
    uint32_t seed;
  } az_iot_test_proxy_impairment;

  az_iot_test_proxy_impairment az_iot_test_proxy_impairment_default(void);

  /* Apply (or clear, with a zeroed struct) impairment for one direction. Safe
   * to call before or during a connection; takes effect on the next chunk. */
  void az_iot_test_proxy_set_impairment(
      az_iot_test_proxy* proxy,
      az_iot_test_proxy_direction dir,
      const az_iot_test_proxy_impairment* impairment);

  /* Hold all traffic in `dir` for `ms`, then resume. Distinct from a drop: the
   * connection stays open and nothing is lost, which is what a client sees
   * during a radio gap or a paused VM. */
  void az_iot_test_proxy_stall(
      az_iot_test_proxy* proxy,
      az_iot_test_proxy_direction dir,
      unsigned ms);

  /* Bytes currently buffered and not yet released in `dir`. Lets a test wait
   * for a shaped transfer to finish instead of sleeping for a guessed time. */
  uint64_t az_iot_test_proxy_queued_bytes(
      az_iot_test_proxy* proxy,
      az_iot_test_proxy_direction dir);

  /* Cumulative count of write calls issued in `dir`. A fragmented stream shows
   * far more writes than packets, so a test can prove the shaping it asked for
   * was actually applied rather than assume it. This counts calls, never
   * content; the proxy still records no traffic. */
  uint64_t az_iot_test_proxy_writes(az_iot_test_proxy* proxy, az_iot_test_proxy_direction dir);

  /* --- Scriptable broker behaviour. The controls above shape bytes without
   * looking at them; these act on parsed MQTT packets, which is what it takes
   * to produce a refusal a real broker would never send on demand -- a SUBACK
   * carrying a failure code, an acknowledgement for a packet id nobody sent, a
   * PUBLISH cut short mid-flight.
   *
   * A rule matches packets of one type travelling in one direction, and either
   * changes that packet (suppress, truncate, corrupt) or has a side effect
   * (inject, drop, stall). "Suppress the SUBSCRIBE, inject a SUBACK" is two
   * rules, which keeps each one doing a single thing.
   *
   * Rules run against a real broker session: everything not matched is still
   * forwarded, so the failure lands in the middle of a genuine connection
   * rather than on a stub. --- */

  /* MQTT control packet types, as they appear in the fixed header. Plain
   * constants rather than an enum so that matching on "any" and switching on a
   * type stay ordinary integer work. */
#define AZ_IOT_TEST_PROXY_PKT_ANY 0
#define AZ_IOT_TEST_PROXY_PKT_CONNECT 1
#define AZ_IOT_TEST_PROXY_PKT_CONNACK 2
#define AZ_IOT_TEST_PROXY_PKT_PUBLISH 3
#define AZ_IOT_TEST_PROXY_PKT_PUBACK 4
#define AZ_IOT_TEST_PROXY_PKT_PUBREC 5
#define AZ_IOT_TEST_PROXY_PKT_PUBREL 6
#define AZ_IOT_TEST_PROXY_PKT_PUBCOMP 7
#define AZ_IOT_TEST_PROXY_PKT_SUBSCRIBE 8
#define AZ_IOT_TEST_PROXY_PKT_SUBACK 9
#define AZ_IOT_TEST_PROXY_PKT_UNSUBSCRIBE 10
#define AZ_IOT_TEST_PROXY_PKT_UNSUBACK 11
#define AZ_IOT_TEST_PROXY_PKT_PINGREQ 12
#define AZ_IOT_TEST_PROXY_PKT_PINGRESP 13
#define AZ_IOT_TEST_PROXY_PKT_DISCONNECT 14
#define AZ_IOT_TEST_PROXY_PKT_AUTH 15

/* Injected packets are hand-built acknowledgements, not payloads. */
#define AZ_IOT_TEST_PROXY_RULE_BYTES_MAX 64
#define AZ_IOT_TEST_PROXY_RULES_MAX 8

  typedef enum az_iot_test_proxy_action
  {
    /* Send `bytes` to `inject_dir`. The matched packet is still forwarded
     * unless a separate suppress rule says otherwise. */
    AZ_IOT_TEST_PROXY_ACTION_INJECT = 0,
    /* Drop the matched packet from the stream; the peer never sees it. */
    AZ_IOT_TEST_PROXY_ACTION_SUPPRESS,
    /* Forward only the first `truncate_to` bytes of the matched packet, so the
     * peer is left waiting for a body that never arrives. */
    AZ_IOT_TEST_PROXY_ACTION_TRUNCATE,
    /* XOR `corrupt_mask` into the byte at `corrupt_offset`. */
    AZ_IOT_TEST_PROXY_ACTION_CORRUPT,
    /* Reset the connection once the matched packet has been handled. */
    AZ_IOT_TEST_PROXY_ACTION_DROP,
    /* Hold `stall_dir` for `stall_ms`. */
    AZ_IOT_TEST_PROXY_ACTION_STALL
  } az_iot_test_proxy_action;

  typedef struct az_iot_test_proxy_rule
  {
    /* Which stream to watch, and which packet type (ANY matches every type). */
    az_iot_test_proxy_direction dir;
    uint8_t on_packet;
    /* Ignore this many matches before firing: "the second PUBLISH". */
    uint32_t skip;
    /* Fire every time rather than once. Zero -- the default for a zeroed
     * struct -- means once, which is what a one-shot fault wants. */
    int repeat;
    az_iot_test_proxy_action action;

    /* INJECT. `bytes` is copied when the rule is added, so it need not outlive
     * the call. An acknowledgement has to carry the packet id of whatever it
     * answers, so `echo_packet_id` overwrites the two bytes at
     * `packet_id_offset` with the id of the matched packet. */
    az_iot_test_proxy_direction inject_dir;
    const uint8_t* bytes;
    size_t bytes_len;
    int echo_packet_id;
    size_t packet_id_offset;

    size_t truncate_to;
    size_t corrupt_offset;
    uint8_t corrupt_mask;
    unsigned stall_ms;
    az_iot_test_proxy_direction stall_dir;
  } az_iot_test_proxy_rule;

  /* Returns the rule's index, or -1. Rejected: a table that is already full, a
   * direction or packet type outside its range, an action that is not one of
   * the values above, and an INJECT with no bytes, more bytes than
   * AZ_IOT_TEST_PROXY_RULE_BYTES_MAX, or an out-of-range inject_dir. Fields
   * belonging to other actions are not validated -- a truncate length on an
   * INJECT rule is ignored, not an error. */
  int az_iot_test_proxy_add_rule(az_iot_test_proxy* proxy, const az_iot_test_proxy_rule* rule);

  void az_iot_test_proxy_clear_rules(az_iot_test_proxy* proxy);

  /* How many times the rule at `index` fired. Counts are cumulative across
   * connections; clearing the rules resets them. Lets a test assert the fault
   * it asked for was actually injected instead of assuming it. */
  uint32_t az_iot_test_proxy_rule_hits(az_iot_test_proxy* proxy, size_t index);

  /* Observability (cumulative across connections). */
  uint64_t az_iot_test_proxy_bytes_forwarded(az_iot_test_proxy* proxy);
  uint32_t az_iot_test_proxy_packets_seen(az_iot_test_proxy* proxy);
  uint32_t az_iot_test_proxy_connections(az_iot_test_proxy* proxy);

  /* --- TLS termination (C2): present a runtime-generated leaf to the client so
   * certificate-rejection paths (untrusted chain, expiry, hostname) are drivable
   * without an external TLS broker. Requires an OpenSSL build. --- */
  typedef struct az_iot_test_proxy_tls_options
  {
    /* SAN embedded in the presented leaf, in OpenSSL form ("IP:127.0.0.1" or
     * "DNS:host"). NULL => "IP:127.0.0.1". */
    const char* leaf_san;
    /* Leaf validity relative to now, in seconds. Both 0 => a valid window of
     * [-1h, +24h]. Set not_after_offset_sec negative for an expired leaf. */
    long not_before_offset_sec;
    long not_after_offset_sec;
    /* Sign the leaf with a second CA that is NOT the one az_iot_test_proxy_ca_pem
     * exports, so the client's chain validation fails. */
    int sign_with_untrusted_ca;
  } az_iot_test_proxy_tls_options;

  az_iot_test_proxy_tls_options az_iot_test_proxy_tls_options_default(void);

  /* Non-zero if the proxy was built with TLS-termination support (OpenSSL). */
  int az_iot_test_proxy_tls_supported(void);

  /* Enable client-facing TLS termination, generating a CA + leaf per `tls` (or
   * defaults when NULL). Call before a client connects. Returns 0 on success,
   * non-zero if TLS is unsupported or generation failed. */
  int az_iot_test_proxy_enable_tls(
      az_iot_test_proxy* proxy,
      const az_iot_test_proxy_tls_options* tls);

  /* Copy the PEM of the CA the client must trust into `out` (NUL-terminated).
   * Returns bytes written (excluding NUL), or 0 on error / if TLS isn't enabled. */
  size_t az_iot_test_proxy_ca_pem(az_iot_test_proxy* proxy, char* out, size_t cap);

  /* --- Control-packet injection / synthetic broker (C3): drive CONNACK-code
   * routing, server DISCONNECT handling and keep-alive timeouts without a real
   * broker. Once a synthetic CONNACK is set the proxy answers the client's
   * CONNECT with it (no upstream connection) then stays silent except for any
   * injected DISCONNECT; because it never emits PINGRESP an idle client with a
   * short keep-alive will time out. Bytes are the full MQTT packets the caller
   * builds for its version. Returns 0 on success, non-zero on bad args. --- */
  int az_iot_test_proxy_set_synthetic_connack(
      az_iot_test_proxy* proxy,
      const uint8_t* connack,
      size_t len);
  int az_iot_test_proxy_set_synthetic_disconnect(
      az_iot_test_proxy* proxy,
      const uint8_t* disconnect,
      size_t len,
      unsigned delay_ms);

  /* Stop the pump thread, close all sockets and free the handle. NULL-safe. */
  void az_iot_test_proxy_stop(az_iot_test_proxy* proxy);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_TEST_PROXY_H */
