// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Minimal proto3 wire-format reader/writer.
 *
 * The AEG/Hub-Next protocols (presence Birth/BirthAck, twin TwinPush,
 * DesiredPatch, TwinGet, ...) carry protobuf payloads. The SDK does not link a
 * protobuf runtime: the messages are small and flat, so they are encoded and
 * decoded field by field with the helpers below. Everything is bounds-checked
 * and allocation-free; the caller supplies the buffer.
 *
 * Reader convention: every function takes the buffer plus a cursor and returns
 * false on a truncated or malformed encoding, leaving the cursor unspecified.
 * A false return means "stop parsing this message" -- callers keep whatever
 * fields they decoded before the failure, which matches proto3's tolerance for
 * partial/unknown content.
 *
 * Writer convention: every function returns false if the value does not fit in
 * the remaining capacity, and leaves the cursor unchanged so the caller can
 * report a buffer-too-small error without having emitted a partial field.
 */
#ifndef AZ_IOT_PROTO3_INTERNAL_H
#define AZ_IOT_PROTO3_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* proto3 wire types (the low 3 bits of a field key). */
#define AZ_IOT_PROTO3_WIRE_VARINT 0
#define AZ_IOT_PROTO3_WIRE_64BIT 1
#define AZ_IOT_PROTO3_WIRE_LEN 2
#define AZ_IOT_PROTO3_WIRE_32BIT 5

/* A varint carries seven payload bits per byte. Bits 0..62 come from nine such
 * groups and bit 63 from a tenth, so MAX_SHIFT is the last shift a legal uint64
 * can reach and TOP_BIT_MAX is the largest payload that tenth byte may carry. */
/* protobuf caps field numbers at 2^29-1; 0 is not a valid field number. */
#define AZ_IOT_PROTO3_MAX_FIELD_NUMBER 536870911u

/** Most bytes a varint can take: a uint64 needs ten 7-bit groups. */
#define AZ_IOT_PROTO3_VARINT_MAX_BYTES 10u

#define AZ_IOT_PROTO3_VARINT_MAX_SHIFT 63u
#define AZ_IOT_PROTO3_VARINT_TOP_BIT_MAX 1u

/* ---- reader ------------------------------------------------------------- */

/* Read a base-128 varint. Rejects encodings longer than 10 bytes. */
bool az_iot_proto3_read_varint(const uint8_t* buf, size_t len, size_t* pos, uint64_t* out_value);

/* Read a field key and split it into field number and wire type. Rejects a
 * field number outside protobuf's 1..2^29-1: such a key means the frame is
 * malformed, not that the field is one this SDK does not know, so it must not
 * be skipped as unknown. */
bool az_iot_proto3_read_tag(
    const uint8_t* buf,
    size_t len,
    size_t* pos,
    uint32_t* out_field,
    uint8_t* out_wire);

/* Read a length-delimited field, returning a pointer into `buf` (no copy).
 * `out_bytes` is non-NULL even when the field is empty, so a present-but-empty
 * field stays distinguishable from an absent one. */
bool az_iot_proto3_read_bytes(
    const uint8_t* buf,
    size_t len,
    size_t* pos,
    const uint8_t** out_bytes,
    size_t* out_len);

/* Advance past a field of `wire` type whose key has already been read. Used to
 * ignore fields the SDK does not know, so a later service-side schema revision
 * does not break decoding. Returns false for group wire types (3/4, removed in
 * proto3) and anything else unrecognized. */
bool az_iot_proto3_skip_field(const uint8_t* buf, size_t len, size_t* pos, uint8_t wire);

/* ---- writer ------------------------------------------------------------- */

/* Append a field key. */
bool az_iot_proto3_write_tag(uint8_t* buf, size_t cap, size_t* pos, uint32_t field, uint8_t wire);

/* Append a varint-typed field. proto3 omits default-valued fields, so callers
 * generally skip the call entirely when `value` is 0. */
bool az_iot_proto3_write_varint_field(
    uint8_t* buf,
    size_t cap,
    size_t* pos,
    uint32_t field,
    uint64_t value);

/* Append a length-delimited field. A zero-length field is still emitted, which
 * is how an explicitly-present-but-empty `optional bytes` is encoded. */
bool az_iot_proto3_write_bytes_field(
    uint8_t* buf,
    size_t cap,
    size_t* pos,
    uint32_t field,
    const uint8_t* bytes,
    size_t len);

#endif /* AZ_IOT_PROTO3_INTERNAL_H */
