// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Internal helper for the e2e service client: the AMQP 1.0 paths (telemetry
 * receive from the Event Hub-compatible endpoint, and cloud-to-device send to
 * the IoT Hub service endpoint), built on the vendored `az_amqp` library and
 * its sample reference transport.
 *
 * This header is INTERNAL to the az_iot_e2e_service static library. The e2e
 * suites never include it (only its unit test does), so the `az_amqp` dependency
 * stays behind the service-client facade.
 */
#ifndef AZ_IOT_E2E_AMQP_H
#define AZ_IOT_E2E_AMQP_H

#include <azure/az_amqp.h>

#include "az_amqp_sample_transport.h"
#include "az_iot_e2e_service.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define E2E_AMQP_MAX_PARTITIONS 32
#define E2E_AMQP_CAPTURE_MAX 64
#define E2E_AMQP_CAPTURE_BODY_MAX 1024
#define E2E_AMQP_CAPTURE_PROPS_MAX 4096
#define E2E_AMQP_WATCHED_MAX 4
#define E2E_AMQP_WATCHED_NEEDLE_MAX 192
#define E2E_AMQP_CAPTURE_FILTER_MAX 192
/* Longest a telemetry watcher may take to connect, from its first or restarted setup step. */
#define E2E_AMQP_SETUP_TIMEOUT_S 60
/* Largest message one partition receiver reassembles (heap, per partition): past the 1 MB
 * message limit plus the properties and annotations the hub adds, so an oversized message the
 * hub accepted is still seen. The link silently drops a larger one. */
