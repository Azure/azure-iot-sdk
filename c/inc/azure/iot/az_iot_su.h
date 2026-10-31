// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
#ifndef AZ_IOT_SU_H
#define AZ_IOT_SU_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "az_iot_connection_client.h"
#include "az_iot_result.h"

/**
 * @brief Most steps an update manifest may have. Device Update allows 10.
 *
 * With AZ_IOT_SU_MAX_FILE_COUNT and AZ_IOT_SU_MAX_FILE_COUNT_PER_STEP, sizes the parsed manifest
 * az_iot_su_client embeds, and the update request. A manifest over any of them fails parsing, so
 * its workflow fails. Set them with the CMake options of the same names: the SDK and the
 * application must be built with the same values.
 */
#ifndef AZ_IOT_SU_MAX_STEP_COUNT
#define AZ_IOT_SU_MAX_STEP_COUNT 10
#endif

/** @brief Most files an update may have (manifest `files` and payload `fileUrls`). Device
 * Update allows 10. See AZ_IOT_SU_MAX_STEP_COUNT. */
#ifndef AZ_IOT_SU_MAX_FILE_COUNT
#define AZ_IOT_SU_MAX_FILE_COUNT 10
#endif

/** @brief Most files one step may reference; at most AZ_IOT_SU_MAX_FILE_COUNT. See
 * AZ_IOT_SU_MAX_STEP_COUNT. */
#ifndef AZ_IOT_SU_MAX_FILE_COUNT_PER_STEP
#define AZ_IOT_SU_MAX_FILE_COUNT_PER_STEP AZ_IOT_SU_MAX_FILE_COUNT
#endif

#if AZ_IOT_SU_MAX_STEP_COUNT < 1 || AZ_IOT_SU_MAX_FILE_COUNT < 1 \
    || AZ_IOT_SU_MAX_FILE_COUNT_PER_STEP < 1                     \
    || AZ_IOT_SU_MAX_FILE_COUNT_PER_STEP > AZ_IOT_SU_MAX_FILE_COUNT
#error "AZ_IOT_SU_MAX_*: each must be at least 1, and files per step at most the file count"
#endif

