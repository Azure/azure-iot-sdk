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
 * control is needed on any CI leg (C4); rules that act on parsed MQTT packets,
 * to inject refusals a real broker will not produce on demand (C5); and mutual
 * TLS, so the proxy can demand a client certificate and issue the good and bad
 * ones a test needs to present (C6).
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

#include <stdbool.h>
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
    /* Upstream (real) broker the proxy forwards to. Required UNLESS
     * http_connect is set, which makes the client name the upstream instead. */
    const char* upstream_host;
    uint16_t upstream_port;

    /* Behave as an HTTP CONNECT proxy rather than a transparent passthrough.
     *
     * The proxy reads a "CONNECT host:port HTTP/1.1" request, answers
     * "200 Connection established", and pumps bytes to the host the CLIENT
     * named -- so upstream_host/upstream_port are ignored and one fixture can
     * front DPS and a hub in the same test, which a passthrough cannot.
     *
     * Everything the passthrough offers still applies once the tunnel is open:
     * the drop controls, the impairments and the counters all see the tunnelled
     * bytes. TLS is NOT terminated here -- the client negotiates it end to end
     * with the real broker inside the tunnel, which is the property that makes
     * a proxied session worth testing at all.
     *
     * NOTE, and the reason the "cannot intercept" wording above is narrower in
     * this mode: a CONNECT proxy dials whatever host the client asks for. It
     * still only ever receives connections a test aimed at its loopback port,
     * so it intercepts nothing; but it is no longer limited to one preset
     * upstream. */
    bool http_connect;

    /* Require HTTP Basic proxy authentication, and refuse with 407 when the
     * client's credentials are absent or wrong. NULL (the default) accepts any
     * client, authenticated or not.
     *
     * These are the DECODED credentials the proxy expects, exactly as the
     * caller configured them on az_iot_mqtt_proxy_options. Checking them here
     * is what proves the SDK's own encoding round-trips: a client that mangles
     * a credential containing a delimiter fails to authenticate against this,
     * which no unit test on the generated string can demonstrate.
     *
     * Ignored unless http_connect is set. */
    const char* required_username;
    const char* required_password;

    /* Carry the stream as opaque bytes: shape and forward, without framing it
     * as MQTT.
     *
     * The pump normally reassembles each MQTT packet before forwarding, which
     * is what lets rules match on packet type and what makes the packet
     * counters meaningful. That framing assumes the stream IS MQTT, so a
     * WebSocket session -- an HTTP upgrade followed by WS frames carrying MQTT
     * inside them -- stalls: the framer waits for a packet length it read out
     * of bytes that were never an MQTT header, and forwards nothing.
     *
     * Set this to put a proxy in front of a WebSocket listener. The
     * impairments still apply (they shape writes, which needs no protocol
     * knowledge), so fragmentation, latency, bandwidth and stalls all work.
     * What does NOT work in this mode, because it has no packets to act on:
     * az_iot_test_proxy_add_rule(), reset_after_packets, and
     * az_iot_test_proxy_packets_seen(), which stays 0.
     *
     * This is the narrow form of the protocol/transport split tracked in
     * docs/design.md section 4.5; it separates the two for the cases that never
     * needed the protocol, rather than introducing the full codec seam. */
    bool opaque_stream;
  } az_iot_test_proxy_options;

  az_iot_test_proxy_options az_iot_test_proxy_options_default(void);

  /* Initialize through az_iot_test_proxy_options_default() rather than by
   * aggregate initialization: it zeroes the struct, and every field added here
   * is APPENDED and means "not requested" when zero, so a caller written
   * against an older revision keeps its behaviour (passthrough, no
   * authentication) without being edited.
   *
   * There is no size/version guard because there is nothing to guard against:
   * this is a test fixture, built as a static library from this tree and never
   * installed or shipped, so its header and its objects cannot be mixed across
   * revisions the way a distributed library's can. */

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

  /* --- Session-setup inspection. The proxy decodes FIELDS of the two control
   * packets that carry a session's terms -- the client's CONNECT and its
   * DISCONNECT -- and publishes them here.
   *
   * This does not weaken the "records no traffic" rule above and is not a step
   * toward a codec: nothing is stored except the handful of scalars below, no
   * PUBLISH is ever looked at, and no payload is retained (the Will is reported
   * by topic and length, never by content). What it buys is the only honest way
   * to assert that an adapter put the Clean Start flag, the Session Expiry
   * Interval or the Will on the wire -- the client's own API cannot testify to
   * that, and a broker will not report it back.
   *
   * Both are per-connection and reset when a new client connects, so a test
   * that reconnects reads the terms of the latest session, not the first. --- */