#define E2E_AMQP_RECV_MESSAGE_MAX (1280 * 1024)

  /** @brief One received telemetry message: a body prefix and every property, as text. */
  typedef struct e2e_amqp_capture
  {
    char body[E2E_AMQP_CAPTURE_BODY_MAX]; /* NUL-terminated prefix, for substring search */
    size_t body_size; /* full body size */
    /* "name\0value\0" pairs, named by section: "amqp:<field>" for the properties section,
     * "app:<key>" for application properties, "annotation:<key>" for message annotations. */
    char props[E2E_AMQP_CAPTURE_PROPS_MAX];
    size_t props_len;
    bool props_truncated;
    uint32_t seq; /* arrival order since the watcher began, from 0 */
  } e2e_amqp_capture;

  /** @brief A step of the telemetry watcher's connection setup, each advanced by
   * e2e_amqp_telemetry_do_work(). */
  typedef enum
  {
    E2E_AMQP_TELEMETRY_PHASE_IDLE = 0, /**< Not connected. */
    E2E_AMQP_TELEMETRY_PHASE_OPENING_CONNECTION,
    E2E_AMQP_TELEMETRY_PHASE_BEGINNING_SESSION,
    E2E_AMQP_TELEMETRY_PHASE_OPENING_CBS,
    E2E_AMQP_TELEMETRY_PHASE_AUTHORIZING,
    E2E_AMQP_TELEMETRY_PHASE_RECEIVING, /**< Receivers attached or attaching. */
  } e2e_amqp_telemetry_phase;

  /** @brief The outcome of a CBS put-token request. */
  typedef struct e2e_amqp_put_token_result
  {
    bool done;
    uint32_t status;
  } e2e_amqp_put_token_result;

  /* A telemetry watcher: an AMQP connection to the IoT Hub Event Hub-compatible
   * endpoint with one receiver link per partition. Received messages are
   * captured into a ring (newest overwrite oldest) so the test can poll for a
   * correlation marker. The watcher replays the last minutes of a hub other runs
   * are sending to, often more messages than the ring holds, so only bodies
   * containing the capture filter are captured.
   *
   * The struct is large (frame + per-partition buffers) and therefore intended to
   * be heap-allocated as part of the owning service object. */
  typedef struct e2e_amqp_telemetry
  {
    az_amqp_sample_transport transport_storage;
    az_amqp_transport transport;
    az_amqp_connection connection;
    az_amqp_session session;
    az_amqp_cbs cbs;
    az_amqp_link receivers[E2E_AMQP_MAX_PARTITIONS];
    int partition_count;
    e2e_amqp_telemetry_phase phase;
    time_t setup_deadline; /* a connection setup not done by then fails */
    const char* error; /* static reason of the last failure, or NULL */
    bool connection_failed;
    e2e_amqp_put_token_result put_token;
    /* Lowest sequence number not received yet, per partition: a reconnect replays events. */
    int64_t next_sequence_number[E2E_AMQP_MAX_PARTITIONS];

    /* Capture ring of recently received messages that contain capture_filter. */
    e2e_amqp_capture captured[E2E_AMQP_CAPTURE_MAX];
    int captured_count;
    int captured_next; /* ring slot the next message is written to */
    uint32_t received_count; /* data messages received since begin, captured or not */
    char capture_filter[E2E_AMQP_CAPTURE_FILTER_MAX]; /* empty: capture every message */
    bool eviction_reported; /* the first eviction from the ring has been logged */

    /** @brief Needles matched against every full body on arrival; a match latches until end. */
    char watched[E2E_AMQP_WATCHED_MAX][E2E_AMQP_WATCHED_NEEDLE_MAX];
    bool watched_seen[E2E_AMQP_WATCHED_MAX];
    int watched_count;

    /* Backing storage referenced by the az_amqp objects above. */
    uint8_t incoming_buffer[AZ_AMQP_DEFAULT_MAX_FRAME_SIZE];
    uint8_t outgoing_buffer[AZ_AMQP_DEFAULT_MAX_FRAME_SIZE];
    az_amqp_session* session_slots[1];
    az_amqp_link* link_slots[E2E_AMQP_MAX_PARTITIONS + 2];
    uint8_t cbs_reply_buffer[1024];
    uint8_t* recv_buffers; /* partition_count * E2E_AMQP_RECV_MESSAGE_MAX, heap */
    char source_addr[E2E_AMQP_MAX_PARTITIONS][256];
    char link_name[E2E_AMQP_MAX_PARTITIONS][32];
    char audience_buffer[256];
    uint8_t filter_buffer[256];
    az_span filter; /* encoded enqueued-time filter in filter_buffer, or empty */
    char host[256];
    char sas_token[512];
  } e2e_amqp_telemetry;

  /**
   * @brief Connects to @p eh_host:5671, CBS-authorizes @p sas_token against the entity and
   * attaches one receiver per partition, within #E2E_AMQP_SETUP_TIMEOUT_S.
   *
   * @param[in] consumer_group Consumer group to read from; `NULL` or empty selects `$Default`.
   * @param[in] enqueued_after_ms Only messages enqueued after this Unix time (ms) are delivered;
   * 0 reads from the start of the stream.
   * @param[in] capture_filter See e2e_amqp_telemetry_set_capture_filter().
   * @param[out] err_out When non-NULL, set to a static message on failure.
   * @return true on success.
   */
  bool e2e_amqp_telemetry_begin(
      e2e_amqp_telemetry* t,
      const char* eh_host,
      const char* entity_path,
      const char* consumer_group,
      int64_t enqueued_after_ms,
      const char* sas_token,
      int partition_count,
      const char* capture_filter,
      const char** err_out);

  /**
   * @brief Captures only the bodies that contain @p capture_filter.
   *
   * @param[in] capture_filter Substring a body must contain; `NULL` or empty captures every body.
   * @return false if @p capture_filter is too long; the filter is then unchanged.
   */
  bool e2e_amqp_telemetry_set_capture_filter(e2e_amqp_telemetry* t, const char* capture_filter);

  /**
   * @brief Counts a received data body and, if it contains the capture filter, captures it into
   * the ring, evicting the oldest capture when the ring is full.
   *
   * @return The capture, for the caller to add the properties to; NULL if the filter excludes the
   * body.
   */
  e2e_amqp_capture* e2e_amqp_telemetry_capture_body(
      e2e_amqp_telemetry* t,
      const uint8_t* body,
      size_t body_size);

  /**
   * @brief Records that @p partition delivered the event with @p sequence_number.
   *
   * @param[in] sequence_number Negative when unknown: the event is then taken as new.
   * @return false if the event was received before, i.e. replayed after a reconnect.
   */
  bool e2e_amqp_telemetry_record_sequence_number(
      e2e_amqp_telemetry* t,
      int partition,
      int64_t sequence_number);

  /**
   * @brief Advances the telemetry connection once, waiting up to @p wait_ms for socket I/O, and
   * then its setup by at most one step.
   *
   * @return false, with t->error set, if the connection failed or its setup did not finish
   * within #E2E_AMQP_SETUP_TIMEOUT_S.
   */
  bool e2e_amqp_telemetry_do_work(e2e_amqp_telemetry* t, int wait_ms);

  /** @brief The partition whose receiver the service detached (e.g. a sixth reader of the
   * partition in the consumer group), or -1 while every receiver is attached or attaching. */
  int e2e_amqp_telemetry_find_detached_receiver(const e2e_amqp_telemetry* t);

  /**
   * @brief Drops the connection, without waiting for the service, and starts setting it up again
   * with the parameters of e2e_amqp_telemetry_begin(); e2e_amqp_telemetry_do_work() completes
   * it. Captures, watched needles and received sequence numbers are kept.
   *
   * @return false, with t->error set, if the setup could not start.
   */
  bool e2e_amqp_telemetry_restart(e2e_amqp_telemetry* t);

  /* Returns true if any captured telemetry body contains @p needle. */
  bool e2e_amqp_telemetry_seen(const e2e_amqp_telemetry* t, const char* needle);

  /** @brief Registers @p needle to be latched by e2e_amqp_telemetry_watched_seen().
   * @return false when the table is full or @p needle is empty or too long. */
  bool e2e_amqp_telemetry_watch_needle(e2e_amqp_telemetry* t, const char* needle);

  /** @brief True once any body (full, not only the captured prefix) received since @p needle was
   * registered has contained it, regardless of later ring eviction. */
  bool e2e_amqp_telemetry_watched_seen(const e2e_amqp_telemetry* t, const char* needle);

  /** @brief The earliest captured message whose body contains @p needle, or NULL. */
  const e2e_amqp_capture* e2e_amqp_telemetry_find(const e2e_amqp_telemetry* t, const char* needle);

  /** @brief Copies property @p name of @p c into @p out (truncated, NUL-terminated).
   * @return false if @p c has no such property. */
  bool e2e_amqp_capture_property(
      const e2e_amqp_capture* c,
      const char* name,
      char* out,
      size_t out_size);

  /** @brief Copies into @p name_out the name of the first property of @p c whose value is
   * @p value. @return false if none. */
  bool e2e_amqp_capture_find_value(
      const e2e_amqp_capture* c,
      const char* value,
      char* name_out,
      size_t name_out_size);

  /** @brief Prints every captured property of @p c to stdout. */
  void e2e_amqp_capture_dump(const e2e_amqp_capture* c);

  /* Detach receivers and close the telemetry connection (best-effort). */
  void e2e_amqp_telemetry_end(e2e_amqp_telemetry* t);

  /* Send one cloud-to-device message to @p device_id via the IoT Hub service AMQP
   * endpoint (@p hub_host:5671), authorizing with @p sas_token (audience = hub
   * host). Blocks (pumping internally) until IoT Hub accepts the message or an
   * error/timeout occurs. Opens and tears down its own short-lived connection.
   * Returns true only when the delivery outcome is `accepted`. @p properties may
   * be NULL. */
  bool e2e_amqp_send_c2d(
      const char* hub_host,
      const char* sas_token,
      const char* device_id,
      const uint8_t* payload,
      size_t payload_len,
      const az_iot_e2e_c2d_properties* properties,
      const char** err_out);

