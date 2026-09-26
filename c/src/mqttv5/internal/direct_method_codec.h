// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Bounded proto3 codec for the AEG direct-method messages defined in
 * common/Protos/directmethods.proto.
 *
 * Why hand-rolled rather than nanopb
 *   The six messages here are tiny and fixed-shape -- two varints, three
 *   length-delimited fields and a two-arm oneof -- so the whole wire format in
 *   use is a few hundred bytes of code. Generating it would put protoc in the
 *   path of the CMake build AND of the ESP-IDF component build, which composes
 *   its sources by listing files rather than by running a generator, and would
 *   pull in google/protobuf/timestamp.proto purely for Exec.exec_start, the one
 *   field the protocol declares observability-only and this SDK never reads.
 *   The connection client already encodes presence.proto the same way
 *   (presence_encode_birth in core/connection_client.c).
 *
 * Everything below is allocation-free and bounds-checked. Decoders produce
 * zero-copy views into the caller's buffer, valid for as long as that buffer
 * is; encoders write into a caller-provided buffer and report the length.
 *
 * Unknown fields are skipped rather than rejected, so a later protocol revision
 * that adds a field to a message this SDK already understands still decodes.
 */
#ifndef AZ_IOT_MQTTV5_DIRECT_METHOD_CODEC_H
#define AZ_IOT_MQTTV5_DIRECT_METHOD_CODEC_H

#include <stddef.h>
#include <stdint.h>

#include "azure/iot/az_iot_result.h"

#ifdef __cplusplus
extern "C"
{
#endif

/* Exact wire size of every message this SDK emits, so callers can size a
 * buffer without guessing. ProbeAck{ready} and Abandon both carry one 16-byte
 * id; Result carries a status and a caller-sized body. */
#define AZ_IOT_DM_PROTO_READY_ID_LEN 16u
#define AZ_IOT_DM_PROTO_PROBE_ACK_MAX 20u
#define AZ_IOT_DM_PROTO_ABANDON_MAX 20u
/* Tag + 10-byte int32 varint, then tag + 5-byte length varint. */
#define AZ_IOT_DM_PROTO_RESULT_OVERHEAD 17u

  /* directmethods.proto RejectedReason. */
  typedef enum
  {
    AZ_IOT_DM_PROTO_REJECTED_UNSPECIFIED = 0,
    AZ_IOT_DM_PROTO_REJECTED_METHOD_NOT_FOUND = 1,
    AZ_IOT_DM_PROTO_REJECTED_INSUFFICIENT_TIME = 2,
    AZ_IOT_DM_PROTO_REJECTED_DEVICE_BUSY = 3
  } az_iot_dm_proto_rejected_reason;

  /* directmethods.proto AbandonReason. The .proto spells value 2
   * INSUFFICIENT_TIME_DUP behind a TODO because the name collides with
   * RejectedReason's; the wire value is what matters and it is 2 either way. */
  typedef enum
  {
    AZ_IOT_DM_PROTO_ABANDON_UNSPECIFIED = 0,
    AZ_IOT_DM_PROTO_ABANDON_READY_WAIT_TIMEOUT = 1,
    AZ_IOT_DM_PROTO_ABANDON_INSUFFICIENT_TIME = 2
  } az_iot_dm_proto_abandon_reason;

  /* Decoded Probe. `method_name` points into the decoded buffer and is NOT
   * NUL-terminated; proto3 omits a default-valued field, so an absent
   * method_name arrives as a zero length rather than as an error. */
  typedef struct az_iot_dm_proto_probe
  {
    const char* method_name;
    size_t method_name_len;
    uint32_t response_timeout_seconds;
  } az_iot_dm_proto_probe;

  /* Decoded Exec. Both spans point into the decoded buffer. exec_start is
   * skipped: the protocol declares it observability-only and forbids using it
   * for deadline computation, so decoding it would only invite misuse. */
  typedef struct az_iot_dm_proto_exec
  {
    const uint8_t* ready_id;
    size_t ready_id_len;
    const uint8_t* params;
    size_t params_len;
  } az_iot_dm_proto_exec;

  /* Returns AZ_IOT_ERR_PROTOCOL on a truncated, malformed or group-encoded
   * message, AZ_IOT_ERR_INVALID_ARG on a NULL out-parameter. A zero-length
   * payload is a valid proto3 message with every field defaulted. */
  AZ_NODISCARD az_iot_result az_iot_dm_proto_decode_probe(
      const uint8_t* buffer,
      size_t length,
      az_iot_dm_proto_probe* out_probe);

  AZ_NODISCARD az_iot_result
  az_iot_dm_proto_decode_exec(const uint8_t* buffer, size_t length, az_iot_dm_proto_exec* out_exec);

  /* Encode ProbeAck{ready: Ready{ready_id}}. Needs
   * AZ_IOT_DM_PROTO_PROBE_ACK_MAX bytes. */
  AZ_NODISCARD az_iot_result az_iot_dm_proto_encode_probe_ack_ready(
      uint8_t* buffer,
      size_t capacity,
      const uint8_t* ready_id,
      size_t ready_id_len,
      size_t* out_length);

  /* Encode ProbeAck{rejected: Rejected{reason}}. The `rejected` arm is emitted
   * even for the default reason, because a oneof case is what tells the service
   * this is a rejection rather than an empty message. */
  AZ_NODISCARD az_iot_result az_iot_dm_proto_encode_probe_ack_rejected(
      uint8_t* buffer,
      size_t capacity,
      az_iot_dm_proto_rejected_reason reason,
      size_t* out_length);

  AZ_NODISCARD az_iot_result az_iot_dm_proto_encode_abandon(
      uint8_t* buffer,
      size_t capacity,
      const uint8_t* ready_id,
      size_t ready_id_len,
      az_iot_dm_proto_abandon_reason reason,
      size_t* out_length);

  /* Encode Result{status, body}. Needs body_len +
   * AZ_IOT_DM_PROTO_RESULT_OVERHEAD bytes in the worst case; a negative status
   * is sign-extended to ten varint bytes, as proto3 requires for int32. */
  AZ_NODISCARD az_iot_result az_iot_dm_proto_encode_result(
      uint8_t* buffer,
      size_t capacity,
      int32_t status,
      const uint8_t* body,
      size_t body_len,
      size_t* out_length);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_MQTTV5_DIRECT_METHOD_CODEC_H */