#define AZ_IOT_TEST_PROXY_WILL_TOPIC_MAX 128

  typedef struct az_iot_test_proxy_connect_fields
  {
    /* 0 until a CONNECT has been decoded on this connection; everything below
     * is meaningless while it is 0. */
    int seen;
    /* Protocol level byte: 4 for MQTT 3.1.1, 5 for MQTT 5. */
    uint8_t protocol_level;
    /* CONNECT flags bit 1: Clean Session in 3.1.1, Clean Start in 5. */
    int clean_flag;
    int will_flag;
    uint8_t will_qos;
    int will_retain;
    char will_topic[AZ_IOT_TEST_PROXY_WILL_TOPIC_MAX];
    size_t will_payload_len;
    /* MQTT 5 CONNECT properties. `has_*` distinguishes "absent" from "present
     * and zero", which is the distinction a v3.1.1 assertion rests on: a
     * v3.1.1 CONNECT has no property field at all, so these are never set. */
    int has_session_expiry;
    uint32_t session_expiry_seconds;
    int has_will_delay;
    uint32_t will_delay_seconds;
  } az_iot_test_proxy_connect_fields;

  /* Copy the decoded CONNECT of the current connection into `out`. Returns 1
   * when one has been seen, 0 otherwise (`out` is zeroed either way). */
  int az_iot_test_proxy_last_connect_fields(
      az_iot_test_proxy* proxy,
      az_iot_test_proxy_connect_fields* out);

  /* Reason code of the last DISCONNECT the CLIENT sent on this connection.
   * Returns 1 and writes the code when one has been seen, 0 otherwise. A
   * DISCONNECT with an empty body -- every 3.1.1 one, and a v5 one that omits
   * the code -- reports 0, which is what both mean: Normal Disconnection. */
  int az_iot_test_proxy_last_client_disconnect_reason(az_iot_test_proxy* proxy, uint8_t* out_code);

  /* CONNECT-mode observability. All cumulative, all zero in passthrough mode.
   *
   * These are what let a test prove the session went THROUGH the tunnel rather
   * than around it: a client that ignored the proxy and dialled the broker
   * directly leaves tunnels_opened at 0 while still reaching CONNECTED. */

  /* Tunnels the proxy answered with 2xx and then pumped. */
  uint32_t az_iot_test_proxy_tunnels_opened(az_iot_test_proxy* proxy);
  /* CONNECT requests refused with 407 (missing or wrong credentials). */
  uint32_t az_iot_test_proxy_auth_failures(az_iot_test_proxy* proxy);
  /* The authority from the last CONNECT request line ("host:port"), or NULL if
   * none has been received. Lets a test assert WHICH endpoint was tunnelled --
   * the DPS gateway first, then the assigned hub. Valid until the next
   * CONNECT; copy it if it must outlive that. */
  const char* az_iot_test_proxy_last_connect_target(az_iot_test_proxy* proxy);

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

    /* Ask the client for a certificate and refuse the handshake if it does not
     * present one that the proxy's CA signed. Without this the proxy only ever
     * proves things about *server* certificates, so no amount of client-side
     * fixture work can produce a rejection: a peer that never asks cannot
     * refuse. */
    int require_client_cert;

    /* Ask the client for a certificate but accept whatever it presents, without
     * checking the issuer.
     *
     * For proving a client holds a private key, the chain is the wrong thing to
     * look at: what proves possession is the CertificateVerify signature, which
     * TLS requires the client to produce with that key. A key that lives inside
     * a token cannot be handed to this proxy's CA to be certified, so requiring
     * the proxy's own issuer would make such a key untestable for the one
     * property that matters. The handshake completing IS the proof.
     *
     * Ignored when require_client_cert is set. */
    int accept_any_client_cert;
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

  /* Mint a client certificate for the client to present. Signed by the same CA
   * the proxy verifies against, unless `sign_with_untrusted_ca` asks for one it
   * will not accept.
   *
   * The offsets place the validity window relative to now, so an expired
   * certificate is a negative number rather than a fixture that has to be
   * re-minted by hand every time it ages out -- which is how the previous
   * embedded fixtures died.
   *
   * The default window applies only when BOTH offsets are zero. Setting just
   * one leaves the other at zero and means it literally: a certificate with
   * `not_after_offset_sec` unset expires the instant it is issued.
   *
   * Writes NUL-terminated PEM into `cert_pem` and `key_pem`. Returns 0 on
   * success, -1 if TLS is unavailable, no CA has been generated yet, or either
   * buffer is too small. */
  typedef struct az_iot_test_proxy_client_cert_options
  {
    const char* common_name; /* NULL for a default */
    long not_before_offset_sec;
    long not_after_offset_sec;
    int sign_with_untrusted_ca;
  } az_iot_test_proxy_client_cert_options;

  az_iot_test_proxy_client_cert_options az_iot_test_proxy_client_cert_options_default(void);

  int az_iot_test_proxy_issue_client_cert(
      az_iot_test_proxy* proxy,
      const az_iot_test_proxy_client_cert_options* options,
      char* cert_pem,
      size_t cert_cap,
      char* key_pem,
      size_t key_cap);

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