#define E2E_AMQP_NOTIFY_CAPTURE_MAX 8
#define E2E_AMQP_NOTIFY_BODY_MAX 2048
#define E2E_AMQP_NOTIFY_HOLD_MAX 10

  /* A file-upload notification watcher: an AMQP connection to the IoT Hub service
   * endpoint with a single receiver on /messages/serviceBound/filenotifications.
   *
   * IoT Hub emits one notification per completed upload the device reported as
   * successful, so this is the only way to prove the whole file-upload round trip
   * from the CLOUD side -- the device merely sees the hub accept its notification.
   * Bodies are captured into a small ring so a test can poll for the blob name. */
  typedef struct e2e_amqp_filenotify
  {
    az_amqp_sample_transport transport_storage;
    az_amqp_transport transport;
    az_amqp_connection connection;
    az_amqp_session session;
    az_amqp_cbs cbs;
    az_amqp_link receiver;
    bool started;
    bool connection_failed;

    char captured[E2E_AMQP_NOTIFY_CAPTURE_MAX][E2E_AMQP_NOTIFY_BODY_MAX];
    int captured_count;
    char match[128]; /* only notifications containing this are consumed */

    /* Other live watchers' notifications, released after a short hold. */
    struct
    {
      uint32_t delivery_number;
      int64_t held_at_s;
    } held[E2E_AMQP_NOTIFY_HOLD_MAX];
    int held_count;

    /* Diagnostics. A notification that never arrives is indistinguishable from
     * one that arrived and was filtered out or could not be decoded, so count
     * every disposition and let a failing test report them. */
    int delivered_count; /* deliveries the endpoint handed us, whatever their shape */
    int released_count; /* released because they name another device */
    int stale_count; /* another device's, too old to have a watcher: accepted */
    int unparsed_count; /* body missing or not a DATA body */

    uint8_t incoming_buffer[AZ_AMQP_DEFAULT_MAX_FRAME_SIZE];
    uint8_t outgoing_buffer[AZ_AMQP_DEFAULT_MAX_FRAME_SIZE];
    az_amqp_session* session_slots[1];
    az_amqp_link* link_slots[3]; /* CBS pair (2) + notification receiver (1) */
    uint8_t cbs_reply_buffer[1024];
    uint8_t recv_buffer[4096];
    char audience_buffer[256];
  } e2e_amqp_filenotify;

  /* Connect to @p hub_host:5671, CBS-authorize the hub host with @p sas_token and
   * attach a receiver to the file-notification node.
   *
   * The notification node is HUB-WIDE, so several test legs can be listening at
   * once. Only notifications whose body contains @p match (typically this test's
   * device id) are captured and settled; everything else is RELEASED, after a
   * short hold, so the hub redelivers it to its rightful watcher -- except
   * notifications enqueued too long ago for any watcher to be waiting, which are
   * accepted. On failure returns false and (when non-NULL) points @p err_out at a
   * static message.
   *
   * @p attach_refused_out, when non-NULL, is set to true only when the hub
   * REFUSED the receiver link on the notification node, and false for every other
   * failure. That one case is transient -- it is what a hub reports while the
   * enableFileUploadNotifications flag is still propagating -- so a caller can
   * retry it while failing fast on out-of-memory, a bad token, or an unreachable
   * host. Distinguishing it here rather than by comparing @p err_out keeps the
   * decision from silently breaking if a message is reworded. */
  bool e2e_amqp_filenotify_begin(
      e2e_amqp_filenotify* f,
      const char* hub_host,
      const char* sas_token,
      const char* match,
      const char** err_out,
      bool* attach_refused_out);

  /* Advance the watcher once, waiting up to @p wait_ms for socket I/O. Returns
   * false if the connection has failed. */
  bool e2e_amqp_filenotify_do_work(e2e_amqp_filenotify* f, int wait_ms);

  /* Returns true if any captured notification body contains @p needle. */
  bool e2e_amqp_filenotify_seen(const e2e_amqp_filenotify* f, const char* needle);

  /* Delivery counters, for reporting why a wait timed out. */
  void e2e_amqp_filenotify_stats(
      const e2e_amqp_filenotify* f,
      int* out_delivered,
      int* out_captured,
      int* out_released,
      int* out_stale,
      int* out_unparsed);

  /* Detach the receiver and close the connection (best-effort). */
  void e2e_amqp_filenotify_end(e2e_amqp_filenotify* f);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_E2E_AMQP_H */
