// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */

/**
 * @file az_iot_dps_tpm_client.h
 * @brief DPS registration with TPM attestation, over HTTPS.
 *
 * DPS accepts TPM attestation over HTTPS and AMQP, not MQTT, so this is not
 * part of az_iot_connection_client (whose DPS session is MQTT). The client
 * builds each HTTPS request and consumes each response; the application sends
 * them with its HTTPS stack and runs the TPM through az_iot_dps_tpm_ops. It
 * does no I/O and no allocation.
 *
 * Flow:
 * 1. `PUT /{idScope}/registrations/{registrationId}/register` with the
 *    endorsement and storage root keys, no Authorization. DPS answers 401 with
 *    an encrypted key, which az_iot_dps_tpm_ops::import_key imports.
 * 2. The same PUT with a SAS token signed by az_iot_dps_tpm_ops::hmac_sha256.
 * 3. On 202, `GET .../operations/{operationId}` until DPS assigns. The
 *    assignment carries the hub key, also imported with import_key.
 * 4. The application connects az_iot_connection_client directly to the
 *    assigned hub (az_iot_dps_tpm_assignment) and signs hub SAS tokens with the
 *    TPM key from az_iot_auth::sas::on_sas_token_required.
 *
 * Send each request to the DPS endpoint (`global.azure-devices-provisioning.net`
 * unless the enrollment says otherwise) with headers `Accept: application/json`,
 * `Content-Type:` AZ_IOT_DPS_TPM_CONTENT_TYPE when there is a body, and
 * `Authorization:` az_iot_dps_tpm_request::authorization when it is not NULL.
 */

#ifndef AZ_IOT_DPS_TPM_CLIENT_H
#define AZ_IOT_DPS_TPM_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <azure/core/az_span.h>

#include "az_iot_connection_client.h"
#include "az_iot_crypto.h"
#include "az_iot_result.h"