#ifdef __cplusplus
extern "C"
{
#endif

  /* --- Update manifest (parsed by the SDK) --------------------------------- */

/** @brief Most hashes kept per manifest file; a file with more fails parsing. */
#define AZ_IOT_SU_MAX_FILE_HASH_COUNT 2

  /** @brief Identity of the update a manifest describes. */
  typedef struct az_iot_su_client_update_manifest_update_id
  {
    az_span provider; /**< Update provider. */
    az_span name; /**< Update name. */
    az_span version; /**< Update version. */
  } az_iot_su_client_update_manifest_update_id;

  /** @brief Properties of a manifest step for its handler. */
  typedef struct az_iot_su_client_update_manifest_instructions_step_handler_properties
  {
    /** Defines a successful installation of the step. */
    az_span installed_criteria;
  } az_iot_su_client_update_manifest_instructions_step_handler_properties;

  /** @brief A step of the manifest instructions. */
  typedef struct az_iot_su_client_update_manifest_instructions_step
  {
    /** Update handler of the step, e.g. `microsoft/swupdate:1`. */
    az_span handler;
    /** Ids of the files the step uses; see az_iot_su_client_update_manifest.files. */
    az_span files[AZ_IOT_SU_MAX_FILE_COUNT_PER_STEP];
    /** Number of items in `files`. */
    uint32_t files_count;
    /** Properties for the handler. */
    az_iot_su_client_update_manifest_instructions_step_handler_properties handler_properties;
  } az_iot_su_client_update_manifest_instructions_step;

  /** @brief Instructions of an update manifest. */
  typedef struct az_iot_su_client_update_manifest_instructions
  {
    /** Steps, in order. */
    az_iot_su_client_update_manifest_instructions_step steps[AZ_IOT_SU_MAX_STEP_COUNT];
    /** Number of items in `steps`. */
    uint32_t steps_count;
  } az_iot_su_client_update_manifest_instructions;

  /** @brief A hash of a manifest file. */
  typedef struct az_iot_su_client_update_manifest_file_hash
  {
    az_span hash_type; /**< Hash algorithm, e.g. `sha256`. */
    az_span hash_value; /**< Base64 hash value. */
  } az_iot_su_client_update_manifest_file_hash;

  /** @brief A file of an update manifest. */
  typedef struct az_iot_su_client_update_manifest_file
  {
    /** File id, as in step `files` and the request's `fileUrls`. */
    az_span id;
    /** File name. */
    az_span file_name;
    /** File size in bytes. */
    int64_t size_in_bytes;
    /** Hashes of the file. */
    az_iot_su_client_update_manifest_file_hash hashes[AZ_IOT_SU_MAX_FILE_HASH_COUNT];
    /** Number of items in `hashes`. */
    uint32_t hashes_count;
    /** MIME type; empty when absent. */
    az_span mime_type;
    /** The `properties` JSON object, unparsed; empty when absent. */
    az_span properties;
    /** `downloadHandler` id; empty when absent. Always empty for a related file. */
    az_span download_handler_id;
    /** First of this file's related files in the manifest's `related_files`. */
    uint32_t related_files_index;
    /** Number of this file's related files; 0 for a related file. */
    uint32_t related_files_count;
  } az_iot_su_client_update_manifest_file;

  /**
   * @brief A parsed update manifest. Spans point into the manifest text.
   *
   * Members the SDK does not use, such as `compatibility`, are skipped.
   */
  typedef struct az_iot_su_client_update_manifest
  {
    /** Manifest schema version. */
    az_span manifest_version;
    /** Identity of the update. */
    az_iot_su_client_update_manifest_update_id update_id;
    /** Steps to run. */
    az_iot_su_client_update_manifest_instructions instructions;
    /** Files the steps use. */
    az_iot_su_client_update_manifest_file files[AZ_IOT_SU_MAX_FILE_COUNT];
    /** Number of items in `files`. */
    uint32_t files_count;
    /** Related files (`relatedFiles`) of all files, in caller or workspace storage; see
     * az_iot_su_limits::max_related_files. Those past its capacity are left out. */
    az_iot_su_client_update_manifest_file* related_files;
    /** Number of items in `related_files`. */
    uint32_t related_files_count;
    /** Creation date and time. */
    az_span create_date_time;
  } az_iot_su_client_update_manifest;

  /* --- Update request ------------------------------------------------------ */

/** @brief Default agent version reported to the service (`agentVersion`). */
#define AZ_IOT_SU_CLIENT_AGENT_VERSION "DU;agent/1.0.0"

  /** @brief Action the service asks of the device. */
  typedef enum az_iot_su_client_service_action
  {
    /** Apply the offered update. */
    AZ_IOT_SU_CLIENT_SERVICE_ACTION_APPLY_DEPLOYMENT = 3,
  } az_iot_su_client_service_action;

  /** @brief The deployment an update request belongs to. */
  typedef struct az_iot_su_client_workflow
  {
    /** Action to take. */
    az_iot_su_client_service_action action;
    /** Workflow (deployment) id. */
    az_span id;
    /** Time of the last retry of the deployment; empty when none. */
    az_span retry_timestamp;
  } az_iot_su_client_workflow;

  /** @brief Download URL of an update file. */
  typedef struct az_iot_su_client_file_url
  {
    /** File id, as in the manifest `files`. */
    az_span id;
    /** Download URL. */
    az_span url;
  } az_iot_su_client_file_url;

  /** @brief An update request (`updateMetadata`). Spans point into the request text. */
  typedef struct az_iot_su_client_update_request
  {
    /** Deployment the request belongs to. */
    az_iot_su_client_workflow workflow;
    /** Update manifest text, as signed. */
    az_span update_manifest;
    /** JWS over the manifest. */
    az_span update_manifest_signature;
    /** Download URL of each file and related file, in caller or workspace storage of
     * AZ_IOT_SU_FILE_URL_CAPACITY() entries. */
    az_iot_su_client_file_url* file_urls;
    /** Number of items in `file_urls`. */
    uint32_t file_urls_count;
  } az_iot_su_client_update_request;

  /** @brief Overall result of a workflow, reported to the service. */
  typedef struct az_iot_su_client_install_result
  {
    /** Result code; AZ_IOT_SU_AGENT_RESULT_CODE_SUCCESS on success. */
    int32_t result_code;
    /** Extended result code; see AZ_IOT_SU_EXTENDED_RESULT(). */
    int32_t extended_result_code;
    /** Free-form result details. */
    az_span result_details;
  } az_iot_su_client_install_result;

  /* --- Result codes (returned by platform hooks) --------------------------- */

#define AZ_IOT_SU_RESULT_SUCCESS 0
#define AZ_IOT_SU_RESULT_IN_PROGRESS 1
#define AZ_IOT_SU_RESULT_REBOOT_REQUIRED 2
#define AZ_IOT_SU_RESULT_ALREADY_INSTALLED 3
#define AZ_IOT_SU_RESULT_FAILURE (-1)

/* The agent "success" result code reported to the software updates service (software updates agent
 * convention; the value used throughout azure-sdk-for-c examples). */
#define AZ_IOT_SU_AGENT_RESULT_CODE_SUCCESS 700

/* extended_result_code facility nibble (bits 31..28). The low 28 bits carry the
 * originating hook's raw result (truncated) or an SDK sub-code. See the design
 * doc "Result-Code Mapping". */
#define AZ_IOT_SU_FACILITY_MANIFEST 0x1u /* JWS / SJWK verification, kid, alg */
#define AZ_IOT_SU_FACILITY_DOWNLOAD 0x2u /* download_fn transport failure */
#define AZ_IOT_SU_FACILITY_HASH 0x3u /* SHA-256 mismatch */
#define AZ_IOT_SU_FACILITY_BACKUP 0x4u /* backup_fn failure */
#define AZ_IOT_SU_FACILITY_INSTALL 0x5u /* install_fn failure */
#define AZ_IOT_SU_FACILITY_APPLY 0x6u /* apply_fn failure */
#define AZ_IOT_SU_FACILITY_RESTORE 0x7u /* restore_fn failure (rollback failed) */
/* persist_workflow_state_fn kept failing at a reboot boundary */
#define AZ_IOT_SU_FACILITY_PERSIST 0x8u
/* Superseded by a new workflow under AZ_IOT_SU_REPLACEMENT_ROLLBACK (reserved; not yet produced) */
#define AZ_IOT_SU_FACILITY_SUPERSEDED 0x9u
#define AZ_IOT_SU_FACILITY_INTERNAL 0xFu /* parser/state/buffer error in the client */

/* Compose a 32-bit extended_result_code from a facility nibble + sub-code. */
#define AZ_IOT_SU_EXTENDED_RESULT(facility, code) \
  (int32_t)((((uint32_t)(facility) & 0xFu) << 28) | ((uint32_t)(code) & 0x0FFFFFFFu))

/* Sub-codes of AZ_IOT_SU_FACILITY_MANIFEST: why manifest verification failed. */
#define AZ_IOT_SU_MANIFEST_ERROR_CRYPTO 0x1u /* crypto backend fault, not a rejection */
#define AZ_IOT_SU_MANIFEST_ERROR_MALFORMED 0x2u /* signature, sjwk or key missing or invalid */
#define AZ_IOT_SU_MANIFEST_ERROR_TOO_LARGE 0x3u /* a part exceeds its scratch or field limit */
#define AZ_IOT_SU_MANIFEST_ERROR_ALG 0x4u /* alg is not RS256 */
#define AZ_IOT_SU_MANIFEST_ERROR_UNKNOWN_KID 0x5u /* sjwk kid matches no enabled root key */
#define AZ_IOT_SU_MANIFEST_ERROR_ROOT_SIGNATURE 0x6u /* sjwk does not verify with the root key */
#define AZ_IOT_SU_MANIFEST_ERROR_SIGNATURE 0x7u /* manifest does not verify with the sjwk key */
#define AZ_IOT_SU_MANIFEST_ERROR_HASH 0x8u /* manifest SHA-256 differs from the signed one */

  /* --- Compile-time capacities --------------------------------------------- */

#if defined(AZ_IOT_SU_MAX_ROOT_KEYS) || defined(AZ_IOT_MAX_SU_OBSERVERS)
#error \
    "AZ_IOT_SU_MAX_ROOT_KEYS and AZ_IOT_MAX_SU_OBSERVERS are removed: set az_iot_su_limits::max_root_keys and max_observers"
#endif
#if defined(AZ_IOT_SU_WORKFLOW_ID_SIZE) || defined(AZ_IOT_SU_RETRY_TIMESTAMP_SIZE)
#error \
    "AZ_IOT_SU_WORKFLOW_ID_SIZE and AZ_IOT_SU_RETRY_TIMESTAMP_SIZE are fixed and cannot be overridden"
#endif
#ifdef AZ_IOT_SU_MAX_WORKFLOW_ID_LEN
#error \
    "AZ_IOT_SU_MAX_WORKFLOW_ID_LEN is removed: workflow IDs are bounded by AZ_IOT_SU_WORKFLOW_ID_SIZE"
#endif

/** @brief Default az_iot_su_limits::max_root_keys. */
#ifndef AZ_IOT_SU_DEFAULT_MAX_ROOT_KEYS
#define AZ_IOT_SU_DEFAULT_MAX_ROOT_KEYS 8
#endif

/** @brief Default az_iot_su_limits::max_observers. */
#ifndef AZ_IOT_SU_DEFAULT_MAX_OBSERVERS
#define AZ_IOT_SU_DEFAULT_MAX_OBSERVERS 4
#endif

/** @brief Default az_iot_su_limits::max_related_files: four for each of ten files. */
#ifndef AZ_IOT_SU_DEFAULT_MAX_RELATED_FILES
#define AZ_IOT_SU_DEFAULT_MAX_RELATED_FILES 40
#endif

/** @brief Download URLs an update request may carry: one per file and per related file. */
#define AZ_IOT_SU_FILE_URL_CAPACITY(max_related_files) \
  ((size_t)(AZ_IOT_SU_MAX_FILE_COUNT) + (size_t)(max_related_files))

/** @brief Default az_iot_su_limits::max_update_size: the service's 64 KiB response ceiling. */
#ifndef AZ_IOT_SU_DEFAULT_MAX_UPDATE_SIZE
#define AZ_IOT_SU_DEFAULT_MAX_UPDATE_SIZE 65536
#endif

/** @brief Smallest az_iot_su_limits::max_message_size: the largest status report the client
 * builds fits, so no report is refused for its size. That report needs up to 2166 bytes plus 117
 * per step. */
#define AZ_IOT_SU_MIN_MESSAGE_SIZE (2176u + 128u * (AZ_IOT_SU_MAX_STEP_COUNT))

/** @brief Default az_iot_su_limits::max_message_size. */
#ifndef AZ_IOT_SU_DEFAULT_MAX_MESSAGE_SIZE
#define AZ_IOT_SU_DEFAULT_MAX_MESSAGE_SIZE AZ_IOT_SU_MIN_MESSAGE_SIZE
#endif

/** @brief Default az_iot_su_limits::result_details_size. */
#ifndef AZ_IOT_SU_DEFAULT_RESULT_DETAILS_SIZE
#define AZ_IOT_SU_DEFAULT_RESULT_DETAILS_SIZE 128
#endif

/** @brief Largest az_iot_su_limits::result_details_size: the service limit on resultDetails. */
#define AZ_IOT_SU_MAX_RESULT_DETAILS_SIZE 1024

/**
 * @brief Signature-verification scratch: the stack az_iot_su_parse_update_request() uses, and
 * the default az_iot_su_limits::scratch_size.
 *
 * Holds the decoded JWS header (which embeds the signing key), the signing key and the
 * signatures. A signature that does not fit is rejected with AZ_IOT_ERR_AUTH and an error
 * log naming the part that is too large.
 */
#ifndef AZ_IOT_SU_VERIFY_SCRATCH_SIZE
#define AZ_IOT_SU_VERIFY_SCRATCH_SIZE 8192
#endif

/** @brief Capacity of the packed applied update id (provider, name, version). */
#define AZ_IOT_SU_APPLIED_UPDATE_ID_SIZE 192

/** @brief Largest opaque channel state (e.g. ETags) carried in the persisted blob. */
#define AZ_IOT_SU_CHANNEL_STATE_MAX_SIZE 256

/** @brief Persisted-state bytes beyond the update metadata and the step result details: header
 * (40 bytes) and trailer (16 fixed, 16 per step, 4 + 16 per file URL, then length-prefixed
 * workflow id, applied update id, channel state and the details of each step, 16 retry schedule,
 * 4 CRC). Grows with the step and file limits. */
#ifndef AZ_IOT_SU_PERSIST_OVERHEAD
#define AZ_IOT_SU_PERSIST_OVERHEAD                                         \
  (92u + (AZ_IOT_SU_WORKFLOW_ID_SIZE) + (AZ_IOT_SU_APPLIED_UPDATE_ID_SIZE) \
   + (AZ_IOT_SU_CHANNEL_STATE_MAX_SIZE) + 20u * (AZ_IOT_SU_MAX_STEP_COUNT) \
   + 16u * (AZ_IOT_SU_MAX_FILE_COUNT))
#endif

/**
 * @brief Largest decoded workflow `id` the client accepts, in bytes. It is kept for reporting,
 * duplicate detection and persistence. A deployment with a longer id is refused: nothing is
 * processed or reported, and AZ_IOT_SU_EVENT_UPDATE_REFUSED is raised with
 * AZ_IOT_ERR_NOT_ENOUGH_SPACE. 128, the service's maximum workflow id length.
 */
#define AZ_IOT_SU_WORKFLOW_ID_SIZE 128
/** @brief Capacity for the copied-out `retryTimestamp` (ISO-8601, ~28 chars; not persisted). */
#define AZ_IOT_SU_RETRY_TIMESTAMP_SIZE 64

/**
 * @brief Consecutive failed persist_workflow_state_fn writes after which the client stops retrying.
 *
 * Retries are spaced 1 s, 2 s, 4 s, ... (at most 60 s). On reaching the limit
 * AZ_IOT_SU_EVENT_PERSIST_FAILED is raised with `persist_retrying` false and:
 * - a pending reboot checkpoint fails the workflow: restore_fn rolls it back
 *   and it is reported FAILED with facility AZ_IOT_SU_FACILITY_PERSIST on the
 *   step. If there is no restore_fn or a restore fails, the overall extended
 *   result carries AZ_IOT_SU_FACILITY_RESTORE (sub-code 0: no restore_fn);
 * - an unsent terminal report is no longer stored (it is still sent);
 * - a new workflow waiting on the previous record's retirement proceeds.
 * A later write is still attempted once when needed; a tracked success resets
 * the count (the best-effort erase of a stale record does not).
 * While a workflow is held at a reboot boundary, a new workflow is deferred
 * (AZ_IOT_SU_EVENT_UPDATE_DEFERRED).
 */
#ifndef AZ_IOT_SU_PERSIST_MAX_ATTEMPTS
#define AZ_IOT_SU_PERSIST_MAX_ATTEMPTS 5
#endif
#if AZ_IOT_SU_PERSIST_MAX_ATTEMPTS < 1 || AZ_IOT_SU_PERSIST_MAX_ATTEMPTS > 0xFFFFFFFF
#error "AZ_IOT_SU_PERSIST_MAX_ATTEMPTS must be between 1 and 0xFFFFFFFF"
#endif

/** @brief Persisted bytes per download URL beyond AZ_IOT_SU_MAX_FILE_COUNT. */
#define AZ_IOT_SU_PERSIST_URL_RECORD_SIZE 16

/** @brief Largest blob passed to persist_workflow_state_fn for a client with these
 * az_iot_su_limits::max_update_size, result_details_size and max_related_files; size storage for
 * this. The format version is internal and checked by az_iot_su_client_resume(). */
#define AZ_IOT_SU_STATE_BLOB_MAX_SIZE(max_update_size, result_details_size, max_related_files) \
  ((size_t)(max_update_size) + (size_t)AZ_IOT_SU_PERSIST_OVERHEAD                              \
   + (size_t)(AZ_IOT_SU_MAX_STEP_COUNT) * (size_t)(result_details_size)                        \
   + (size_t)(max_related_files) * (size_t)AZ_IOT_SU_PERSIST_URL_RECORD_SIZE)

  /* --- Internal fine-grained state enum ------------------------------------ */

  typedef enum az_iot_su_state
  {
    AZ_IOT_SU_STATE_IDLE = 0,
    AZ_IOT_SU_STATE_MANIFEST_RECEIVED,
    AZ_IOT_SU_STATE_VERIFYING_MANIFEST,
    AZ_IOT_SU_STATE_DOWNLOAD_STARTED,
    AZ_IOT_SU_STATE_DOWNLOAD_COMPLETE,
    AZ_IOT_SU_STATE_BACKUP_STARTED,
    AZ_IOT_SU_STATE_BACKUP_COMPLETE,
    AZ_IOT_SU_STATE_INSTALL_STARTED,
    AZ_IOT_SU_STATE_INSTALL_COMPLETE,
    AZ_IOT_SU_STATE_APPLY_STARTED,
    AZ_IOT_SU_STATE_RESTORE_STARTED,
    AZ_IOT_SU_STATE_FAILED,
    /** Library mode: the verified update is with the application, which drives
     * it and reports through az_iot_su_client_report_status(). */
    AZ_IOT_SU_STATE_DELEGATED,
  } az_iot_su_state;

  /* --- Platform hooks (vtable) --------------------------------------------- */

  /**
   * Platform-specific operations the application (or a bundled adapter) provides.
   * The core software updates library MUST NOT link any platform/OS/network code; everything
   * platform-specific is reached through this vtable.
   *
   * Hooks return one of the AZ_IOT_SU_RESULT_* codes above. download_fn and the
   * chunkable install/apply hooks MAY return IN_PROGRESS to be re-invoked on the
   * next do_work(); install_fn/apply_fn MAY return REBOOT_REQUIRED.
   */
  typedef struct az_iot_su_platform_hooks
  {
    /**
     * Download one file (called once per file, once per do_work iteration).
     * Return IN_PROGRESS to continue on the next do_work, SUCCESS when complete.
     * Hash verification is performed by core with the connection client's
     * crypto backend.
     */
    int32_t (*download_fn)(
        const az_iot_su_client_update_manifest_file* file,
        az_span download_url,
        uint32_t file_index,
        uint32_t file_count,
        void* user_ctx);

    /**
     * Read a chunk of a previously-downloaded file so core can verify its
     * SHA-256 hash against the signed manifest. OPTIONAL (MAY be NULL): when
     * provided, core performs streaming per-file hash verification after each
     * successful download_fn; when NULL, hash verification is skipped (the
     * platform is then responsible for integrity, e.g. verifying during
     * download). Read from byte @p offset into @p buffer (capacity
     * @p buffer_size); set @p out_read to the number of bytes read (0 signals
     * end-of-file). MUST return AZ_IOT_SU_RESULT_SUCCESS on a successful read.
     */
    int32_t (*read_file_fn)(
        const az_iot_su_client_update_manifest_file* file,
        uint32_t file_index,
        size_t offset,
        uint8_t* buffer,
        size_t buffer_size,
        size_t* out_read,
        void* user_ctx);

    /**
     * Install the previously-downloaded payload for one step.
     * MAY return IN_PROGRESS (chunked) or REBOOT_REQUIRED.
     */
    int32_t (*install_fn)(
        const az_iot_su_client_update_manifest* manifest,
        uint32_t step_index,
        void* user_ctx);

    /**
     * Apply (activate) the installed update for one step.
     * MAY return IN_PROGRESS (chunked) or REBOOT_REQUIRED.
     */
    int32_t (*apply_fn)(
        const az_iot_su_client_update_manifest* manifest,
        uint32_t step_index,
        void* user_ctx);

    /** Backup current state before installing. OPTIONAL (MAY be NULL). */
    int32_t (*backup_fn)(
        const az_iot_su_client_update_manifest* manifest,
        uint32_t step_index,
        void* user_ctx);

    /** Restore previous state on failure. OPTIONAL (MAY be NULL). */
    int32_t (*restore_fn)(
        const az_iot_su_client_update_manifest* manifest,
        uint32_t step_index,
        void* user_ctx);

    /**
     * Report whether the update described by manifest is already installed.
     * MUST return AZ_IOT_SU_RESULT_ALREADY_INSTALLED if so, SUCCESS otherwise.
     */
    int32_t (*is_installed_fn)(const az_iot_su_client_update_manifest* manifest, void* user_ctx);

    /**
     * Persist workflow state for reboot survival. OPTIONAL — REQUIRED only if a
     * reboot is possible mid-update (i.e. install/apply may return
     * REBOOT_REQUIRED), or for the terminal report to survive a reboot.
     *
     * Written at a reboot requested by install/apply, and when a workflow ends
     * (carrying its unsent terminal report until the service accepts it).
     *
     * @p state_blob_len == 0 means invalidate: empty or erase the stored record
     * so a later boot does not resume a workflow that has already ended. Return
     * non-zero on failure; the client raises AZ_IOT_SU_EVENT_PERSIST_FAILED and
     * retries from do_work() with back-off, holding the workflow at a reboot
     * boundary, up to AZ_IOT_SU_PERSIST_MAX_ATTEMPTS.
     */
    int32_t (*persist_workflow_state_fn)(
        const uint8_t* state_blob,
        size_t state_blob_len,
        void* user_ctx);

    /**
     * Load previously-persisted workflow state. Return 0 and fill
     * state_blob/len on success; non-zero if no state persisted. A successful
     * zero-length read also means nothing to resume.
     */
    int32_t (*load_workflow_state_fn)(
        uint8_t* state_blob,
        size_t state_blob_capacity,
        size_t* state_blob_len,
        void* user_ctx);

    void* user_ctx;
  } az_iot_su_platform_hooks;

  /* --- Download handlers ------------------------------------------------------------------- */

/** @brief Longest az_iot_su_download_handler::id, in bytes (the import manifest's limit). */
#define AZ_IOT_SU_MAX_DOWNLOAD_HANDLER_ID_SIZE 64

  /**
   * @brief Produces a file from a related file instead of downloading it in full, for files whose
   * manifest entry names this handler's id (e.g. a delta handler, "microsoft/delta:1").
   *
   * The client does not call download handlers yet; registering one only validates it.
   * Hooks return AZ_IOT_SU_RESULT_* codes and MAY return IN_PROGRESS to be re-invoked on the next
   * do_work().
   */
  typedef struct az_iot_su_download_handler
  {
    /** Handler id matched against a file's downloadHandler id: 1 to
     * AZ_IOT_SU_MAX_DOWNLOAD_HANDLER_ID_SIZE printable ASCII characters, no space. Must outlive
     * the client. */
    const char* id;

    /**
     * Choose the related file of @p file to download. Set @p out_related_file_index (0 to the
     * file's related-file count - 1) and return SUCCESS. Any other result except IN_PROGRESS
     * means no related file is usable: @p file is downloaded in full.
     */
    int32_t (*choose_related_file_fn)(
        const az_iot_su_client_update_manifest* manifest,
        const az_iot_su_client_update_manifest_file* file,
        uint32_t* out_related_file_index,
        void* user_ctx);

    /**
     * Build @p file, the manifest's file @p file_index, from the downloaded @p related_file.
     * Core then verifies @p file's hash with read_file_fn; on failure @p file is downloaded in
     * full.
     */
    int32_t (*build_file_from_related_fn)(
        const az_iot_su_client_update_manifest_file* file,
        const az_iot_su_client_update_manifest_file* related_file,
        uint32_t file_index,
        void* user_ctx);

    /** OPTIONAL (MAY be NULL). Called once a workflow that used this handler succeeds, e.g. to
     * keep its files as sources for later updates. */
    void (*workflow_succeeded_fn)(const az_iot_su_client_update_manifest* manifest, void* user_ctx);

    void* user_ctx;
  } az_iot_su_download_handler;

  /* --- Root key store (owned and managed by software updates core) ---------------------- */

  /**
   * An RSA root public key trusted to sign Signed JWKs (SJWKs). All fields are
   * caller-owned; core stores the pointers (no deep copy of key bytes), so the
   * arrays MUST outlive the client.
   */
  typedef struct az_iot_su_root_key
  {
    const char* kid; /* JWK key id, matched against the SJWK header `kid`. */
    const uint8_t* modulus; /* big-endian RSA modulus (n). */
    size_t modulus_len;
    const uint8_t* exponent; /* big-endian RSA exponent (e). */
    size_t exponent_len;
    bool disabled; /* true = revoked/disabled; rejected during resolution. */
  } az_iot_su_root_key;

  /* --- Device properties (plain struct, deep-copied by the client) --------- */

  typedef struct az_iot_su_update_id_info
  {
    const char* provider;
    const char* name;
    const char* version;
  } az_iot_su_update_id_info;

/** @brief Maximum compatibility properties: manufacturer, model and custom ones combined. */
#define AZ_IOT_SU_MAX_COMPATIBILITY_PROPERTIES 5

  /** @brief A custom compatibility property. */
  typedef struct az_iot_su_custom_property
  {
    const char* name; /**< Nonempty; unique across the emitted properties. */
    const char* value; /**< May be empty, not NULL. */
  } az_iot_su_custom_property;

  /**
   * @brief Device properties sent on update checks.
   *
   * Compatibility properties go only on update checks. The installed update ID
   * also goes on status reports that have no applied update to report, so
   * replacing it can change a pending status report.
   *
   * Caller-owned. az_iot_su_client_init() and
   * az_iot_su_client_update_device_properties() deep-copy them; the caller may
   * then change or free them.
   *
   * The managed client requires 1 to AZ_IOT_SU_MAX_COMPATIBILITY_PROPERTIES
   * compatibility properties: manufacturer and model count one each when
   * non-NULL, plus the custom ones. The installed update ID takes at most
   * AZ_IOT_SU_APPLIED_UPDATE_ID_SIZE bytes, NUL terminators included. All copied strings take
   * az_iot_su_device_properties_buffer_size() bytes, at most
   * az_iot_su_limits::device_properties_size.
   */
  typedef struct az_iot_su_device_properties
  {
    const char* manufacturer; /**< Compatibility property; NULL to omit. */
    const char* model; /**< Compatibility property; NULL to omit. */
    /** All NULL (nothing installed), or a complete nonempty triple. */
    az_iot_su_update_id_info installed_update_id;
    const az_iot_su_custom_property* custom_properties; /**< May be NULL if count is 0. */
    size_t custom_properties_count; /**< Entries in custom_properties. */
  } az_iot_su_device_properties;

/**
 * @brief Default az_iot_su_limits::device_properties_size, in bytes.
 *
 * Size it with az_iot_su_device_properties_buffer_size() of the largest properties the
 * application will set, including later az_iot_su_client_update_device_properties() calls.
 */
#ifndef AZ_IOT_SU_DEVICE_PROPERTIES_BUFFER_SIZE
#define AZ_IOT_SU_DEVICE_PROPERTIES_BUFFER_SIZE 512
#endif

/**
 * @brief The `timeout_ms` that asks for NO bound: the request is retried for as
 * long as it takes, and the only abandonment is a channel verdict.
 *
 * Named because a bare 0 at a call site reads like "expire immediately", which
 * is the opposite of what it does. */
#define AZ_IOT_SU_REQUEST_NO_TIMEOUT 0u

/**
 * @brief A default request timeout, in milliseconds, for callers with no policy
 * of their own.
 *
 * One minute. A check not answered within it -- still being retried, or sent
 * and awaiting its response -- is abandoned and told to the application, which
 * can ask again when it chooses instead of the client waiting or retrying
 * silently.
 *
 * Only a default. The bound is a per-call argument of
 * az_iot_su_client_request_update() / _request_onboarding_update(), because
 * only the application knows how long it can wait for a given check -- a
 * boot-time onboarding probe and a nightly background poll do not share a
 * deadline. */
#define AZ_IOT_SU_REQUEST_DEFAULT_TIMEOUT_MS 60000u

  /**
   * @brief Exact az_iot_su_limits::device_properties_size needed to cache @p device_properties.
   *
   * @param[in] device_properties Properties to size.
   * @return Bytes of packed NUL-terminated strings; 0 if @p device_properties is
   *   NULL or invalid. An update-check body that would
   *   not fit the channel is still rejected when the properties are set.
   */
  AZ_NODISCARD size_t
  az_iot_su_device_properties_buffer_size(const az_iot_su_device_properties* device_properties);

  /* --- Client struct -------------------------------------------------------- */

  /* --- Update outcome vocabulary ------------------------------------------ */
  /* The structured result the engine produces for a workflow. Public because an
   * application observes it; the transport that carries it is internal. */

  /**
   * @brief Terminal and non-terminal outcomes a device reports for a workflow.
   *
   * These are the values the service accepts on the status-report operation.
   * An engine that declines a deployment reports `SKIPPED`; there is no
   * protocol-level accept/reject acknowledgement.
   */
  typedef enum az_iot_su_outcome
  {
    AZ_IOT_SU_OUTCOME_IN_PROGRESS = 0,
    AZ_IOT_SU_OUTCOME_SUCCEEDED,
    AZ_IOT_SU_OUTCOME_FAILED,
    AZ_IOT_SU_OUTCOME_CANCELED, /**< Set by the service; the client never reports it. */
    AZ_IOT_SU_OUTCOME_SKIPPED,
  } az_iot_su_outcome;

  /**
   * @brief Which layer a failure came from. `NOT_APPLICABLE` is used for every
   *        non-failure outcome.
   */
  typedef enum az_iot_su_failure_origin
  {
    AZ_IOT_SU_FAILURE_ORIGIN_NOT_APPLICABLE = 0,
    AZ_IOT_SU_FAILURE_ORIGIN_CLOUD_SERVICE,
    AZ_IOT_SU_FAILURE_ORIGIN_MANAGED_RESOURCE,
    AZ_IOT_SU_FAILURE_ORIGIN_AGENT_CORE,
    AZ_IOT_SU_FAILURE_ORIGIN_AGENT_EXTENSION,
    AZ_IOT_SU_FAILURE_ORIGIN_AGENT_DEPENDENCY,
    AZ_IOT_SU_FAILURE_ORIGIN_DEVICE,
    AZ_IOT_SU_FAILURE_ORIGIN_OTHER,
  } az_iot_su_failure_origin;

  /** @brief An update identity triple. Spans are NOT owned by this struct. */
  typedef struct az_iot_su_report_update_id
  {
    const char* provider;
    const char* name;
    const char* version;
  } az_iot_su_report_update_id;

  /**
   * @brief Terminal result for one manifest step.
   *
   * The DPS report contract requires every serialized step to carry all four
   * structured result fields. `result_details` is optional and its span is
   * borrowed for the duration of the report call.
   */
  typedef struct az_iot_su_step_result
  {
    az_iot_su_outcome outcome;
    az_iot_su_failure_origin failure_origin;
    int32_t result_code;
    int32_t extended_result_code;
    az_span result_details;
  } az_iot_su_step_result;

  /**
   * @brief The structured result the engine hands a channel.
   *
   * `workflow_id` alone is the correlation key: reporting is idempotent on it.
   *
   * `installed_update_id` means "what is installed on the device *now*", not
   * "what this workflow is about". It is therefore the previously installed
   * update while a workflow is in progress or has failed, and the newly applied
   * update once the workflow has succeeded. It may be NULL when the device has
   * nothing installed, in which case the channel omits it rather than
   * serializing a null.
   */
  typedef struct az_iot_su_report
  {
    const char* workflow_id;

    /* NULL when the device has nothing installed. */
    const az_iot_su_report_update_id* installed_update_id;

    az_iot_su_outcome outcome;
    az_iot_su_failure_origin failure_origin;

    /* Agent result code. The engine emits the values the contract defines:
     * 1 while in progress, 700 on success, negative on failure. */
    int32_t result_code;

    /* Comma-separated hex codes, e.g. "00000000" or "0x80000001". Never NULL. */
    const char* extended_result_codes;

    /* Free-form human-readable detail. May be NULL. */
    const char* result_details;

    /* Terminal per-step results. Omitted for IN_PROGRESS reports. Array and
     * detail spans are borrowed for the report call; NULL when count is zero. */
    const az_iot_su_step_result* step_results;
    int32_t step_results_count;
  } az_iot_su_report;

  /* Opaque forward declaration. The delivery/reporting channel is an INTERNAL
   * construct (src/features/su/internal/su_channel_internal.h): applications
   * do not build one and cannot see inside it. It is named here only because
   * the client struct is caller-allocated and therefore needs its size. */
  struct az_iot_su_channel_vtable;

  /**
   * @brief Which device-update operation an event refers to.
   *
   * The two fetch routes are distinct: the onboarding route is the day-0 one,
   * for a device with no registry entry yet, and only the application knows
   * which it asked for. A status report is a third thing again -- losing one
   * costs the service its record of what the device did, which is not the same
   * failure as an update check coming back empty.
   */
  typedef enum az_iot_su_operation
  {
    /* Day-0 fetch: the device has no registry entry yet. Sends agentInfo, and
     * installedUpdateId when one is set. */
    AZ_IOT_SU_OP_GET_ONBOARDING_UPDATE = 0,
    /* Operational fetch: the device is already registered. */
    AZ_IOT_SU_OP_GET_UPDATE,
    /* Status report for a workflow. */
    AZ_IOT_SU_OP_REPORT_STATUS,
  } az_iot_su_operation;

  /** @brief Which kind of thing an az_iot_su_event reports. */
  typedef enum az_iot_su_event_kind
  {
    /* The deployment workflow moved. Carries `state` and `previous_state`.
     * Otherwise observable only by polling az_iot_su_client_get_state().
     * Entering AZ_IOT_SU_STATE_FAILED also carries `reason` and
     * `extended_result_code`; other transitions carry AZ_IOT_OK and 0. */
    AZ_IOT_SU_EVENT_WORKFLOW_STATE_CHANGED = 0,

    /* An operation was ABANDONED: it reached a verdict that stops the client
     * re-arming it. Carries `operation`, `reason` and the service diagnosis.
     *
     * "Retry" here means the CLIENT's own re-arming, which the application
     * cannot see: on a retryable failure the engine puts the request back and
     * reissues it on the next az_iot_su_client_do_work(), for as long as it
     * takes. This ends that loop. It does NOT mean the application may not ask
     * again -- asking again is the intended response, at a time it chooses.
     *
     * Ask again on the SAME route, which is why `operation` is carried:
     * az_iot_su_client_request_onboarding_update() for
     * AZ_IOT_SU_OP_GET_ONBOARDING_UPDATE, az_iot_su_client_request_update()
     * for AZ_IOT_SU_OP_GET_UPDATE. They are not interchangeable -- the regular
     * route is rejected for a device with no registry entry.
     *
     * An abandoned AZ_IOT_SU_OP_REPORT_STATUS is DIFFERENT: there is no public
     * call to reissue one, and the event is diagnostic. The client reports
     * again at its next REPORTING point, which is not the same as its next
     * state change -- several transitions are deliberately silent (the
     * SDK does not put every intermediate step on the wire), so the next report
     * may be some steps away, or not until the workflow reaches a terminal
     * state. The service therefore keeps the last state it was told for a
     * while. That is the intended behaviour, not a gap to work around: do not
     * drive a workflow from the reported state, and treat this event as
     * something to log rather than something to act on.
     *
     * The exception is a TERMINAL report the service rejected permanently
     * (`service_error` names the cause, e.g. REPORT_CONFLICT or
     * UNKNOWN_WORKFLOW_ID): it is retired and never sent again. The
     * service's view is authoritative; call az_iot_su_client_request_update()
     * to learn it.
     *
     * A report rejected with REPORT_CONFLICT, or with an untyped 409000, is
     * the one case where the client checks for updates itself, after this
     * event: on the route last requested (regular if none), with
     * AZ_IOT_SU_REQUEST_DEFAULT_TIMEOUT_MS. No check is added while one is
     * queued or in flight; that check's answer serves instead. Its answer
     * arrives as for any check. */
    AZ_IOT_SU_EVENT_OPERATION_ABANDONED,

    /* persist_workflow_state_fn failed. Raised on the first failure of an episode
     * (`persist_retrying` true) and when AZ_IOT_SU_PERSIST_MAX_ATTEMPTS is
     * reached (`persist_retrying` false); with a limit of 1, once, with
     * `persist_retrying` false. See that macro for what giving up does.
     * Carries `state`, `reason` (AZ_IOT_ERR_INTERNAL) and `persist_attempts`.
     * Any write may fail: a reboot checkpoint, the terminal record, or the
     * clear of a superseded record. Until PERSIST_RECOVERED, stored state may
     * be missing or stale; a workflow at a reboot boundary is held there, so
     * do not reboot the device (see az_iot_su_client_get_reboot_state()). */
    AZ_IOT_SU_EVENT_PERSIST_FAILED,

    /* A tracked persist_workflow_state_fn write succeeded after
     * AZ_IOT_SU_EVENT_PERSIST_FAILED. Carries `state` and `persist_attempts`
     * (the failures that preceded it). The best-effort erase of a stale record
     * after a failed terminal-record write is not tracked and never raises it. */
    AZ_IOT_SU_EVENT_PERSIST_RECOVERED,

    /* A delivered update was refused before processing, so nothing was
     * installed or reported for it. Carries `state` (unchanged) and `reason`:
     * AZ_IOT_ERR_NOT_ENOUGH_SPACE when its workflow id exceeds
     * AZ_IOT_SU_WORKFLOW_ID_SIZE, it has more `fileUrls` than
     * AZ_IOT_SU_MAX_FILE_COUNT or the payload exceeds
     * az_iot_su_limits::max_update_size; AZ_IOT_ERR_PROTOCOL when the payload
     * cannot be decoded (not JSON, a required field missing or of the wrong
     * type, or an invalid string escape). Raised again on each redelivery. */
    AZ_IOT_SU_EVENT_UPDATE_REFUSED,

    /* A delivered update with a new workflow id was not started because the
     * current workflow may not be replaced (see az_iot_su_replacement_policy),
     * or a due retry of a failed workflow was not started because that
     * workflow's terminal report is not yet settled with the service.
     * Nothing was processed or reported for it. Carries `state` (unchanged)
     * and `reason` AZ_IOT_ERR_BUSY. Raised again on each redelivery. To take
     * it, request an update again once the current workflow has ended (Idle
     * or Failed) and its report is settled and, when its last apply asked for
     * a reboot, after that reboot. */
    AZ_IOT_SU_EVENT_UPDATE_DEFERRED,

    /* An update check was answered with no update. Carries `state` and
     * `operation` (the route that asked). */
    AZ_IOT_SU_EVENT_NO_UPDATE,

    /* A delivered update carried the workflow id of the current workflow, or
     * of the last one when it SUCCEEDED or was SKIPPED, and was ignored as a
     * redelivery: nothing was processed or reported for it. Carries `state`
     * (unchanged). Raised again on each redelivery. A FAILED workflow
     * offered again is retried instead (see
     * AZ_IOT_SU_EVENT_UPDATE_RETRY_WAIT). */
    AZ_IOT_SU_EVENT_UPDATE_DUPLICATE,

    /* The service accepted a status report. Carries `state` and
     * `report_terminal`. Once a terminal report is accepted, nothing more is
     * owed to the service for that workflow. */
    AZ_IOT_SU_EVENT_REPORT_ACCEPTED,

    /* The last workflow FAILED and was offered again before
     * its retry wait elapsed, so it was not started: nothing was processed or
     * reported for it. Carries `state` (unchanged) and `retry_after_ms`.
     * Raised again on each early offer. The wait is 1 h after the first
     * failure, doubling per consecutive failure up to 24 h, plus up to 10%
     * jitter, and survives a restart when persistence is configured. */
    AZ_IOT_SU_EVENT_UPDATE_RETRY_WAIT,

    /* A workflow's terminal report is settled: nothing more is sent for that
     * workflow. Raised once, after REPORT_ACCEPTED or OPERATION_ABANDONED for
     * that report. Carries `state`, `report` (the report as sent) and
     * `reason`: AZ_IOT_OK when the service accepted it; otherwise the verdict
     * that retired it, with `service_error`. Not raised for a report kept
     * after a lost session (AZ_IOT_ERR_NOT_CONNECTED): that one is sent again
     * and settles later. */
    AZ_IOT_SU_EVENT_WORKFLOW_COMPLETED,

    /** Library mode only: an update passed verification and is now the
     * application's to drive. Carries `state` (DELEGATED), `request` and
     * `manifest`. The SDK runs none of its platform hooks for it; report
     * progress and the outcome with az_iot_su_client_report_status(). */
    AZ_IOT_SU_EVENT_UPDATE_AVAILABLE
  } az_iot_su_event_kind;

  /**
   * @brief What the service said about a refused operation.
   *
   * The classification alone collapses distinct failures that need different
   * operator responses: a malformed body, a device that is not onboarded, a
   * rejected credential and a disabled enrollment all arrive as one result
   * code. These are the fields that tell them apart.
   *
   * All are best-effort: a body that carries none leaves the strings empty and
   * the numeric code 0. Valid only for the duration of the callback. */
  typedef struct az_iot_su_service_error
  {
    /* Numeric `errorCode`, e.g. 400002. 0 when the body carried none. */
    int32_t code;
    /* Best-effort error TEXT, not a guaranteed machine-readable code.
     *
     * The device-facing body carries the originating code in "message" when it
     * has one ("INVALID_REQUEST", "UNKNOWN_WORKFLOW_ID"), which is what the
     * classifier matches on -- but the same field is sometimes free prose
     * ("Deserialization error."), so an application MUST NOT treat it as a
     * stable identifier. Log it, show it, match on it defensively; branch on
     * `code` and on the event's `reason`.
     *
     * Never NULL; empty when the body carried none. */
    const char* message;
    /* The service's correlation GUID (`trackingId`) -- the one value a support
     * request needs. Never NULL; empty when the body carried none. */
    const char* tracking_id;
    /* How long the service asked the device to wait before asking again, in
     * milliseconds. 0 when it asked for no particular delay.
     *
     * Carried so the application can SCHEDULE its next attempt instead of
     * guessing. It is the service's own instruction, not an SDK heuristic: the
     * client does not silently wait it out on the application's behalf, because
     * that would consume a request budget the application set. */
    uint32_t retry_after_ms;
  } az_iot_su_service_error;

  /**
   * @brief SDK-produced, callback-lifetime view of something the software updates client
   * did.
   *
   * Read `kind` first: it says which of the remaining fields carry meaning.
   * The event and everything it points at are valid only until the callback
   * returns; copy anything that must outlive it. */
  typedef struct az_iot_su_event
  {
    az_iot_su_event_kind kind;

    /** Every event: the workflow state when it was raised (the new state for
     * WORKFLOW_STATE_CHANGED). */
    az_iot_su_state state;
    /** WORKFLOW_STATE_CHANGED only; other events repeat `state`. */
    az_iot_su_state previous_state;

    /* `operation`: OPERATION_ABANDONED and NO_UPDATE only; unspecified for
     * other events. `reason` is also set by PERSIST_FAILED, UPDATE_REFUSED,
     * UPDATE_DEFERRED and WORKFLOW_STATE_CHANGED into FAILED. */
    az_iot_su_operation operation;
    az_iot_result reason;
    az_iot_su_service_error service_error;

    /* PERSIST_FAILED / PERSIST_RECOVERED only. */
    uint32_t persist_attempts; /**< Consecutive failed writes. */
    bool persist_retrying; /**< PERSIST_FAILED: whether the client retries. */

    /** WORKFLOW_STATE_CHANGED into FAILED: the failed step's extended result
     * code (AZ_IOT_SU_EXTENDED_RESULT() layout: facility in bits 31..28, e.g.
     * AZ_IOT_SU_FACILITY_MANIFEST with an AZ_IOT_SU_MANIFEST_ERROR_* sub-code),
     * else the overall one. `reason` is then AZ_IOT_ERR_AUTH when the manifest
     * or a file failed verification (a file hash that cannot be read
     * included), AZ_IOT_ERR_PROTOCOL for a manifest that cannot be decoded or
     * parsed, AZ_IOT_ERR_INTERNAL otherwise (AZ_IOT_SU_MANIFEST_ERROR_CRYPTO
     * included). 0 for other events. */
    int32_t extended_result_code;
    /** REPORT_ACCEPTED: the report carried a terminal outcome. */
    bool report_terminal;
    /** UPDATE_RETRY_WAIT: milliseconds until the workflow may be retried. */
    uint32_t retry_after_ms;
    /** REPORT_ACCEPTED with report_terminal, or WORKFLOW_COMPLETED: the terminal report as sent,
     * step results included; its installed_update_id is NULL when longer than
     * AZ_IOT_SU_APPLIED_UPDATE_ID_SIZE. NULL otherwise. Valid only during the callback. */
    const az_iot_su_report* report;

    /** UPDATE_AVAILABLE only: the verified request (workflow id, file URLs).
     * Owned by the client; unlike the event, valid until the workflow ends: a
     * terminal az_iot_su_client_report_status(), or deinit. NULL otherwise. */
    const az_iot_su_client_update_request* request;
    /** UPDATE_AVAILABLE only: the verified manifest. Same lifetime as `request`;
     * `request->update_manifest` is its verified JSON text, for fields the SDK
     * does not parse. NULL otherwise. */
    const az_iot_su_client_update_manifest* manifest;
  } az_iot_su_event;

  /**
   * @brief Told when the software updates client raises an event.
   *
   * Called synchronously, from whichever call observed the change: a verdict
   * carried by an inbound message is delivered during
   * az_iot_connection_client_do_work(), one reached by the client's own
   * bookkeeping during az_iot_su_client_do_work(), and a workflow state
   * restored from persistence during az_iot_su_client_resume(). Do not assume
   * any one of them. */
  typedef void (*az_iot_su_observer_callback)(const az_iot_su_event* event, void* user_ctx);

  /** @brief Internal: one registered observer, in the workspace. */
  typedef struct az_iot_su_observer_registration
  {
    az_iot_su_observer_callback cb;
    void* user_ctx;
  } az_iot_su_observer_registration;

  /** @brief Internal: the strictest alignment of the workspace parts that hold pointers. */
  typedef union
  {
    void* pointer;
    void (*function)(void);
    uint64_t integer;
    size_t size;
  } az_iot_su_workspace_alignment;

/** @brief Largest az_iot_su_limits::max_root_keys and max_observers. */
#define AZ_IOT_SU_MAX_LIMIT_COUNT 1024

  /** @brief Internal: a copy of a terminal report handed to the channel. */
  typedef struct az_iot_su_sent_report_internal
  {
    bool valid;
    az_iot_su_outcome outcome;
    az_iot_su_failure_origin failure_origin;
    int32_t result_code;
    char workflow_id[AZ_IOT_SU_WORKFLOW_ID_SIZE + 1];
    char extended_result_codes[16];
    bool has_installed;
    char installed[AZ_IOT_SU_APPLIED_UPDATE_ID_SIZE]; /**< provider, name, version; NUL each */
    az_iot_su_step_result step_results[AZ_IOT_SU_MAX_STEP_COUNT];
    int32_t step_results_count;
  } az_iot_su_sent_report_internal;

  typedef struct az_iot_su_client
  {
    struct
    {
      struct
      {
        const struct az_iot_su_channel_vtable* vtable;
        void* ctx;
      } channel;

      /* Storage for the SDK-built channel. Opaque here: sized so the client
       * stays caller-allocated with no hidden allocation. */
      struct
      {
        void* pointers[24];
        uint8_t bytes[848];
        uint64_t alignment[4];
      } channel_storage;
      az_iot_su_platform_hooks hooks;
      const az_iot_crypto* crypto;

      /* Root-key store, in the workspace (az_iot_su_limits::max_root_keys entries). The
       * descriptors' key bytes reference caller arrays. */
      az_iot_su_root_key* root_keys;
      size_t root_key_count;

      az_iot_su_state state;

      /* Current deployment, parsed from the `updateMetadata` payload. */
      az_iot_su_client_update_request current_request;
      az_iot_su_client_update_manifest current_manifest;
      /** Workspace storage behind current_request.file_urls and current_manifest.related_files. */
      az_iot_su_client_file_url* file_url_storage;
      az_iot_su_client_update_manifest_file* related_file_storage;
      size_t max_related_files;
      bool have_request;
      uint32_t current_step;
      uint32_t current_file;
      /* A reboot checkpoint failed to persist; Apply waits until it succeeds. */
      bool checkpoint_pending;
      /* Storage is believed to hold a checkpoint this client wrote or resumed
       * from, so an invalidation write is owed when the workflow ends. */
      bool checkpoint_stored;
      /** Failed persist_workflow_state_fn writes since the last success, and when to retry. */
      az_iot_retry_state persist_retry;
      /** AZ_IOT_SU_PERSIST_MAX_ATTEMPTS, copied at initialization. */
      uint32_t persist_max_attempts;
      /** Return value of the last failed persist_workflow_state_fn write. */
      int32_t persist_last_error;
      /** The terminal record's write failed; further attempts wait for persist_retry. */
      bool terminal_write_failed;
      /** The stored checkpoint is the terminal-report record, not a workflow position. */
      bool checkpoint_terminal;
      /** The active workflow's terminal report is not yet accepted; kept durable until it is. */
      bool report_owed;
      /** The report last handed to the channel carries a terminal outcome. */
      bool terminal_report_in_flight;
      /** The stored checkpoint belongs to a superseded workflow; the new one waits for its clear.
       */
      bool checkpoint_superseded;
      /** The superseded record is still stored after its clear gave up; storage is behind
       * until a later write replaces or clears it. */
      bool superseded_record_stored;
      /** The stored checkpoint is the retry record of a FAILED workflow. */
      bool checkpoint_retry;

      /* Workflow id of the active (or last) deployment; a payload carrying it
       * is a redelivery and is ignored unless that workflow FAILED. Retry
       * timestamp and manifest CRC are not persisted and are
       * otherwise unused. */
      bool active_workflow_valid;
      /** The workflow named by active_workflow_id has started and not yet ended. */
      bool active_workflow_running;
      /** Retry schedule of active_workflow_id after it FAILED. */
      az_iot_retry_state workflow_retry;
      /** Unix time source, from the connection client options; NULL uses time(). */
      az_iot_unix_time_callback unix_time_fn;
      void* unix_time_ctx; /**< Passed to unix_time_fn. */
      uint8_t active_workflow_id[AZ_IOT_SU_WORKFLOW_ID_SIZE];
      size_t active_workflow_id_len;
      uint8_t active_retry_timestamp[AZ_IOT_SU_RETRY_TIMESTAMP_SIZE];
      size_t active_retry_timestamp_len;
      uint32_t active_manifest_crc;

      /* Workspace parts, laid out by init. The workflow record is serialized in
       * place around request_buffer: persist_blob is its header, then the
       * request, then the trailer. */
      /** COPY of the service payload backing current_request/current_manifest
       * spans (the channel's delivery buffer is gone after the callback). */
      uint8_t* request_buffer;
      size_t request_buffer_size; /**< az_iot_su_limits::max_update_size. */
      size_t request_len;
      uint8_t* persist_blob; /**< Workflow record and resume load buffer. */
      size_t persist_blob_size; /**< AZ_IOT_SU_STATE_BLOB_MAX_SIZE() of the limits. */
      uint8_t* terminal_blob; /**< Terminal record. */
      uint8_t* scratch; /**< Decode and verification scratch. */
      size_t scratch_size; /**< az_iot_su_limits::scratch_size. */

      /* The unescaped manifest text within request_buffer (decode_manifest
       * sets this; persistence/resume re-parses it). */
      az_span manifest_text;

      /* Accumulated result reported to the service. */
      az_iot_su_client_install_result install_result;
      az_iot_su_step_result step_results[AZ_IOT_SU_MAX_STEP_COUNT];
      int32_t step_results_count;

      /** Workspace part holding the copied property strings. */
      uint8_t* device_properties_buffer;
      size_t device_properties_buffer_size; /**< Size of device_properties_buffer. */
      /** Copied properties; strings point into device_properties_buffer. */
      az_iot_su_device_properties device_properties;
      /** Copied custom properties referenced by device_properties. */
      az_iot_su_custom_property custom_properties[AZ_IOT_SU_MAX_COMPATIBILITY_PROPERTIES];
      bool device_properties_report_pending; /**< A status report is due. */

      /* Which fetch the application asked for and the channel has not yet
       * accepted: 0 none, 1 onboarding, 2 regular. Not a bool, because a retry
       * must re-issue the route that was actually requested. */
      uint8_t pending_fetch;

      /* Which fetch the channel has accepted and not yet answered, with the
       * same encoding. Its deadline is pending_fetch_deadline_ms: the timeout
       * bounds the whole wait, not only the part before the channel accepts. */
      uint8_t fetch_in_flight;
      /** An update was delivered since fetch_in_flight was last issued. */
      bool fetch_delivered;
      /** The report last handed to the channel is terminal; kept until its verdict. */
      bool report_sent_terminal;

      /* When the pending fetch stops being retried, as a monotonic instant.
       *
       * WALL-CLOCK, and honoured absolutely: the caller asked for an answer
       * within N milliseconds, not for N milliseconds of some subset of the
       * wait. Time spent obeying a service-requested delay is NOT excluded --
       * excluding it would silently move the deadline the caller set and take
       * away its ability to plan.
       *
       * Covers the fetch whether it is queued (pending_fetch) or accepted and
       * awaiting its answer (fetch_in_flight).
       *
       * 0 means NO DEADLINE IS ARMED -- either nothing is pending, or the
       * caller passed timeout_ms = 0, which deliberately leaves a queued
       * request retrying indefinitely.
       *
       * The slot auto-retries: the channel refusing puts the request straight
       * back, so a request that can NEVER be served -- the device never
       * provisions, the enrollment is missing, DPS has no linked hub -- was
       * reissued for the life of the client with the application never told.
       * It could not tell that from "no update available". */
      uint64_t pending_fetch_deadline_ms;

      /* The timeout the caller passed for the pending fetch, so a request the
       * client re-arms itself after a retryable verdict keeps the caller's
       * policy instead of silently acquiring a new one. */
      uint32_t pending_fetch_timeout_ms;

      /* Terminal outcome for the active workflow, latched at the transition
       * that ends it. Reporting is keyed on workflowId, so the engine must be
       * able to distinguish SUCCEEDED / SKIPPED after the workflow
       * state itself has returned to Idle. */
      az_iot_su_outcome pending_outcome;

      /* The update id that was actually applied, captured BEFORE the return to
       * Idle clears the manifest. A successful report carries this, because
       * installedUpdateId means "what is installed now" — not "what this
       * workflow was about". Strings are packed into applied_update_id_buf. */
      char applied_update_id_buf[AZ_IOT_SU_APPLIED_UPDATE_ID_SIZE];
      az_iot_su_report_update_id applied_update_id;
      /** Installed update id adopted after a SUCCEEDED workflow; the cached
       * device properties point here, so it fits whatever their buffer size. */
      char adopted_installed_id_buf[AZ_IOT_SU_APPLIED_UPDATE_ID_SIZE];
      bool applied_update_id_valid;

      /* Connection-state observer / detach safety (see design doc §16). */
      bool detached;

      /** Library mode: verified updates are handed to the application. */
      bool library_mode;
      /** az_iot_su_client_config_options::download_handlers, referenced. */
      const az_iot_su_download_handler* download_handlers;
      size_t download_handler_count;
      /** The hand-off events are being dispatched. */
      bool delegating;
      /** A terminal report arrived during the hand-off; applied after it. */
      bool delegated_end_pending;
      /** Failure origin of a FAILED report; NOT_APPLICABLE means AGENT_CORE. */
      az_iot_su_failure_origin pending_failure_origin;

      /* Application observers, in the workspace (max_observers entries), and the guard that
       * keeps the array stable while it is being walked. */
      az_iot_su_observer_registration* observers;
      size_t max_observers;
      bool dispatching;

      /** Backoff after retryable verdicts with no service delay; reset by any accepted operation.
       */
      az_iot_retry_state retry;
      /** Jitter PRNG state for retry, persist_retry and workflow_retry. */
      uint64_t retry_rng;
      /** pending_fetch was re-armed by a verdict that armed retry. */
      bool pending_fetch_paced;
      /** The pending report was re-armed by a verdict that armed retry. */
      bool report_paced;
      /** install_fn was called in the current workflow. */
      bool install_called;
      /** The last apply asked for a reboot; cleared by the reboot (re-initialization). */
      bool reboot_owed;
      /** install_fn or apply_fn of the current workflow asked for a reboot; cleared on FAILED. */
      bool reboot_requested;
      /** An earlier workflow in this process asked for a reboot that has not happened. */
      bool earlier_reboot_requested;
      /** Route of the application's last update request (pending_fetch encoding); 0 if none. */
      uint8_t last_requested_fetch;
      /** Terminal reports handed to the channel, for the REPORT_ACCEPTED event:
       * the engine state may move on before the verdict arrives. Slot
       * sent_terminal_slot is the latest; the other is restored if the channel
       * refuses a newer one. */
      az_iot_su_sent_report_internal sent_terminal[2];
      uint8_t sent_terminal_slot; /**< Index of the latest sent_terminal entry. */
      /* Kept last: earlier offsets stay unchanged. */
      uint8_t* step_details; /**< result_details_size bytes per step. */
      uint8_t* sent_details; /**< Two sent_terminal slots of result_details_size bytes per step. */
      size_t step_details_size; /**< az_iot_su_limits::result_details_size. */
      /** Overall result details, NUL-terminated: result_details_size + 1 bytes. */
      uint8_t* overall_details;
    } _internal;
  } az_iot_su_client;

  /* --- Lifecycle ----------------------------------------------------------- */

  /**
   * @brief What a new workflow id does to a workflow that may not be replaced.
   *
   * A workflow may be replaced until its first install_fn call. From then
   * until it ends (Idle or Failed), and after a last apply that asked for a
   * reboot until that reboot, the device holds a partly applied or not yet
   * activated deployment, which is validated only as a whole.
   */
  typedef enum az_iot_su_replacement_policy
  {
    /** Default. The new workflow is not started and AZ_IOT_SU_EVENT_UPDATE_DEFERRED
     * is raised; the current one runs to its end. */
    AZ_IOT_SU_REPLACEMENT_DEFER = 0,
    /** Roll back the current workflow with restore_fn, report it FAILED with
     * AZ_IOT_SU_FACILITY_SUPERSEDED, then start the new one. Requires restore_fn.
     * Not yet supported: az_iot_su_client_init() returns AZ_IOT_ERR_NOT_SUPPORTED. */
    AZ_IOT_SU_REPLACEMENT_ROLLBACK = 1
  } az_iot_su_replacement_policy;

  /**
   * @brief Sizes of the parts of the application-provided workspace.
   *
   * Runtime values: the client struct does not depend on them.
   */
  typedef struct az_iot_su_limits
  {
    /** Largest update metadata accepted, in bytes; a larger one raises
     * AZ_IOT_SU_EVENT_UPDATE_REFUSED. Also bounds the persisted state:
     * AZ_IOT_SU_STATE_BLOB_MAX_SIZE(). At least 256. */
    size_t max_update_size;
    /** Decode and signature-verification scratch, in bytes. A signature part or file
     * URL that does not fit is refused. At least 256. */
    size_t scratch_size;
    /** Largest update-check or status-report body, in bytes. At least
     * AZ_IOT_SU_MIN_MESSAGE_SIZE. */
    size_t max_message_size;
    /** Device-property strings, in bytes; see az_iot_su_device_properties_buffer_size(). */
    size_t device_properties_size;
    /** Bytes of resultDetails kept per step; see az_iot_su_client_set_step_result_details().
     * At most AZ_IOT_SU_MAX_RESULT_DETAILS_SIZE. */
    size_t result_details_size;
    /** Most root keys az_iot_su_client_config_options::root_keys may hold. At most
     * AZ_IOT_SU_MAX_LIMIT_COUNT. */
    size_t max_root_keys;
    /** Most observers az_iot_su_client_add_observer() can register. At most
     * AZ_IOT_SU_MAX_LIMIT_COUNT. */
    size_t max_observers;
    /** Most related files (`relatedFiles`, e.g. deltas) kept from a manifest; more are left out,
     * so their files are downloaded in full. Also allows as many more download URLs, see
     * AZ_IOT_SU_FILE_URL_CAPACITY(). 0 keeps none. At most AZ_IOT_SU_MAX_LIMIT_COUNT. */
    size_t max_related_files;
  } az_iot_su_limits;

/**
 * @brief Workspace bytes for the given az_iot_su_limits fields, as a constant expression for
 * static allocation. Equal to az_iot_su_workspace_size() for valid limits.
 */
#define AZ_IOT_SU_WORKSPACE_SIZE(                                                              \
    max_update_size,                                                                           \
    scratch_size,                                                                              \
    max_message_size,                                                                          \
    device_properties_size,                                                                    \
    result_details_size,                                                                       \
    max_root_keys,                                                                             \
    max_observers,                                                                             \
    max_related_files)                                                                         \
  ((size_t)(max_update_size) + (size_t)(scratch_size) + (size_t)(max_message_size)             \
   + 2u * (size_t)(device_properties_size) + (size_t)AZ_IOT_SU_APPLIED_UPDATE_ID_SIZE          \
   + 2u * (size_t)AZ_IOT_SU_PERSIST_OVERHEAD                                                   \
   + 5u * (size_t)(AZ_IOT_SU_MAX_STEP_COUNT) * (size_t)(result_details_size)                   \
   + (size_t)(result_details_size) + 1u + (size_t)(max_root_keys) * sizeof(az_iot_su_root_key) \
   + (size_t)(max_observers) * sizeof(az_iot_su_observer_registration)                         \
   + AZ_IOT_SU_FILE_URL_CAPACITY(max_related_files) * sizeof(az_iot_su_client_file_url)        \
   + (size_t)(max_related_files)                                                               \
       * (sizeof(az_iot_su_client_update_manifest_file)                                        \
          + (size_t)AZ_IOT_SU_PERSIST_URL_RECORD_SIZE)                                         \
   + sizeof(az_iot_su_workspace_alignment) - 1u)

