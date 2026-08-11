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
 * control is needed on any CI leg (C4).
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
