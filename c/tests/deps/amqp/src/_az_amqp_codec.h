// Copyright (c) Microsoft Corporation. All rights reserved.
// SPDX-License-Identifier: MIT

/**
 * @file
 *
 * @brief Internal: AMQP 1.0 wire constants (format codes, descriptors, frame layout) and
 * big-endian read/write helpers shared by the implementation. Not part of the public API.
 */

#ifndef _az_AMQP_CODEC_INTERNAL_H
#define _az_AMQP_CODEC_INTERNAL_H

#include <stdint.h>
#include <string.h>

// ----- Primitive format codes (OASIS AMQP 1.0 §1.6) -----
#define _AZ_AMQP_FC_NULL 0x40
#define _AZ_AMQP_FC_BOOL 0x56
#define _AZ_AMQP_FC_TRUE 0x41
#define _AZ_AMQP_FC_FALSE 0x42
#define _AZ_AMQP_FC_UBYTE 0x50
#define _AZ_AMQP_FC_USHORT 0x60
#define _AZ_AMQP_FC_UINT0 0x43
#define _AZ_AMQP_FC_SMALLUINT 0x52
#define _AZ_AMQP_FC_UINT 0x70
#define _AZ_AMQP_FC_ULONG0 0x44
#define _AZ_AMQP_FC_SMALLULONG 0x53
#define _AZ_AMQP_FC_ULONG 0x80
#define _AZ_AMQP_FC_BYTE 0x51
#define _AZ_AMQP_FC_SHORT 0x61
#define _AZ_AMQP_FC_SMALLINT 0x54
#define _AZ_AMQP_FC_INT 0x71
#define _AZ_AMQP_FC_SMALLLONG 0x55
#define _AZ_AMQP_FC_LONG 0x81
#define _AZ_AMQP_FC_FLOAT 0x72
#define _AZ_AMQP_FC_DOUBLE 0x82
#define _AZ_AMQP_FC_CHAR 0x73
#define _AZ_AMQP_FC_TIMESTAMP 0x83
#define _AZ_AMQP_FC_UUID 0x98
#define _AZ_AMQP_FC_VBIN8 0xa0
#define _AZ_AMQP_FC_VBIN32 0xb0
#define _AZ_AMQP_FC_STR8 0xa1
#define _AZ_AMQP_FC_STR32 0xb1
#define _AZ_AMQP_FC_SYM8 0xa3
#define _AZ_AMQP_FC_SYM32 0xb3
#define _AZ_AMQP_FC_LIST0 0x45
#define _AZ_AMQP_FC_LIST8 0xc0
#define _AZ_AMQP_FC_LIST32 0xd0
#define _AZ_AMQP_FC_MAP8 0xc1
#define _AZ_AMQP_FC_MAP32 0xd1
#define _AZ_AMQP_FC_ARRAY8 0xe0
#define _AZ_AMQP_FC_ARRAY32 0xf0
#define _AZ_AMQP_FC_DESCRIBED 0x00

// ----- Performative descriptors (§2.7) -----
#define _AZ_AMQP_DESC_OPEN 0x10
#define _AZ_AMQP_DESC_BEGIN 0x11
#define _AZ_AMQP_DESC_ATTACH 0x12
#define _AZ_AMQP_DESC_FLOW 0x13
#define _AZ_AMQP_DESC_TRANSFER 0x14
#define _AZ_AMQP_DESC_DISPOSITION 0x15
#define _AZ_AMQP_DESC_DETACH 0x16
#define _AZ_AMQP_DESC_END 0x17
#define _AZ_AMQP_DESC_CLOSE 0x18
#define _AZ_AMQP_DESC_ERROR 0x1d

// ----- Terminus + delivery-state descriptors (§3.4, §3.5) -----
#define _AZ_AMQP_DESC_SOURCE 0x28
#define _AZ_AMQP_DESC_TARGET 0x29
#define _AZ_AMQP_DESC_RECEIVED 0x23
#define _AZ_AMQP_DESC_ACCEPTED 0x24
#define _AZ_AMQP_DESC_REJECTED 0x25
#define _AZ_AMQP_DESC_RELEASED 0x26
#define _AZ_AMQP_DESC_MODIFIED 0x27

// ----- Message section descriptors (§3.2) -----
#define _AZ_AMQP_DESC_HEADER 0x70
#define _AZ_AMQP_DESC_DELIVERY_ANNOTATIONS 0x71
#define _AZ_AMQP_DESC_MESSAGE_ANNOTATIONS 0x72
#define _AZ_AMQP_DESC_PROPERTIES 0x73
#define _AZ_AMQP_DESC_APPLICATION_PROPERTIES 0x74
#define _AZ_AMQP_DESC_DATA 0x75
#define _AZ_AMQP_DESC_AMQP_SEQUENCE 0x76
#define _AZ_AMQP_DESC_AMQP_VALUE 0x77
#define _AZ_AMQP_DESC_FOOTER 0x78

// ----- SASL performative descriptors (§5.3) -----
#define _AZ_AMQP_DESC_SASL_MECHANISMS 0x40
#define _AZ_AMQP_DESC_SASL_INIT 0x41
#define _AZ_AMQP_DESC_SASL_CHALLENGE 0x42
#define _AZ_AMQP_DESC_SASL_RESPONSE 0x43
#define _AZ_AMQP_DESC_SASL_OUTCOME 0x44

// ----- Frame layout (§2.3) -----
#define _AZ_AMQP_FRAME_TYPE_AMQP 0x00
#define _AZ_AMQP_FRAME_TYPE_SASL 0x01
#define _AZ_AMQP_FRAME_HEADER_SIZE 8
#define _AZ_AMQP_FRAME_DOFF_MIN 2 // 2 * 4 = 8 bytes

// Protocol headers (8 bytes): "AMQP" + protocol-id + major.minor.revision.
#define _AZ_AMQP_PROTOCOL_ID_AMQP 0
#define _AZ_AMQP_PROTOCOL_ID_SASL 3

// ----- Big-endian read/write helpers -----

static inline void _az_amqp_write_u16_be(uint8_t* p, uint16_t v)
{
  p[0] = (uint8_t)(v >> 8);
  p[1] = (uint8_t)(v);
}

static inline void _az_amqp_write_u32_be(uint8_t* p, uint32_t v)
{
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)(v);
}

static inline void _az_amqp_write_u64_be(uint8_t* p, uint64_t v)
{
  p[0] = (uint8_t)(v >> 56);
  p[1] = (uint8_t)(v >> 48);
  p[2] = (uint8_t)(v >> 40);
  p[3] = (uint8_t)(v >> 32);
  p[4] = (uint8_t)(v >> 24);
  p[5] = (uint8_t)(v >> 16);
  p[6] = (uint8_t)(v >> 8);
  p[7] = (uint8_t)(v);
}

static inline uint16_t _az_amqp_read_u16_be(uint8_t const* p)
{
  return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static inline uint32_t _az_amqp_read_u32_be(uint8_t const* p)
{
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static inline uint64_t _az_amqp_read_u64_be(uint8_t const* p)
{
  return ((uint64_t)p[0] << 56) | ((uint64_t)p[1] << 48) | ((uint64_t)p[2] << 40)
      | ((uint64_t)p[3] << 32) | ((uint64_t)p[4] << 24) | ((uint64_t)p[5] << 16)
      | ((uint64_t)p[6] << 8) | (uint64_t)p[7];
}

#endif // _az_AMQP_CODEC_INTERNAL_H