/** @brief Workspace bytes for az_iot_su_limits_default(). */
#define AZ_IOT_SU_WORKSPACE_DEFAULT_SIZE       \
  AZ_IOT_SU_WORKSPACE_SIZE(                    \
      AZ_IOT_SU_DEFAULT_MAX_UPDATE_SIZE,       \
      AZ_IOT_SU_VERIFY_SCRATCH_SIZE,           \
      AZ_IOT_SU_DEFAULT_MAX_MESSAGE_SIZE,      \
      AZ_IOT_SU_DEVICE_PROPERTIES_BUFFER_SIZE, \
      AZ_IOT_SU_DEFAULT_RESULT_DETAILS_SIZE,   \
      AZ_IOT_SU_DEFAULT_MAX_ROOT_KEYS,         \
      AZ_IOT_SU_DEFAULT_MAX_OBSERVERS,         \
      AZ_IOT_SU_DEFAULT_MAX_RELATED_FILES)

  /**
   * @brief Returns the default limits: AZ_IOT_SU_DEFAULT_MAX_UPDATE_SIZE,
   * AZ_IOT_SU_VERIFY_SCRATCH_SIZE, AZ_IOT_SU_DEFAULT_MAX_MESSAGE_SIZE,
   * AZ_IOT_SU_DEVICE_PROPERTIES_BUFFER_SIZE, AZ_IOT_SU_DEFAULT_RESULT_DETAILS_SIZE,
   * AZ_IOT_SU_DEFAULT_MAX_ROOT_KEYS, AZ_IOT_SU_DEFAULT_MAX_OBSERVERS and
   * AZ_IOT_SU_DEFAULT_MAX_RELATED_FILES.
   *
   * @return The default limits.
   */
  AZ_NODISCARD az_iot_su_limits az_iot_su_limits_default(void);

  /**
   * @brief Workspace bytes az_iot_su_client_init() needs for @p limits.
   *
   * @param[in] limits Limits to size.
   * @return Bytes; 0 if @p limits is NULL or a limit is out of range.
   */
  AZ_NODISCARD size_t az_iot_su_workspace_size(const az_iot_su_limits* limits);

  /**
   * Configuration for az_iot_su_client_init(). Obtain an instance from
   * az_iot_su_client_config_options_default() and set the required fields
   * before calling initialize.
   */
  typedef struct az_iot_su_client_config_options
  {
    /** Platform operations (download, install, apply, ...). Required unless
     * library_mode, which ignores it. */
    const az_iot_su_platform_hooks* hooks;
    /** Library mode: hand each verified update to the application
     * (AZ_IOT_SU_EVENT_UPDATE_AVAILABLE) instead of installing it, and send
     * what it reports with az_iot_su_client_report_status(). No platform hook is
     * called, so nothing is persisted: a restart forgets the update, which the
     * service offers again. root_keys is then required. */
    bool library_mode;
    /** RSA root keys that anchor manifest trust. The descriptors are copied; the
     * key bytes are referenced and must outlive the client. For Microsoft-signed
     * updates pass az_iot_su_microsoft_root_keys(). */
    const az_iot_su_root_key* root_keys;
    size_t root_key_count; /**< Entries in root_keys; at most limits.max_root_keys. */
    /** Required. Deep-copied; may be changed or freed after initialize. */
    const az_iot_su_device_properties* device_properties;
    /** Sizes of the workspace parts. Default az_iot_su_limits_default(). */
    az_iot_su_limits limits;
    /** Required. The client's memory: update copy, persisted-state record, scratch, message
     * body and device-property strings. At least az_iot_su_workspace_size(&limits) bytes, no
     * alignment needed; must outlive the client. */
    uint8_t* workspace;
    size_t workspace_size; /**< Size of workspace. */
    /** What a new workflow id does to a workflow that may not be replaced.
     * Default AZ_IOT_SU_REPLACEMENT_DEFER. */
    az_iot_su_replacement_policy replacement_policy;
    /** Download handlers, with distinct ids. Referenced, not copied: must outlive the client.
     * Need hooks->read_file_fn; not allowed with library_mode. */
    const az_iot_su_download_handler* download_handlers;
    size_t download_handler_count; /**< Entries in download_handlers. */
  } az_iot_su_client_config_options;

  /**
   * @brief Returns options with az_iot_su_limits_default() and every other field zero.
   *
   * Set hooks (unless library_mode), root_keys, root_key_count, device_properties, workspace and
   * workspace_size before az_iot_su_client_init().
   *
   * @return The options.
   */
  AZ_NODISCARD az_iot_su_client_config_options az_iot_su_client_config_options_default(void);

  /**
   * @brief Build-time view of az_iot_su_client, filled by az_iot_su_client_init() in the
   * application's build.
   */
  typedef struct az_iot_su_client_layout
  {
    size_t layout_size; /**< sizeof(az_iot_su_client_layout). */
    size_t client_size; /**< sizeof(az_iot_su_client). */
    size_t max_step_count; /**< AZ_IOT_SU_MAX_STEP_COUNT. */
    size_t max_file_count; /**< AZ_IOT_SU_MAX_FILE_COUNT. */
    size_t max_file_count_per_step; /**< AZ_IOT_SU_MAX_FILE_COUNT_PER_STEP. */
  } az_iot_su_client_layout;

  /**
   * @brief Exported initializer behind az_iot_su_client_init(); call that instead.
   *
   * @param[in] layout az_iot_su_client as compiled where it is allocated. If any
   *   field differs from the SDK build (or layout is NULL), the client is not touched.
   * @return See az_iot_su_client_init().
   */
  AZ_NODISCARD az_iot_result az_iot_su_client_init_checked(
      az_iot_su_client* client,
      az_iot_connection_client* connection,
      const az_iot_su_client_config_options* options,
      const az_iot_su_client_layout* layout);

  /**
   * @brief Initializes the software updates client.
   *
   * Inline, so the SDK can check that az_iot_su_client was compiled with the
   * same manifest limits (AZ_IOT_SU_MAX_*) in the application as in the SDK (see
   * az_iot_su_client_layout).
   *
   * @param[out] client Client to initialize.
   * @param[in] connection Connection client the device-update channel is built
   *   on. Need not be connected, but its DPS ID scope, registration ID and
   *   credential must be set: an application-requested onboarding check can
   *   run before registration. Initialization sends nothing. Its crypto
   *   backend (az_iot_connection_client_options::crypto) verifies updates.
   * @param[in] options Hooks, trust store, device properties and cache.
   * @return AZ_IOT_OK on success.
   * @retval AZ_IOT_ERR_INVALID_ARG A required field is NULL, the connection
   *   client has no crypto backend, or the device properties are malformed
   *   (including zero compatibility properties), replacement_policy is not
   *   an az_iot_su_replacement_policy value, a limit is out of range, or a download handler is
   *   invalid (NULL id or hook, bad or duplicate id, no read_file_fn, library_mode). Also
   *   returned, with an error log and the client untouched, when any
   *   az_iot_su_client_layout field differs between the application and SDK builds.
   * @retval AZ_IOT_ERR_NOT_SUPPORTED The crypto backend has no verify_rs256, or
   *   replacement_policy is AZ_IOT_SU_REPLACEMENT_ROLLBACK.
   * @retval AZ_IOT_ERR_NOT_ENOUGH_SPACE root_key_count exceeds
   *   limits.max_root_keys, the properties exceed the count limit or
   *   AZ_IOT_SU_APPLIED_UPDATE_ID_SIZE, the device_properties_size or max_message_size limit is
   *   too small
   *   for them, or the workspace is smaller than az_iot_su_workspace_size().
   */
  AZ_NODISCARD AZ_INLINE az_iot_result az_iot_su_client_init(
      az_iot_su_client* client,
      az_iot_connection_client* connection,
      const az_iot_su_client_config_options* options)
  {
    const az_iot_su_client_layout layout = {
      sizeof(az_iot_su_client_layout),           sizeof(az_iot_su_client),
      (size_t)AZ_IOT_SU_MAX_STEP_COUNT,          (size_t)AZ_IOT_SU_MAX_FILE_COUNT,
      (size_t)AZ_IOT_SU_MAX_FILE_COUNT_PER_STEP,
    };
    return az_iot_su_client_init_checked(client, connection, options, &layout);
  }

  /**
   * Return Microsoft's compiled-in software updates root public keys (const, static storage).
   * Convenience for the common case; equivalent to passing your own array to
   * az_iot_su_client_init().
   */
  const az_iot_su_root_key* az_iot_su_microsoft_root_keys(size_t* out_count);

  void az_iot_su_client_deinit(az_iot_su_client* client);

  /**
   * Resume a workflow after device reboot. The application SHOULD call this during
   * startup, before requesting an update check. If no persisted state exists,
   * this is a no-op. A record holding an unsent terminal report restores it
   * (the client stays Idle) and re-sends it from do_work().
   *
   * @return AZ_IOT_OK if resumed or nothing usable was persisted (including a
   *   record of another format version); AZ_IOT_ERR_NOT_SUPPORTED for a valid
   *   record of this format when persist_workflow_state_fn is NULL (it could never be cleared);
   *   AZ_IOT_ERR_INVALID_ARG for a NULL client or
   *   a record whose download URLs do not cover the remaining steps;
   *   AZ_IOT_ERR_BUSY when a workflow is in progress (the record is loaded over its
   *   update copy); AZ_IOT_ERR_DETACHED if the client is detached.
   */
  AZ_NODISCARD az_iot_result az_iot_su_client_resume(az_iot_su_client* client);

  /* --- Runtime ------------------------------------------------------------- */

  /**
   * Drive the software updates state machine. The application MUST call this from its do_work
   * loop. Non-blocking: processes at most one chunk of work per invocation. Not
   * AZ_NODISCARD: a pump whose result is typically observed via state, not return.
   */
  az_iot_result az_iot_su_client_do_work(az_iot_su_client* client);

  /**
   * @brief Sets the resultDetails reported for one step of the workflow in progress.
   *
   * Call from a platform hook, or between do_work() calls, while the workflow runs. The
   * text is copied, persisted with the step's result and reported with the terminal
   * result; an empty @p details clears it. The client sets the step's outcome and codes.
   * When the report would not fit az_iot_su_limits::max_message_size it is sent without
   * step details. A change made while the workflow waits for a reboot is written by a later
   * do_work() through persist_workflow_state_fn before the workflow goes on. A failed write raises
   * AZ_IOT_SU_EVENT_PERSIST_FAILED and is retried; a reboot before it succeeds
   * (AZ_IOT_SU_EVENT_PERSIST_RECOVERED) restores the previous details.
   *
   * @param[in,out] client The client.
   * @param[in] step_index Manifest step index, as passed to the hooks.
   * @param[in] details Human-readable text; at most az_iot_su_limits::result_details_size
   *   bytes.
   * @return AZ_IOT_OK; AZ_IOT_ERR_INVALID_ARG on a NULL client; AZ_IOT_ERR_NOT_FOUND
   *   when no workflow is past manifest verification or @p step_index is not one of its
   *   steps; AZ_IOT_ERR_NOT_ENOUGH_SPACE when @p details is longer than
   *   result_details_size; AZ_IOT_ERR_DETACHED if the client is detached.
   */
  az_iot_result az_iot_su_client_set_step_result_details(
      az_iot_su_client* client,
      uint32_t step_index,
      az_span details);

  /** Get the current software updates agent state. */
  az_iot_su_state az_iot_su_client_get_state(const az_iot_su_client* client);

  /**
   * @brief Get the update the client is working on, in either mode.
   *
   * The request (`workflowId`, the manifest text, its signature, the file URLs)
   * is available from AZ_IOT_SU_STATE_MANIFEST_RECEIVED until the client returns
   * to Idle; `request->update_manifest` is always the unescaped text the
   * signature covers. It is not verified before the state leaves
   * AZ_IOT_SU_STATE_VERIFYING_MANIFEST. After az_iot_su_client_resume() the
   * signature and retry timestamp are empty.
   *
   * The parsed manifest is available once verified, until the workflow ends;
   * not in AZ_IOT_SU_STATE_FAILED. In library mode both are the ones
   * AZ_IOT_SU_EVENT_UPDATE_AVAILABLE carries.
   *
   * Both are owned by the client and valid until it returns to Idle, a new
   * workflow replaces this one (managed client only; see
   * AZ_IOT_SU_EVENT_WORKFLOW_STATE_CHANGED), or it is deinitialized. Call on the
   * do_work() thread, or serialize with it; legal from an observer.
   *
   * @param[in] client The client.
   * @param[out] out_request The current request; NULL on error.
   * @param[out] out_manifest The verified manifest, or NULL when there is none yet.
   * @return AZ_IOT_OK when a workflow is in progress.
   * @retval AZ_IOT_ERR_INVALID_ARG A NULL argument.
   * @retval AZ_IOT_ERR_NOT_FOUND No workflow is in progress.
   */
  AZ_NODISCARD az_iot_result az_iot_su_client_get_current_update(
      const az_iot_su_client* client,
      const az_iot_su_client_update_request** out_request,
      const az_iot_su_client_update_manifest** out_manifest);

  /**
   * @brief Library mode: report the application's progress or outcome for the
   *        delegated update.
   *
   * Reported for the delegated `workflowId`, with the managed client's pacing,
   * retries and abandonment (AZ_IOT_SU_EVENT_OPERATION_ABANDONED for
   * AZ_IOT_SU_OP_REPORT_STATUS). A terminal outcome ends the workflow: the client
   * returns to Idle and the UPDATE_AVAILABLE event's `request` / `manifest` become
   * invalid. Until then a later call replaces the earlier one.
   *
   * The wire result code is 1 for IN_PROGRESS and 700 for SUCCEEDED; otherwise
   * `result->result_code`, or -1 when it is 0. `failure_origin` MUST be
   * NOT_APPLICABLE unless the outcome is FAILED, and then it MUST NOT be. A FAILED
   * result code must not be positive, and CANCELED (the service's) is refused.
   * Step results are ignored for IN_PROGRESS;
   * otherwise there is exactly one per manifest step, none IN_PROGRESS. On
   * SUCCEEDED the manifest's update ID is reported as installed; otherwise the
   * cached installed update ID.
   *
   * Overall `result_details` are sent with this report only; the workflow's
   * step details can be set earlier with az_iot_su_client_set_step_result_details().
   * A report that does not fit az_iot_su_limits::max_message_size is sent
   * without either.
   *
   * Call on the do_work() thread, or serialize with it. Legal from an
   * observer; a terminal report made during the hand-off events takes effect
   * once every observer has had them, and later calls get NOT_FOUND.
   *
   * @param[in,out] client Library-mode client.
   * @param[in] result Overall result. `result_details` is copied and must not
   *   contain a NUL byte.
   * @param[in] step_results Per-step results, in manifest step order; copied.
   *   May be NULL when @p step_results_count is 0.
   * @param[in] step_results_count Entries in @p step_results.
   * @return AZ_IOT_OK when recorded; the report is sent on this call or a later
   *   do_work(). On failure nothing changes.
   * @retval AZ_IOT_ERR_INVALID_ARG Bad arguments, an invalid result, or a
   *   terminal step count other than the manifest's.
   * @retval AZ_IOT_ERR_NOT_SUPPORTED The client is not in library mode.
   * @retval AZ_IOT_ERR_NOT_FOUND No update is delegated to the application.
   * @retval AZ_IOT_ERR_NOT_ENOUGH_SPACE Any details longer than
   *   az_iot_su_limits::result_details_size.
   * @retval AZ_IOT_ERR_DETACHED The client is detached.
   */
  AZ_NODISCARD az_iot_result az_iot_su_client_report_status(
      az_iot_su_client* client,
      const az_iot_su_step_result* result,
      const az_iot_su_step_result* step_results,
      int32_t step_results_count);

  /** @brief Whether the device may reboot (or the process restart) now. */
  typedef enum
  {
    /** Storage is current and no reboot was asked for in this process (or the
     * workflow that asked failed). */
    AZ_IOT_SU_REBOOT_NONE = 0,
    /** Storage is behind what az_iot_su_client_resume() needs: a
     * persist_workflow_state_fn write is failing or being retried. A restart now could
     * lose or repeat work, whether or not a reboot was asked for. Stays so
     * after the client gives up retrying (PERSIST_FAILED with
     * `persist_retrying` false) until a later write succeeds. */
    AZ_IOT_SU_REBOOT_PENDING,
    /** install_fn or apply_fn asked for a reboot and storage is current:
     * reboot now. */
    AZ_IOT_SU_REBOOT_READY,
  } az_iot_su_reboot_state;

  /**
   * @brief Whether a hook asked for a reboot, and whether storage allows a restart now.
   *
   * Check after each az_iot_su_client_do_work(): the next call continues the
   * workflow as if the device had not rebooted. Also use it before a planned
   * exit or shutdown: PENDING means storage is not current yet. With no
   * persist_workflow_state_fn there is nothing to store, so it is never PENDING (and a
   * workflow does not survive a restart).
   *
   * @param[in] client Initialized client.
   * @return AZ_IOT_SU_REBOOT_NONE if @p client is NULL or detached.
   */
  AZ_NODISCARD az_iot_su_reboot_state
  az_iot_su_client_get_reboot_state(const az_iot_su_client* client);

  /**
   * @brief Start being told about az_iot_su_event. Every observer receives
   * every event; read `kind` to see which fields carry meaning.
   *
   * Idempotent on the (cb, user_ctx) PAIR, not on cb alone: one callback shared
   * by two owners is two subscriptions and is delivered twice.
   *
   * An observer MUST NOT add an observer, and MUST NOT deinit the client --
   * both mutate the array being walked, and deinit frees the array
   * itself. Adding answers AZ_IOT_ERR_BUSY during a dispatch. REMOVING is
   * permitted and must be: an owner torn down in reaction to an event has to
   * give its seat back before its storage goes away. Calling
   * az_iot_su_client_request_update() or _request_onboarding_update() from an
   * observer IS supported.
   *
   * Single-threaded contract: MUST be called on the do_work thread or be
   * externally serialized with do_work(). It mutates an array both pumps read
   * -- an event can be delivered from either az_iot_su_client_do_work() or
   * az_iot_connection_client_do_work() -- so an unsynchronized call is a data
   * race.
   *
   * Returns AZ_IOT_ERR_INVALID_ARG on a NULL client or NULL cb,
   * AZ_IOT_ERR_NOT_ENOUGH_SPACE when az_iot_su_limits::max_observers are already
   * registered, and AZ_IOT_ERR_BUSY when called from inside an observer.
   *
   * @param[in] client   The software updates client.
   * @param[in] cb       Callback to register. MUST NOT be NULL.
   * @param[in] user_ctx Opaque context passed back to @p cb.
   */
  az_iot_result az_iot_su_client_add_observer(
      az_iot_su_client* client,
      az_iot_su_observer_callback cb,
      void* user_ctx);

  /**
   * @brief Stop being told. Matches on the (cb, user_ctx) pair, so one
   * callback registered with two contexts can be withdrawn one at a time.
   *
   * Legal from inside an observer, and that case is the reason it must be: an
   * owner destroyed in reaction to an event releases the storage the entry
   * points at, so it has to withdraw before it returns.
   *
   * Single-threaded contract, the same as adding: MUST be called on the
   * do_work thread or be externally serialized with do_work(). It mutates the
   * array both pumps read. "Legal from inside an observer" means legal during
   * a SYNCHRONOUS dispatch on that thread -- it does not make the call
   * thread-safe.
   *
   * Returns AZ_IOT_ERR_INVALID_ARG on a NULL client or NULL cb, and
   * AZ_IOT_ERR_NOT_FOUND when that pair is not registered.
   *
   * @param[in] client   The software updates client.
   * @param[in] cb       Callback to withdraw. MUST NOT be NULL.
   * @param[in] user_ctx The context it was registered with.
   */
  az_iot_result az_iot_su_client_remove_observer(
      az_iot_su_client* client,
      az_iot_su_observer_callback cb,
      void* user_ctx);

  /**
   * @brief Ask for an ONBOARDING update — the day-0/pre-registration route.
   *
   * Use this while the device has no device record with the service yet. It is
   * the permissive route: it needs no registry entry. Like the regular route, it
   * sends `installedUpdateId` when the device properties set one, so the
   * service does not offer what is already installed; it is omitted otherwise.
   *
   * The application chooses the route because it is the only party that knows:
   * it persists its provisioning result across boots, while the SDK sees only
   * the current process. The service cannot be asked either — "no device
   * record" and "malformed request" share one error code, so probing would
   * mask real errors.
   *
   * Asynchronous. Records the request; the NEXT az_iot_su_client_do_work()
   * issues it, and retries on a later tick if the channel is not ready. The
   * result arrives through the engine, not this return value. Returns
   * AZ_IOT_ERR_INVALID_ARG if @p client is NULL.
   *
   * There is ONE pending slot, and the newest request wins. Calling either
   * request function twice before do_work() does NOT queue two fetches: the
   * second replaces the first, and only the second is issued. A request made
   * while an earlier one is still in flight likewise replaces whatever the
   * engine would otherwise have retried.
   *
   * @p timeout_ms bounds the WHOLE wait, in wall-clock milliseconds: if the
   * check has not been answered by then, the request is dropped and
   * AZ_IOT_SU_EVENT_OPERATION_ABANDONED is raised with
   * `reason = AZ_IOT_ERR_TIMEOUT`. Time the device spends obeying a
   * service-requested delay counts against it like any other -- the deadline
   * the caller set is the deadline that is kept, so the caller can plan around
   * it. If the service asks for a delay that cannot fit, the request is
   * abandoned AT ONCE rather than at the deadline, and the event carries
   * `service_error.retry_after_ms` so the caller can decide when to ask again.
   *
   * A retryable failure that names no delay is retried by the client after a
   * jittered exponential backoff (1 s doubling to 60 s, +/-20%), reset by any
   * accepted operation; status reports are paced the same way. The backoff
   * counts against @p timeout_ms. A new request is not held by it.
   *
   * While a workflow's terminal report is owed, the check waits for that
   * report's verdict, so the service sees the outcome first; the report is
   * sent again if nothing else will send it. The wait counts against
   * @p timeout_ms. If that report loses its session for good, the check is
   * abandoned with AZ_IOT_ERR_NOT_CONNECTED; the report stays owed.
   *
   * Pass AZ_IOT_SU_REQUEST_NO_TIMEOUT for no bound: the request is then
   * retried indefinitely and the only abandonment is a channel verdict.
   * AZ_IOT_SU_REQUEST_DEFAULT_TIMEOUT_MS is available for callers with no
   * policy of their own.
   *
   * @param[in] client   The software updates client.
   * @param[in] timeout_ms Wall-clock bound in milliseconds;
   *                     AZ_IOT_SU_REQUEST_NO_TIMEOUT for none.
   * @return AZ_IOT_OK when the request is recorded, AZ_IOT_ERR_INVALID_ARG if
   *         @p client is NULL.
   *
   * Single-threaded contract: MUST be called on the do_work thread or be
   * externally serialized with do_work().
   */
  AZ_NODISCARD az_iot_result
  az_iot_su_client_request_onboarding_update(az_iot_su_client* client, uint32_t timeout_ms);

  /**
   * @brief Ask for a REGULAR (software) update — the operational route.
   *
   * Use this once the device is provisioned and has a device record. It sends
   * `installedUpdateId` when set, which is how the service knows what to offer
   * next.
   *
   * Calling it on a device that has no device record yet is rejected by the
   * service as an invalid request; use
   * az_iot_su_client_request_onboarding_update() until then.
   *
   * Asynchronous, with the same contract as
   * az_iot_su_client_request_onboarding_update().
   */
  AZ_NODISCARD az_iot_result
  az_iot_su_client_request_update(az_iot_su_client* client, uint32_t timeout_ms);

  /**
   * @brief Replaces the cached device properties.
   *
   * Deep-copies @p device_properties. No I/O, and no update check is scheduled:
   * the next az_iot_su_client_request_update() or
   * az_iot_su_client_request_onboarding_update() sends them. While a workflow
   * runs or its terminal report is owed, also marks a workflow-status report
   * due on do_work(); once that report is settled, it is not sent again. That
   * report never carries the compatibility properties, but carries the new
   * installed update ID unless an applied update succeeded.
   *
   * Not needed after a SUCCEEDED workflow: the client then adopts the applied
   * update ID as installed by itself (also when az_iot_su_client_resume()
   * restores its terminal record). Call it when the installed software changes
   * otherwise.
   *
   * Call on the do_work() thread, or serialize with it.
   *
   * @param[in,out] client Initialized client.
   * @param[in] device_properties New properties; may be changed or freed after return.
   * @return AZ_IOT_OK on success. On failure the previous properties and pending
   *   work are unchanged.
   * @retval AZ_IOT_ERR_INVALID_ARG Malformed properties, or strings pointing into the client's
   *   copy of them.
   * @retval AZ_IOT_ERR_NOT_ENOUGH_SPACE The count limit, AZ_IOT_SU_APPLIED_UPDATE_ID_SIZE,
   *   az_iot_su_limits::device_properties_size or the update-check body capacity would be
   *   exceeded. The body is sized with the longest installed ID, fully escaped, so a later
   *   adoption of the applied update ID always fits. For AZ_IOT_SU_APPLIED_UPDATE_ID_SIZE and
   * device_properties_size, an ERROR log gives the bytes needed.
   */
  AZ_NODISCARD az_iot_result az_iot_su_client_update_device_properties(
      az_iot_su_client* client,
      const az_iot_su_device_properties* device_properties);

  /* --- Transport-free primitives ------------------------------------------ */
  /*
   * The functions below let a caller build their OWN software updates agent on top of the
   * SDK's vetted parse + trust + report code, WITHOUT adopting the managed state
   * machine, a channel, or any transport. They take spans/structs only, perform no
   * hidden allocation, and (where they verify) are fail-closed. The managed
   * az_iot_su_client is implemented in terms of the same internal cores, so both
   * modes share one copy of the security-critical path. See
   * docs/eng/software-updates.md §5.3. For an agent that still wants the SDK's channel,
   * see az_iot_su_client_config_options::library_mode.
   */

  /**
   * Streaming read callback used by az_iot_su_verify_file_hash(). Read up to
   * @p buffer_size bytes starting at @p offset into @p buffer and set
   * @p out_read to the number of bytes produced (0 signals end-of-file). MUST
   * return AZ_IOT_SU_RESULT_SUCCESS on a successful read (including the final
   * 0-byte read at end-of-file); any other value is treated as a read error.
   */
  typedef int32_t (*az_iot_su_read_chunk_callback)(
      size_t offset,
      uint8_t* buffer,
      size_t buffer_size,
      size_t* out_read,
      void* read_ctx);

  /** @brief Caller storage for the arrays az_iot_su_parse_update_request() fills. */
  typedef struct az_iot_su_update_request_storage
  {
    /** AZ_IOT_SU_FILE_URL_CAPACITY(max_related_files) entries. */
    az_iot_su_client_file_url* file_urls;
    /** max_related_files entries; may be NULL when max_related_files is 0. */
    az_iot_su_client_update_manifest_file* related_files;
    /** Related files to keep; see az_iot_su_limits::max_related_files. At most
     * AZ_IOT_SU_MAX_LIMIT_COUNT. */
    size_t max_related_files;
  } az_iot_su_update_request_storage;

  /**
   * @brief Verify a manifest's signature chain without a client.
   *
   * The check the client makes before it trusts a manifest: JWS/SJWK, root-key
   * `kid`, RS256, both RSA checks, SHA-256 binding. Nothing is parsed or
   * modified. Uses AZ_IOT_SU_VERIFY_SCRATCH_SIZE bytes of stack.
   *
   * @param[in] manifest The unescaped manifest text the signature covers, as in
   *   az_iot_su_client_update_request::update_manifest.
   * @param[in] signature The compact `updateManifestSignature`.
   * @param[in] crypto Backend; needs verify_rs256.
   * @param[in] root_keys Trusted root keys, e.g. az_iot_su_microsoft_root_keys().
   * @param[in] root_key_count Entries in @p root_keys.
   * @param[out] out_error Why it failed: an AZ_IOT_SU_MANIFEST_ERROR_* sub-code,
   *   reported as AZ_IOT_SU_EXTENDED_RESULT(AZ_IOT_SU_FACILITY_MANIFEST, code);
   *   0 otherwise. May be NULL.
   * @return AZ_IOT_OK when the manifest verifies.
   * @retval AZ_IOT_ERR_AUTH The manifest or its signature is rejected.
   * @retval AZ_IOT_ERR_INTERNAL The crypto backend failed
   *   (AZ_IOT_SU_MANIFEST_ERROR_CRYPTO); not a rejection.
   * @retval AZ_IOT_ERR_INVALID_ARG An invalid @p crypto, or NULL @p root_keys
   *   with a non-zero @p root_key_count.
   * @retval AZ_IOT_ERR_NOT_SUPPORTED @p crypto has no verify_rs256.
   */
  AZ_NODISCARD az_iot_result az_iot_su_verify_manifest(
      az_span manifest,
      az_span signature,
      const az_iot_crypto* crypto,
      const az_iot_su_root_key* root_keys,
      size_t root_key_count,
      uint32_t* out_error);

  /**
   * @brief Verify and parse an `updateMetadata` object without a client.
   *
   * Verifies the manifest with az_iot_su_verify_manifest(), then parses it.
   * Fail-closed: non-NULL outputs are zeroed on every error.
   *
   * @param request_json   `{ workflowId, updateManifest, updateManifestSignature,
   *                       fileUrls }` as the service sends it. The
   *                       `workflowId`, `updateManifest` and every `fileUrls` id
   *                       and URL are decoded in place, overwriting those string
   *                       values (also on failure), so the buffer must be
   *                       writable and outlive both outputs, whose spans point
   *                       into it.
   * @param crypto         Backend; needs verify_rs256.
   * @param root_keys      Trusted root keys, e.g. az_iot_su_microsoft_root_keys().
   * @param root_key_count Entries in @p root_keys.
   * @param storage        Arrays behind the outputs' file URLs and related files; must outlive
   *                       them.
   * @param out_request    Workflow id and file URLs.
   * @param out_manifest   The verified manifest.
   * @return AZ_IOT_OK; AZ_IOT_ERR_NOT_FOUND without `workflowId`;
   * AZ_IOT_ERR_INVALID_ARG on bad arguments or malformed input;
   * AZ_IOT_ERR_NOT_ENOUGH_SPACE when the update exceeds AZ_IOT_SU_MAX_STEP_COUNT,
   * AZ_IOT_SU_MAX_FILE_COUNT, AZ_IOT_SU_MAX_FILE_COUNT_PER_STEP, 2 hashes per file or the
   * file URL capacity of @p storage;
   * AZ_IOT_ERR_NOT_SUPPORTED when @p crypto has no verify_rs256;
   * AZ_IOT_ERR_AUTH when verification fails; AZ_IOT_ERR_INTERNAL when the
   * crypto backend fails. az_iot_su_verify_manifest() reports why.
   */
  AZ_NODISCARD az_iot_result az_iot_su_parse_update_request(
      az_span request_json,
      const az_iot_crypto* crypto,
      const az_iot_su_root_key* root_keys,
      size_t root_key_count,
      const az_iot_su_update_request_storage* storage,
      az_iot_su_client_update_request* out_request,
      az_iot_su_client_update_manifest* out_manifest);

  /**
   * Verify one downloaded file's SHA-256 against the signed manifest, streaming
   * the file back through @p read_chunk. Standalone (no client/state machine) so a
   * bring-your-own-state-machine agent performs the same integrity check the
   * managed client does after each download. Needs only the SHA-256 functions
   * of @p crypto, not verify_rs256.
   *
   * Returns AZ_IOT_OK when the hash matches, AZ_IOT_ERR_INVALID_ARG on bad
   * arguments, or AZ_IOT_ERR_AUTH on a missing sha256 entry, a backend or read
   * error, or a hash mismatch.
   */
  AZ_NODISCARD az_iot_result az_iot_su_verify_file_hash(
      const az_iot_su_client_update_manifest_file* file,
      const az_iot_crypto* crypto,
      az_iot_su_read_chunk_callback read_chunk,
      void* read_ctx);

  /**
   * @brief Builds the status-report request body for @p report.
   *
   * The body the managed client and library mode send; for an agent that
   * carries reports itself.
   *
   * @param[in] report The report. Only read during the call.
   * @param[out] out_json Destination buffer.
   * @param[in] out_size Size of @p out_json.
   * @param[out] out_len Bytes written; zero on error. May be NULL.
   * @return AZ_IOT_OK on success.
   * @retval AZ_IOT_ERR_INVALID_ARG Bad arguments, or a report the service would
   *   refuse (empty `workflow_id`, incomplete `installed_update_id`, or an
   *   outcome / failure-origin mismatch).
   * @retval AZ_IOT_ERR_NOT_ENOUGH_SPACE The body does not fit @p out_json.
   */
  AZ_NODISCARD az_iot_result az_iot_su_build_report(
      const az_iot_su_report* report,
      uint8_t* out_json,
      size_t out_size,
      size_t* out_len);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_SU_H */
