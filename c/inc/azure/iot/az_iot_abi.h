// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/**
 * @file az_iot_abi.h
 * @brief ABI profile, fingerprint and struct-versioning helpers.
 *
 * Contract: docs/struct_versioning.md.
 */
#ifndef AZ_IOT_ABI_H
#define AZ_IOT_ABI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/** @brief ABI revision. Bumped on any change a prebuilt application cannot absorb. */
#define AZ_IOT_ABI_VERSION 1

/**
 * @brief 1 for the SHARED (updatable library) profile, 0 for EMBEDDED.
 *
 * Set by the build (CMake `AZ_IOT_ABI_PROFILE`) and propagated to consumers of
 * `az_iot_core`. Builds that bypass CMake get EMBEDDED.
 */
#ifndef AZ_IOT_ABI_SHARED
#define AZ_IOT_ABI_SHARED 0
#endif

/**
 * @brief True when the SDK-stamped struct @p p of type @p T carries @p field.
 *
 * Compare against the last field actually read, not `sizeof(T)`, so a struct
 * stamped by an older library is still usable.
 */
#define AZ_IOT_STRUCT_HAS_FIELD(p, T, field) \
  ((size_t)(p)->_internal_size >= offsetof(T, field) + sizeof((p)->field))

/** @brief Folds @p v into fingerprint @p h (32-bit FNV-1a step). */
#define AZ_IOT_ABI_MIX(h, v) ((uint32_t)(((uint32_t)(h) ^ (uint32_t)(v)) * 16777619u))

/**
 * @brief Build-invariant ABI fingerprint: ABI version, profile and pointer size.
 *
 * Client-specific fingerprints extend this with the size macros that shape
 * their storage.
 */
#define AZ_IOT_ABI_FINGERPRINT                                                            \
  AZ_IOT_ABI_MIX(                                                                         \
      AZ_IOT_ABI_MIX(AZ_IOT_ABI_MIX(2166136261u, AZ_IOT_ABI_VERSION), AZ_IOT_ABI_SHARED), \
      sizeof(void*))

/**
 * @name Client storage reserves (bytes)
 * Spare storage appended to each caller-allocated client in the SHARED profile,
 * so an updated library may grow its internal state without a rebuild of the
 * application. 0 in the EMBEDDED profile. Provisional: re-baselined right
 * before the first GA release.
 * @{
 */
#if AZ_IOT_ABI_SHARED
#define AZ_IOT_CONNECTION_CLIENT_RESERVE 768
#define AZ_IOT_SU_CLIENT_RESERVE 1792
#define AZ_IOT_MQTTV5_DIRECT_METHOD_CLIENT_RESERVE 512
#define AZ_IOT_MQTTV3_DIRECT_METHOD_CLIENT_RESERVE 192
#define AZ_IOT_MQTTV5_TWIN_CLIENT_RESERVE 256
#define AZ_IOT_MQTTV3_TWIN_CLIENT_RESERVE 128
#define AZ_IOT_MQTTV3_FILE_UPLOAD_CLIENT_RESERVE 128
#define AZ_IOT_MQTTV3_TELEMETRY_CLIENT_RESERVE 64
#define AZ_IOT_MQTTV5_TELEMETRY_CLIENT_RESERVE 64
#define AZ_IOT_MQTTV3_C2D_CLIENT_RESERVE 64
#define AZ_IOT_CERTIFICATE_PROVIDER_PEM_RESERVE 64
#else
#define AZ_IOT_CONNECTION_CLIENT_RESERVE 0
#define AZ_IOT_SU_CLIENT_RESERVE 0
#define AZ_IOT_MQTTV5_DIRECT_METHOD_CLIENT_RESERVE 0
#define AZ_IOT_MQTTV3_DIRECT_METHOD_CLIENT_RESERVE 0
#define AZ_IOT_MQTTV5_TWIN_CLIENT_RESERVE 0
#define AZ_IOT_MQTTV3_TWIN_CLIENT_RESERVE 0
#define AZ_IOT_MQTTV3_FILE_UPLOAD_CLIENT_RESERVE 0
#define AZ_IOT_MQTTV3_TELEMETRY_CLIENT_RESERVE 0
#define AZ_IOT_MQTTV5_TELEMETRY_CLIENT_RESERVE 0
#define AZ_IOT_MQTTV3_C2D_CLIENT_RESERVE 0
#define AZ_IOT_CERTIFICATE_PROVIDER_PEM_RESERVE 0
#endif
  /** @} */

  /**
   * @brief AZ_IOT_ABI_FINGERPRINT as compiled into the library.
   *
   * Differs from the header's value when the application and the library were
   * built for different ABI versions, profiles or pointer sizes.
   */
  uint32_t az_iot_abi_fingerprint(void);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_ABI_H */