#ifdef __cplusplus
extern "C"
{
#endif

/** @brief DPS service API version used when the options set none. */
#define AZ_IOT_DPS_TPM_DEFAULT_API_VERSION "2026-11-02-preview"

/** @brief Content-Type of a request body. */
#define AZ_IOT_DPS_TPM_CONTENT_TYPE "application/json; charset=utf-8"

/** @brief Default az_iot_dps_tpm_client_options::sas_token_lifetime_seconds. */
#define AZ_IOT_DPS_TPM_DEFAULT_SAS_TOKEN_LIFETIME_SECONDS 3600u

/** @brief Wait before the next request when DPS sends no Retry-After. */
#define AZ_IOT_DPS_TPM_DEFAULT_RETRY_AFTER_SECONDS 3u

/** @brief Longest ID scope or registration ID, in characters. */
#define AZ_IOT_DPS_TPM_ID_MAX 128

/** @brief Longest assigned hub host name, in characters. */
#define AZ_IOT_DPS_TPM_HOST_MAX 255

/** @brief Longest operation ID or key name, in characters. */
#define AZ_IOT_DPS_TPM_TOKEN_MAX 127

/**
 * @brief Bytes of az_iot_dps_tpm_registration_id_from_ek() output, terminator
 * included: unpadded base32 of a SHA-256 digest.
 */
#define AZ_IOT_DPS_TPM_REGISTRATION_ID_SIZE 53

/**
 * @brief Request buffer for an RSA 2048 EK and SRK without CSR or payload, with
 * margin. Add the CSR and payload lengths when they are set.
 */
#define AZ_IOT_DPS_TPM_REQUEST_BUFFER_SIZE 2048

  /**
   * @brief TPM operations, implemented with the application's TSS. Each
   * returns AZ_IOT_OK on success. An import_key error ends the registration
   * and is returned by az_iot_dps_tpm_client_handle_response(). An
   * hmac_sha256 error is returned by az_iot_dps_tpm_client_get_request() and
   * leaves the request to be built again.
   */
  typedef struct az_iot_dps_tpm_ops
  {
    /**
     * @brief Imports a key DPS encrypted to the TPM, replacing any earlier one.
     *
     * The TPM runs TPM2_ActivateCredential (EK, SRK), TPM2_Import and
     * TPM2_Load under the SRK, then keeps the key for hmac_sha256 (typically
     * persisted with TPM2_EvictControl). az_iot_dps_tpm_parse_key() splits
     * @p key into those commands' inputs.
     *
     * @param[in] ctx     az_iot_dps_tpm_ops::ctx.
     * @param[in] key     Decoded `authenticationKey`.
     * @param[in] key_len Bytes in @p key.
     */
    az_iot_result (*import_key)(void* ctx, const uint8_t* key, size_t key_len);

    /**
     * @brief HMAC-SHA256 of @p data with the imported key (TPM2_HMAC).
     *
     * @param[in]  ctx      az_iot_dps_tpm_ops::ctx.
     * @param[in]  data     Data to sign.
     * @param[in]  data_len Bytes in @p data.
     * @param[out] out      MAC.
     */
    az_iot_result (*hmac_sha256)(
        void* ctx,
        const uint8_t* data,
        size_t data_len,
        uint8_t out[AZ_IOT_SHA256_SIZE]);

    void* ctx; /**< Passed to each operation. */
  } az_iot_dps_tpm_ops;

  /** @brief Options for az_iot_dps_tpm_client_init(). Strings and buffers are
   * caller-owned and must outlive the client. */
  typedef struct az_iot_dps_tpm_client_options
  {
    const char* id_scope; /**< DPS ID scope. Required. */
    /** Registration ID of the enrollment. Required; `A-Z a-z 0-9 - . _ :`.
     * See az_iot_dps_tpm_registration_id_from_ek(). */
    const char* registration_id;
    /** Endorsement key public area, marshalled TPM2B_PUBLIC. Required. */
    const uint8_t* endorsement_key;
    size_t endorsement_key_len; /**< Bytes in endorsement_key. */
    /** Storage root key public area, marshalled TPM2B_PUBLIC. Required. */
    const uint8_t* storage_root_key;
    size_t storage_root_key_len; /**< Bytes in storage_root_key. */
    /** JSON object sent as the registration `payload`. NULL: none. */
    const char* payload;
    /** Base64 DER certificate signing request sent as `csr`, for a hub that
     * takes X.509 only (mqttv5). NULL: none. Needs DPS support for certificate
     * issuance under TPM attestation. */
    const char* csr;
    /** NULL selects AZ_IOT_DPS_TPM_DEFAULT_API_VERSION. */
    const char* api_version;
    /** 0 selects AZ_IOT_DPS_TPM_DEFAULT_SAS_TOKEN_LIFETIME_SECONDS. */
    uint32_t sas_token_lifetime_seconds;
    az_iot_dps_tpm_ops tpm; /**< Required: import_key and hmac_sha256. */
  } az_iot_dps_tpm_client_options;

  /** @brief A request to send. Points into the buffer given to
   * az_iot_dps_tpm_client_get_request(). */
  typedef struct az_iot_dps_tpm_request
  {
    const char* method; /**< "PUT" or "GET". */
    const char* path; /**< Path and query, starting with '/'. */
    const char* authorization; /**< Authorization header value; NULL: none. */
    const char* body; /**< JSON body; NULL for GET. */
    size_t body_len; /**< Bytes in body. */
  } az_iot_dps_tpm_request;

  /** @brief What to do after a response. */
  typedef struct az_iot_dps_tpm_next
  {
    /** True: registration is done; see az_iot_dps_tpm_client_get_assignment(). */
    bool assigned;
    /** When not assigned: seconds to wait before the next request. */
    uint32_t retry_after_seconds;
  } az_iot_dps_tpm_next;

  /** @brief The registration result. Strings live in the client. */
  typedef struct az_iot_dps_tpm_assignment
  {
    const char* assigned_hub; /**< Hub host name: az_iot_connection_client_options::host. */
    const char* device_id; /**< Device ID: az_iot_connection_client_options::client_id. */
    /** az_iot_connection_client_options::connection_profile. Absent or
     * `classic`: MQTT_V3; `mqttV5`: MQTT_V5; other: UNKNOWN. */
    az_iot_connection_profile connection_profile;
    /** True when DPS sent a hub key and import_key took it. */
    bool hub_key_imported;
    /** `registrationState.issuedCertificateChain` JSON array, a slice of the
     * last response body; empty when absent. */
    az_span issued_certificate_chain;
  } az_iot_dps_tpm_assignment;

  /** @brief Inputs of TPM2_ActivateCredential and TPM2_Import in a decoded
   * `authenticationKey`. Each is a slice of the key, marshalled TPM2B with its
   * size prefix. */
  typedef struct az_iot_dps_tpm_key_parts
  {
    az_span credential_blob; /**< TPM2B_ID_OBJECT: ActivateCredential credentialBlob. */
    az_span secret; /**< TPM2B_ENCRYPTED_SECRET: ActivateCredential secret. */
    az_span duplicate; /**< TPM2B_PRIVATE: Import duplicate. */
    az_span in_sym_seed; /**< TPM2B_ENCRYPTED_SECRET: Import inSymSeed. */
    az_span object_public; /**< TPM2B_PUBLIC: Import objectPublic. */
    /** UINT16-sized encrypted data that may follow; empty when absent. Bytes
     * after it are ignored, as later fields may be added. */
    az_span encrypted_data;
  } az_iot_dps_tpm_key_parts;

  /** @brief Client state. Caller-allocated; fields are internal. */
  typedef struct az_iot_dps_tpm_client
  {
    struct
    {
      az_iot_dps_tpm_client_options opts;
      int state;
      uint16_t http_status;
      int32_t error_code;
      char operation_id[AZ_IOT_DPS_TPM_TOKEN_MAX + 1];
      char key_name[AZ_IOT_DPS_TPM_TOKEN_MAX + 1];
      char assigned_hub[AZ_IOT_DPS_TPM_HOST_MAX + 1];
      char device_id[AZ_IOT_DPS_TPM_ID_MAX + 1];
      az_iot_dps_tpm_assignment assignment;
    } _internal;
  } az_iot_dps_tpm_client;

  /** @brief Options with every field unset. */
  AZ_NODISCARD az_iot_dps_tpm_client_options az_iot_dps_tpm_client_options_default(void);

  /**
   * @brief Validates @p options and starts a registration. Call again to
   * start over, e.g. after a failure.
   *
   * @param[out] client  Client.
   * @param[in]  options Options; copied.
   * @return AZ_IOT_OK; AZ_IOT_ERR_INVALID_ARG for a NULL pointer, a missing
   * required option, an ID with other characters or longer than
   * AZ_IOT_DPS_TPM_ID_MAX, a payload that is not one JSON object, or a CSR or
   * API version with characters JSON or a URL query would need escaped.
   */
  AZ_NODISCARD az_iot_result az_iot_dps_tpm_client_init(
      az_iot_dps_tpm_client* client,
      const az_iot_dps_tpm_client_options* options);

  /**
   * @brief Builds the next request into @p buffer. Rebuilds the same request
   * when called again, e.g. to resend after a transport failure.
   *
   * @param[in]  client       Client.
   * @param[in]  unix_seconds Current Unix time; used for the SAS token expiry
   *                          once DPS has sent its key.
   * @param[out] buffer       Storage for @p out; see
   *                          AZ_IOT_DPS_TPM_REQUEST_BUFFER_SIZE.
   * @param[in]  buffer_size  Bytes in @p buffer.
   * @param[out] out          Request.
   * @return AZ_IOT_OK; AZ_IOT_ERR_INVALID_ARG for a NULL pointer or a 0 time
   * when a token is needed; AZ_IOT_ERR_NOT_INITIALIZED when there is nothing
   * to send (assigned or failed); AZ_IOT_ERR_NOT_ENOUGH_SPACE; the result of
   * az_iot_dps_tpm_ops::hmac_sha256. None of these change the client state.
   */
  AZ_NODISCARD az_iot_result az_iot_dps_tpm_client_get_request(
      az_iot_dps_tpm_client* client,
      uint64_t unix_seconds,
      char* buffer,
      size_t buffer_size,
      az_iot_dps_tpm_request* out);

  /**
   * @brief Consumes the response to the last request.
   *
   * 429 and 5xx keep the request for a retry. Any failure other than
   * AZ_IOT_ERR_INVALID_ARG ends the registration.
   *
   * @param[in]     client              Client.
   * @param[in]     http_status         HTTP status code.
   * @param[in,out] body                Response body. Modified: keys are
   *                                    decoded in place. Must stay valid while
   *                                    az_iot_dps_tpm_assignment::issued_certificate_chain
   *                                    is used.
   * @param[in]     retry_after_seconds Retry-After header value; 0 when absent.
   * @param[out]    out                 Next step.
   * @return AZ_IOT_OK; AZ_IOT_ERR_INVALID_ARG for a NULL pointer;
   * AZ_IOT_ERR_NOT_INITIALIZED when no request is outstanding;
   * AZ_IOT_ERR_AUTH when DPS refused the TPM-signed request;
   * AZ_IOT_ERR_DPS when DPS failed the registration (see
   * az_iot_dps_tpm_client_get_error()); AZ_IOT_ERR_PROTOCOL for a response
   * that cannot be parsed; the result of az_iot_dps_tpm_ops::import_key.
   */
  AZ_NODISCARD az_iot_result az_iot_dps_tpm_client_handle_response(
      az_iot_dps_tpm_client* client,
      uint16_t http_status,
      az_span body,
      uint32_t retry_after_seconds,
      az_iot_dps_tpm_next* out);

  /**
   * @brief The assignment, once az_iot_dps_tpm_next::assigned.
   *
   * @return NULL before then.
   */
  const az_iot_dps_tpm_assignment* az_iot_dps_tpm_client_get_assignment(
      const az_iot_dps_tpm_client* client);

  /**
   * @brief HTTP status and DPS `errorCode` of the last failed response.
   *
   * @param[in]  client      Client.
   * @param[out] http_status HTTP status; 0 when none. May be NULL.
   * @param[out] error_code  DPS error code; 0 when absent. May be NULL.
   */
  void az_iot_dps_tpm_client_get_error(
      const az_iot_dps_tpm_client* client,
      uint16_t* http_status,
      int32_t* error_code);

  /**
   * @brief Splits a decoded `authenticationKey` into the inputs of
   * TPM2_ActivateCredential and TPM2_Import.
   *
   * @param[in]  key     Key given to az_iot_dps_tpm_ops::import_key.
   * @param[in]  key_len Bytes in @p key.
   * @param[out] out     Parts; zeroed on failure.
   * @return AZ_IOT_OK; AZ_IOT_ERR_INVALID_ARG for a NULL pointer;
   * AZ_IOT_ERR_PROTOCOL when a part overruns @p key.
   */
  AZ_NODISCARD az_iot_result
  az_iot_dps_tpm_parse_key(const uint8_t* key, size_t key_len, az_iot_dps_tpm_key_parts* out);

  /**
   * @brief Registration ID derived from the EK: lowercase unpadded base32 of
   * SHA-256(@p endorsement_key). Matches the ID the earlier Azure IoT C SDK
   * derives, for enrollments created with it.
   *
   * @param[in]  crypto              Crypto backend (SHA-256).
   * @param[in]  endorsement_key     Marshalled EK public area, as in the
   *                                 options.
   * @param[in]  endorsement_key_len Bytes in @p endorsement_key; not 0.
   * @param[out] out                 Registration ID, NUL-terminated.
   * @return AZ_IOT_OK; AZ_IOT_ERR_INVALID_ARG for a NULL pointer, an empty
   * key or an unusable @p crypto; the backend's error otherwise.
   */
  AZ_NODISCARD az_iot_result az_iot_dps_tpm_registration_id_from_ek(
      const az_iot_crypto* crypto,
      const uint8_t* endorsement_key,
      size_t endorsement_key_len,
      char out[AZ_IOT_DPS_TPM_REGISTRATION_ID_SIZE]);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_DPS_TPM_CLIENT_H */
