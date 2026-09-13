// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
#ifndef AZ_IOT_SAS_TOKEN_H
#define AZ_IOT_SAS_TOKEN_H

#include <stddef.h>
#include <stdint.h>

#include "azure/iot/az_iot_connection_client.h"
#include "azure/iot/az_iot_result.h"

#ifdef __cplusplus
extern "C"
{
#endif

  /*
   * Shared access signature tokens.
   *
   * Split out from the connection client because the format is exact and
   * failure is silent: a wrong audience, a missing URL-escape or the wrong
   * `skn` does not produce a diagnosable error, it produces a service that
   * simply refuses the connection. Pure functions here can be pinned against
   * the measured wire format.
   *
   * The device key never appears in this file. The caller supplies a signing
   * callback; what is assembled here is the string to sign and the token around
   * the result.
   */

  /* The audience a token is minted for. The two legs differ in both the
   * resource shape and whether `skn` is present, and the difference is not
   * cosmetic -- a DPS-scoped token is rejected by a hub and vice versa. */
  typedef enum
  {
    /* "{idScope}/registrations/{registrationId}", with skn=registration. */
    AZ_IOT_SAS_AUDIENCE_PROVISIONING = 0,
    /* "{host}/devices/{deviceId}", with no skn. */
    AZ_IOT_SAS_AUDIENCE_HUB
  } az_iot_sas_audience;

  /* Build the resource URI a token is scoped to (unescaped).
   *
   * @param first   idScope for PROVISIONING, hub host for HUB.
   * @param second  registrationId for PROVISIONING, deviceId for HUB.
   * @return AZ_IOT_ERR_NOT_ENOUGH_SPACE when the buffer is too small.
   */
  az_iot_result az_iot_sas__build_resource(
      az_iot_sas_audience audience,
      const char* first,
      const char* second,
      char* out,
      size_t out_size,
      size_t* out_len);

  /* Percent-encode per RFC 3986, escaping everything outside the unreserved
   * set. Both `sr` and `sig` travel escaped; a '/' left raw in the resource is
   * the single most common way to get a silently rejected token. */
  az_iot_result az_iot_sas__url_encode(
      const char* in,
      size_t in_len,
      char* out,
      size_t out_size,
      size_t* out_len);

  /* Assemble the string the caller must HMAC-SHA256: the escaped resource, a
   * newline, and the expiry as decimal seconds. */
  az_iot_result az_iot_sas__build_string_to_sign(
      const char* escaped_resource,
      uint64_t expiry_unix_s,
      char* out,
      size_t out_size,
      size_t* out_len);

  /* Assemble the finished header value from a raw (not base64) MAC:
   *
   *   SharedAccessSignature sr={resource}&sig={sig}&se={expiry}[&skn=registration]
   *
   * `skn=registration` is appended for PROVISIONING only. */
  az_iot_result az_iot_sas__build_token(
      az_iot_sas_audience audience,
      const char* escaped_resource,
      const uint8_t* signature,
      size_t signature_len,
      uint64_t expiry_unix_s,
      char* out,
      size_t out_size,
      size_t* out_len);

  /* The whole sequence: resource -> escape -> string-to-sign -> sign -> token.
   * The one entry point the connection client uses. */
  az_iot_result az_iot_sas__mint(
      az_iot_sas_audience audience,
      const char* first,
      const char* second,
      uint64_t expiry_unix_s,
      az_iot_sas_sign_callback sign,
      void* sign_ctx,
      char* out,
      size_t out_size,
      size_t* out_len);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_SAS_TOKEN_H */
