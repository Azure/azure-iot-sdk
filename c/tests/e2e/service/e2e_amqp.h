// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Internal helper for the e2e service client: the AMQP 1.0 paths (telemetry
 * receive from the Event Hub-compatible endpoint, and cloud-to-device send to
 * the IoT Hub service endpoint), built on the vendored `az_amqp` library and
 * its sample reference transport.
 *
 * This header is INTERNAL to the az_iot_e2e_service static library. Test
 * translation units never include it, so the `az_amqp` dependency stays behind
 * the service-client facade.
 */
#ifndef AZ_IOT_E2E_AMQP_H
#define AZ_IOT_E2E_AMQP_H

#include <azure/az_amqp.h>

#include "az_amqp_sample_transport.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define E2E_AMQP_MAX_PARTITIONS 32
#define E2E_AMQP_CAPTURE_MAX 64
#define E2E_AMQP_CAPTURE_BODY_MAX 1024

  /* A telemetry watcher: an AMQP connection to the IoT Hub Event Hub-compatible
   * endpoint with one receiver link per partition. Received message bodies are
   * captured into a ring (newest overwrite oldest) so the test can poll for a
   * correlation marker, even on a hub other runs are sending to.
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
    bool started;
    bool connection_failed;

    /* Capture ring of recently received bodies (NUL-terminated for substring search). */
    char captured[E2E_AMQP_CAPTURE_MAX][E2E_AMQP_CAPTURE_BODY_MAX];
    int captured_count;
    int captured_next; /* ring slot the next body is written to */

    /* Backing storage referenced by the az_amqp objects above. */
    uint8_t incoming_buffer[AZ_AMQP_DEFAULT_MAX_FRAME_SIZE];
    uint8_t outgoing_buffer[AZ_AMQP_DEFAULT_MAX_FRAME_SIZE];
    az_amqp_session* session_slots[1];
    az_amqp_link* link_slots[E2E_AMQP_MAX_PARTITIONS + 2];
    uint8_t cbs_reply_buffer[1024];
    uint8_t recv_buffers[E2E_AMQP_MAX_PARTITIONS][2048];
    char source_addr[E2E_AMQP_MAX_PARTITIONS][256];
    char link_name[E2E_AMQP_MAX_PARTITIONS][32];
    char audience_buffer[256];
    uint8_t filter_buffer[256];
  } e2e_amqp_telemetry;

  /**
   * @brief Connects to @p eh_host:5671, CBS-authorizes @p sas_token against the entity and
   * attaches one receiver per partition.
   *
   * @param[in] consumer_group Consumer group to read from; `NULL` or empty selects `$Default`.
   * @param[in] enqueued_after_ms Only messages enqueued after this Unix time (ms) are delivered;
   * 0 reads from the start of the stream.
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
      const char** err_out);

  /* Advance the telemetry connection once, waiting up to @p wait_ms for socket I/O.
   * Returns false if the connection has failed. */
  bool e2e_amqp_telemetry_do_work(e2e_amqp_telemetry* t, int wait_ms);

  /* Returns true if any captured telemetry body contains @p needle. */
  bool e2e_amqp_telemetry_seen(const e2e_amqp_telemetry* t, const char* needle);

  /* Detach receivers and close the telemetry connection (best-effort). */
  void e2e_amqp_telemetry_end(e2e_amqp_telemetry* t);

  /* Send one cloud-to-device message to @p device_id via the IoT Hub service AMQP
   * endpoint (@p hub_host:5671), authorizing with @p sas_token (audience = hub
   * host). Blocks (pumping internally) until IoT Hub accepts the message or an
   * error/timeout occurs. Opens and tears down its own short-lived connection.
   * Returns true only when the delivery outcome is `accepted`. */
  bool e2e_amqp_send_c2d(
      const char* hub_host,
      const char* sas_token,
      const char* device_id,
      const uint8_t* payload,
      size_t payload_len,
      const char** err_out);

#define E2E_AMQP_NOTIFY_CAPTURE_MAX 8
#define E2E_AMQP_NOTIFY_BODY_MAX 2048

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

    /* Diagnostics. A notification that never arrives is indistinguishable from
     * one that arrived and was filtered out or could not be decoded, so count
     * every disposition and let a failing test report them. */
    int delivered_count; /* deliveries the endpoint handed us, whatever their shape */
    int released_count; /* released because they name another device */
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
   * device id) are captured and settled; everything else is RELEASED so the hub
   * redelivers it to its rightful watcher. On failure returns false and (when
   * non-NULL) points @p err_out at a static message.
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
      int* out_unparsed);

  /* Detach the receiver and close the connection (best-effort). */
  void e2e_amqp_filenotify_end(e2e_amqp_filenotify* f);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_E2E_AMQP_H */
