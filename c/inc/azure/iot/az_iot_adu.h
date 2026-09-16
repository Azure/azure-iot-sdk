// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
#ifndef AZ_IOT_ADU_H
#define AZ_IOT_ADU_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "az_iot_connection_client.h"
#include "az_iot_result.h"

/* azure-sdk-for-c manifest parsing structs. This SDK header is named
 * az_iot_adu.h (NOT az_iot_adu_client.h) specifically so this angle-bracket
 * include resolves to the upstream header rather than shadowing itself. */
#include <azure/iot/az_iot_adu_client.h>

#ifdef __cplusplus
extern "C"
{
#endif

  /* --- Result codes (returned by platform/crypto hooks) -------------------- */

#define AZ_IOT_ADU_RESULT_SUCCESS 0
#define AZ_IOT_ADU_RESULT_IN_PROGRESS 1
#define AZ_IOT_ADU_RESULT_REBOOT_REQUIRED 2
#define AZ_IOT_ADU_RESULT_ALREADY_INSTALLED 3
#define AZ_IOT_ADU_RESULT_CANCELLED 4
#define AZ_IOT_ADU_RESULT_FAILURE (-1)

/* The agent "success" result code reported to the ADU service (ADU agent
 * convention; the value used throughout azure-sdk-for-c examples). */
#define AZ_IOT_ADU_AGENT_RESULT_CODE_SUCCESS 700

/* extended_result_code facility nibble (bits 31..28). The low 28 bits carry the
 * originating hook's raw result (truncated) or an SDK sub-code. See the design
 * doc "Result-Code Mapping". */
#define AZ_IOT_ADU_FACILITY_MANIFEST 0x1u /* JWS / SJWK verification, kid, alg */
#define AZ_IOT_ADU_FACILITY_DOWNLOAD 0x2u /* download_fn transport failure */
#define AZ_IOT_ADU_FACILITY_HASH 0x3u /* SHA-256 mismatch */
#define AZ_IOT_ADU_FACILITY_BACKUP 0x4u /* backup_fn failure */
#define AZ_IOT_ADU_FACILITY_INSTALL 0x5u /* install_fn failure */
#define AZ_IOT_ADU_FACILITY_APPLY 0x6u /* apply_fn failure */
#define AZ_IOT_ADU_FACILITY_RESTORE 0x7u /* restore_fn failure (rollback failed) */
#define AZ_IOT_ADU_FACILITY_INTERNAL 0xFu /* parser/state/buffer error in the client */

/* Compose a 32-bit extended_result_code from a facility nibble + sub-code. */
#define AZ_IOT_ADU_EXTENDED_RESULT(facility, code) \
  (int32_t)((((uint32_t)(facility) & 0xFu) << 28) | ((uint32_t)(code) & 0x0FFFFFFFu))

/* --- Compile-time capacities --------------------------------------------- */

/* Maximum number of RSA root public keys the core trust store holds. */
#ifndef AZ_IOT_ADU_MAX_ROOT_KEYS
#define AZ_IOT_ADU_MAX_ROOT_KEYS 4
#endif

/* Scratch buffer (in-struct) that holds a COPY of the desired-property service
 * payload for the current deployment. The channel's delivery buffer is only
 * valid during the subscriber callback, but the workflow is processed
 * asynchronously across many do_work() calls; the upstream parser stores spans
 * that point INTO this payload (and unescapes the manifest in place), so it must
 * outlive the callback. Sized for a v5 manifest with the upstream's bounded
 * step/file counts plus the JWS signature. Override if your deployments are
 * larger. No heap is used. */
#ifndef AZ_IOT_ADU_REQUEST_BUFFER_SIZE
#define AZ_IOT_ADU_REQUEST_BUFFER_SIZE 4096
#endif

/* In-struct scratch used to (de)serialize the persisted workflow state passed to
 * persist_state_fn / load_state_fn. Sized as the request buffer plus a fixed
 * overhead for the persistence header and v2 trailer (retry offset/len, manifest
 * CRC, install-result ints, per-step result pairs and a trailing CRC-32). The
 * overhead is generous; a compile-time assertion in adu_client.c guarantees the
 * exact serialized size always fits. This lives in the caller-allocated client
 * struct (one per instance) so no file-scope static or heap buffer is needed. */
#ifndef AZ_IOT_ADU_PERSIST_OVERHEAD
#define AZ_IOT_ADU_PERSIST_OVERHEAD 256
#endif
#define AZ_IOT_ADU_PERSIST_BLOB_SIZE (AZ_IOT_ADU_REQUEST_BUFFER_SIZE + AZ_IOT_ADU_PERSIST_OVERHEAD)

/* Capacities for the copied-out deployment identity (workflow `id` and
 * `retryTimestamp`) used to distinguish a retry/replacement from a harmless
 * redelivery. Deployment ids are GUID-shaped (~36 chars) and retry timestamps
 * are ISO-8601 (~28 chars); these include generous headroom. An identity that
 * does not fit simply disables de-duplication for that deployment (it is then
 * reprocessed on redelivery), so correctness never depends on the size. */
#ifndef AZ_IOT_ADU_WORKFLOW_ID_SIZE
#define AZ_IOT_ADU_WORKFLOW_ID_SIZE 64
#endif
#ifndef AZ_IOT_ADU_RETRY_TIMESTAMP_SIZE
#define AZ_IOT_ADU_RETRY_TIMESTAMP_SIZE 64
#endif

/* Persistence/resume blob (see design doc; consumed by Phase 5 resume logic). */
#ifndef AZ_IOT_ADU_STATE_BLOB_VERSION
#define AZ_IOT_ADU_STATE_BLOB_VERSION 1
#endif
#ifndef AZ_IOT_ADU_MAX_WORKFLOW_ID_LEN
#define AZ_IOT_ADU_MAX_WORKFLOW_ID_LEN 73 /* ADU service id: GUID-style, plus NUL */
#endif
#ifndef AZ_IOT_ADU_STATE_BLOB_MAX_SIZE
#define AZ_IOT_ADU_STATE_BLOB_MAX_SIZE 512
#endif

  /* --- Internal fine-grained state enum ------------------------------------ */

  typedef enum az_iot_adu_state
  {
    AZ_IOT_ADU_STATE_IDLE = 0,
    AZ_IOT_ADU_STATE_MANIFEST_RECEIVED,
    AZ_IOT_ADU_STATE_VERIFYING_MANIFEST,
    AZ_IOT_ADU_STATE_DOWNLOAD_STARTED,
    AZ_IOT_ADU_STATE_DOWNLOAD_COMPLETE,
    AZ_IOT_ADU_STATE_BACKUP_STARTED,
    AZ_IOT_ADU_STATE_BACKUP_COMPLETE,
    AZ_IOT_ADU_STATE_INSTALL_STARTED,
    AZ_IOT_ADU_STATE_INSTALL_COMPLETE,
    AZ_IOT_ADU_STATE_APPLY_STARTED,
    AZ_IOT_ADU_STATE_RESTORE_STARTED,
    AZ_IOT_ADU_STATE_FAILED,
  } az_iot_adu_state;

  /* --- Platform hooks (vtable) --------------------------------------------- */

  /**
   * Platform-specific operations the application (or a bundled adapter) provides.
   * The core ADU library MUST NOT link any platform/OS/network code; everything
   * platform-specific is reached through this vtable.
   *
   * Hooks return one of the AZ_IOT_ADU_RESULT_* codes above. download_fn and the
   * chunkable install/apply hooks MAY return IN_PROGRESS to be re-invoked on the
   * next do_work(); install_fn/apply_fn MAY return REBOOT_REQUIRED.
   */
  typedef struct az_iot_adu_platform_hooks
  {
    /**
     * Download one file (called once per file, once per do_work iteration).
     * Return IN_PROGRESS to continue on the next do_work, SUCCESS when complete.
     * The hook SHOULD check az_iot_adu_is_cancelled() periodically.
     * Hash verification is performed by core via the crypto hooks.
     */
    int32_t (*download_fn)(
        const az_iot_adu_client_update_manifest_file* file,
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
     * end-of-file). MUST return AZ_IOT_ADU_RESULT_SUCCESS on a successful read.
     */
    int32_t (*read_file_fn)(
        const az_iot_adu_client_update_manifest_file* file,
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
        const az_iot_adu_client_update_manifest* manifest,
        uint32_t step_index,
        void* user_ctx);

    /**
     * Apply (activate) the installed update for one step.
     * MAY return IN_PROGRESS (chunked) or REBOOT_REQUIRED.
     */
    int32_t (*apply_fn)(
        const az_iot_adu_client_update_manifest* manifest,
        uint32_t step_index,
        void* user_ctx);

    /** Backup current state before installing. OPTIONAL (MAY be NULL). */
    int32_t (*backup_fn)(
        const az_iot_adu_client_update_manifest* manifest,
        uint32_t step_index,
        void* user_ctx);

    /** Restore previous state on failure. OPTIONAL (MAY be NULL). */
    int32_t (*restore_fn)(
        const az_iot_adu_client_update_manifest* manifest,
        uint32_t step_index,
        void* user_ctx);

    /**
     * Report whether the update described by manifest is already installed.
     * MUST return AZ_IOT_ADU_RESULT_ALREADY_INSTALLED if so, SUCCESS otherwise.
     */
    int32_t (*is_installed_fn)(const az_iot_adu_client_update_manifest* manifest, void* user_ctx);

    /**
     * Persist workflow state for reboot survival. OPTIONAL — REQUIRED only if a
     * reboot is possible mid-update (i.e. install/apply may return
     * REBOOT_REQUIRED). Consumed by Phase 5 resume logic.
     */
    int32_t (*persist_state_fn)(const uint8_t* state_blob, size_t state_blob_len, void* user_ctx);

    /**
     * Load previously-persisted workflow state. Return 0 and fill
     * state_blob/len on success; non-zero if no state persisted.
     */
    int32_t (*load_state_fn)(
        uint8_t* state_blob,
        size_t state_blob_capacity,
        size_t* state_blob_len,
        void* user_ctx);

    void* user_ctx;
  } az_iot_adu_platform_hooks;

  /* --- Crypto hooks (REQUIRED, pure primitives) ---------------------------- */

  /**
   * Pure cryptographic primitives. These hooks MUST NOT parse JWS, decode
   * base64url, resolve keys, or enforce revocation — all of that orchestration
   * lives in ADU core. An adapter therefore only wires "RSA verify + SHA-256".
   * Pre-built implementations are available under adapters/adu/.
   */
  typedef struct az_iot_adu_crypto_hooks
  {
    /**
     * Verify an RSASSA-PKCS1-v1_5 signature over SHA-256 (JWS "alg":"RS256").
     * The public key is passed as raw big-endian modulus/exponent (already
     * base64url-decoded by core). MUST return AZ_IOT_ADU_RESULT_SUCCESS iff the
     * signature is valid, AZ_IOT_ADU_RESULT_FAILURE otherwise.
     */
    int32_t (*verify_rs256_fn)(
        const uint8_t* modulus,
        size_t modulus_len,
        const uint8_t* exponent,
        size_t exponent_len,
        const uint8_t* signed_data,
        size_t signed_data_len,
        const uint8_t* signature,
        size_t signature_len,
        void* user_ctx);

    /** Compute SHA-256 of a buffer. */
    int32_t (
        *sha256_fn)(const uint8_t* data, size_t data_len, uint8_t hash_out[32], void* user_ctx);

    /** Initialize an incremental SHA-256 context (opaque, impl-managed). */
    int32_t (*sha256_init_fn)(void** ctx_out, void* user_ctx);

    /** Feed data into an incremental SHA-256. */
    int32_t (*sha256_update_fn)(void* ctx, const uint8_t* data, size_t len, void* user_ctx);

    /** Finalize an incremental SHA-256, write 32-byte hash, free ctx. */
    int32_t (*sha256_final_fn)(void* ctx, uint8_t hash_out[32], void* user_ctx);

    void* user_ctx;
  } az_iot_adu_crypto_hooks;

  /* --- Root key store (owned and managed by ADU core) ---------------------- */

  /**
   * An RSA root public key trusted to sign Signed JWKs (SJWKs). All fields are
   * caller-owned; core stores the pointers (no deep copy of key bytes), so the
   * arrays MUST outlive the client.
   */
  typedef struct az_iot_adu_root_key
  {
    const char* kid; /* JWK key id, matched against the SJWK header `kid`. */
    const uint8_t* modulus; /* big-endian RSA modulus (n). */
    size_t modulus_len;
    const uint8_t* exponent; /* big-endian RSA exponent (e). */
    size_t exponent_len;
    bool disabled; /* true = revoked/disabled; rejected during resolution. */
  } az_iot_adu_root_key;

  /* --- Device properties (plain struct, deep-copied by the client) --------- */

  /* NOTE: the `_info` suffix is deliberate. The vendored azure-sdk-for-c already
   * defines a type of the base name az_iot_adu_update_id, pulled in here via
   * <azure/iot/az_iot_adu_client.h>; `_info` keeps this device-facing struct
   * distinct from the upstream type without resorting to a bare `_t` tag. */
  typedef struct az_iot_adu_update_id_info
  {
    const char* provider;
    const char* name;
    const char* version;
  } az_iot_adu_update_id_info;

  typedef struct az_iot_adu_custom_property
  {
    const char* name;
    const char* value;
  } az_iot_adu_custom_property;

  /**
   * Device properties supplied by the application. All fields are caller-owned;
   * the client DEEP-COPIES them into its cache buffer at init() and on
   * update_device_properties(). After those calls return, the application MAY
   * mutate or free this struct and the arrays/strings it points to.
   */
  typedef struct az_iot_adu_device_properties
  {
    const char* manufacturer;
    const char* model;
    az_iot_adu_update_id_info installed_update_id;
    const az_iot_adu_custom_property* custom_properties; /* caller's array, MAY be NULL */
    size_t custom_properties_count;
  } az_iot_adu_device_properties;

/* Default size (bytes) for the caller-owned device-properties cache buffer set
 * in az_iot_adu_client_config_options. Override before including if your device
 * properties (manufacturer/model/update-id/custom props) are larger, or size a
 * buffer exactly with az_iot_adu_device_props_buffer_size(). */
#ifndef AZ_IOT_ADU_DEVICE_PROPS_BUFFER_SIZE
#define AZ_IOT_ADU_DEVICE_PROPS_BUFFER_SIZE 512
#endif

/* Declares a device-properties cache buffer named `name`, sized by
 * AZ_IOT_ADU_DEVICE_PROPS_BUFFER_SIZE, for az_iot_adu_client_config_options:
 *   AZ_IOT_ADU_DEVICE_PROPS_STORAGE(dp_buf);
 *   opts.device_props_buffer = dp_buf;
 *   opts.device_props_buffer_size = sizeof(dp_buf); */
#define AZ_IOT_ADU_DEVICE_PROPS_STORAGE(name) uint8_t name[AZ_IOT_ADU_DEVICE_PROPS_BUFFER_SIZE]

  /* Returns the exact number of bytes az_iot_adu_client_initialize() needs in
   * device_props_buffer to cache `device_props` (a az_iot_adu_device_properties
   * header plus the packed NUL-terminated strings). Use it to size the buffer
   * precisely instead of the AZ_IOT_ADU_DEVICE_PROPS_BUFFER_SIZE default. Returns
   * 0 if device_props is NULL. */
  AZ_NODISCARD size_t
  az_iot_adu_device_props_buffer_size(const az_iot_adu_device_properties* device_props);

  /* --- Client struct -------------------------------------------------------- */

  /* NOTE: keeps the _t suffix. The vendored azure-sdk-for-c defines az_iot_adu_client
   * (the low-level parser handle, embedded below as the `az` field), so our
   * higher-level client type must stay distinct from it. */
  /* --- Update outcome vocabulary ------------------------------------------ */
  /* The structured result the engine produces for a workflow. Public because an
   * application observes it; the transport that carries it is internal. */

  /**
   * @brief Terminal and non-terminal outcomes a device reports for a workflow.
   *
   * These are the values the service accepts on the status-report operation.
   * `SKIPPED` replaces the ADUv1 accept/reject acknowledgement: an engine that
   * declines a deployment reports it rather than answering a protocol-level
   * "reject".
   */
  typedef enum az_iot_adu_outcome
  {
    AZ_IOT_ADU_OUTCOME_IN_PROGRESS = 0,
    AZ_IOT_ADU_OUTCOME_SUCCEEDED,
    AZ_IOT_ADU_OUTCOME_FAILED,
    AZ_IOT_ADU_OUTCOME_CANCELED,
    AZ_IOT_ADU_OUTCOME_SKIPPED,
  } az_iot_adu_outcome;

  /**
   * @brief Which layer a failure came from. `NOT_APPLICABLE` is used for every
   *        non-failure outcome.
   */
  typedef enum az_iot_adu_failure_origin
  {
    AZ_IOT_ADU_FAILURE_ORIGIN_NOT_APPLICABLE = 0,
    AZ_IOT_ADU_FAILURE_ORIGIN_ADU_CLOUD_SERVICE,
    AZ_IOT_ADU_FAILURE_ORIGIN_ADU_MANAGED_RESOURCE,
    AZ_IOT_ADU_FAILURE_ORIGIN_AGENT_CORE,
    AZ_IOT_ADU_FAILURE_ORIGIN_AGENT_EXTENSION,
    AZ_IOT_ADU_FAILURE_ORIGIN_AGENT_DEPENDENCY,
    AZ_IOT_ADU_FAILURE_ORIGIN_DEVICE,
    AZ_IOT_ADU_FAILURE_ORIGIN_OTHER,
  } az_iot_adu_failure_origin;

  /** @brief An update identity triple. Spans are NOT owned by this struct. */
  typedef struct az_iot_adu_report_update_id
  {
    const char* provider;
    const char* name;
    const char* version;
  } az_iot_adu_report_update_id;

  /**
   * @brief The structured result the engine hands a channel.
   *
   * `workflow_id` alone is the correlation key: reporting is idempotent on it,
   * and the ADUv1 `retryTimestamp` half of the old composite key does not exist
   * here.
   *
   * `installed_update_id` means "what is installed on the device *now*", not
   * "what this workflow is about". It is therefore the previously installed
   * update while a workflow is in progress or has failed, and the newly applied
   * update once the workflow has succeeded. It may be NULL when the device has
   * nothing installed (a day-0 onboarding device), in which case the channel
   * omits it rather than serializing a null.
   */
  typedef struct az_iot_adu_report
  {
    const char* workflow_id;

    /* NULL when the device has nothing installed. */
    const az_iot_adu_report_update_id* installed_update_id;

    az_iot_adu_outcome outcome;
    az_iot_adu_failure_origin failure_origin;

    /* Agent result code. The engine emits the values the contract defines:
     * 1 while in progress, 700 on success, negative on failure. */
    int32_t result_code;

    /* Comma-separated hex codes, e.g. "00000000" or "0x80000001". Never NULL. */
    const char* extended_result_codes;

    /* Free-form human-readable detail. May be NULL. */
    const char* result_details;

    /* Array and detail spans are borrowed for the report call; NULL when count is zero. */
    const az_iot_adu_client_step_result* step_results;
    int32_t step_results_count;
  } az_iot_adu_report;

  /* Opaque forward declaration. The delivery/reporting channel is an INTERNAL
   * construct (src/features/adu/internal/adu_channel_internal.h): applications
   * do not build one and cannot see inside it. It is named here only because
   * the client struct is caller-allocated and therefore needs its size. */
  struct az_iot_adu_channel_vtable;

  typedef struct az_iot_adu_client_t
  {
    struct
    {
      struct
      {
        const struct az_iot_adu_channel_vtable* vtable;
        void* ctx;
      } channel;

      /* Storage for the SDK-built channel. Opaque here: sized so the client
       * stays caller-allocated with no hidden allocation. */
      struct
      {
        void* pointers[24];
        uint8_t bytes[3328];
        uint64_t alignment[4];
      } channel_storage;
      az_iot_adu_platform_hooks hooks;
      az_iot_adu_crypto_hooks crypto;

      /* Upstream parser/formatter handle. */
      az_iot_adu_client az;

      /* Root-key store (core-owned). Pointers reference caller arrays. */
      az_iot_adu_root_key root_keys[AZ_IOT_ADU_MAX_ROOT_KEYS];
      size_t root_key_count;

      az_iot_adu_state state;

      /* Current deployment, parsed from the desired-property patch. */
      az_iot_adu_client_update_request current_request;
      az_iot_adu_client_update_manifest current_manifest;
      bool have_request;
      uint32_t current_step;
      uint32_t current_file;
      bool cancel_requested;

      /* Identity of the deployment currently being processed (or the last one
       * started). Copied out of the request so it survives request_buffer
       * being overwritten by a later patch, and used to tell a retry (same id,
       * newer retryTimestamp) and a replacement (different id) apart from a
       * harmless redelivery (same id + same/empty retryTimestamp). See the
       * design doc "Retry vs. Replacement Detection". */
      bool active_workflow_valid;
      uint8_t active_workflow_id[AZ_IOT_ADU_WORKFLOW_ID_SIZE];
      size_t active_workflow_id_len;
      uint8_t active_retry_timestamp[AZ_IOT_ADU_RETRY_TIMESTAMP_SIZE];
      size_t active_retry_timestamp_len;
      /* CRC-32 fingerprint of the active deployment's raw updateManifest, used
       * to catch the (anomalous) case of an unchanged workflow id + retry
       * timestamp carrying a different manifest: that is a replacement, not a
       * duplicate, so it must (re)start rather than be ignored. */
      uint32_t active_manifest_crc;

      /* COPY of the service payload backing current_request/current_manifest
       * spans (the channel's delivery buffer is gone after the callback). */
      uint8_t request_buffer[AZ_IOT_ADU_REQUEST_BUFFER_SIZE];
      size_t request_len;

      /* Scratch for (de)serializing persisted workflow state (persist/resume).
       * Per-instance so multiple ADU clients never share it; persist and resume
       * never run concurrently, so one buffer serves both directions. */
      uint8_t persist_scratch[AZ_IOT_ADU_PERSIST_BLOB_SIZE];

      /* The unescaped manifest text within request_buffer (parse_manifest
       * sets this; persistence/resume re-parses it). */
      az_span manifest_text;

      /* Accumulated result reported to the service. */
      az_iot_adu_client_install_result install_result;

      /* Client-owned device-properties cache (deep copy of caller's struct). */
      uint8_t* device_props_buffer;
      size_t device_props_buffer_size;
      bool device_props_report_pending;

      /* Which fetch the application asked for and the channel has not yet
       * accepted: 0 none, 1 onboarding, 2 regular. Not a bool, because a retry
       * must re-issue the route that was actually requested. */
      uint8_t pending_fetch;

      /* Upstream-shaped view of the cached custom properties (az_span arrays
       * over the packed strings in device_props_buffer), handed to the
       * agent-state formatter at report time. */
      az_iot_adu_device_custom_properties custom_props_view;

      /* Serialized installed-update-id, the JSON object the ADU service
       * expects in the reported `installedUpdateId` field, built once at
       * initialize time from the caller's update id. */
      char update_id_json[128];
      size_t update_id_json_len;

      /* Terminal outcome for the active workflow, latched at the transition
       * that ends it. Reporting is keyed on workflowId, so the engine must be
       * able to distinguish SUCCEEDED / CANCELED / SKIPPED after the workflow
       * state itself has returned to Idle. */
      az_iot_adu_outcome pending_outcome;

      /* The update id that was actually applied, captured BEFORE the return to
       * Idle clears the manifest. A successful report carries this, because
       * installedUpdateId means "what is installed now" — not "what this
       * workflow was about". Strings are packed into applied_update_id_buf. */
      char applied_update_id_buf[192];
      az_iot_adu_report_update_id applied_update_id;
      bool applied_update_id_valid;

      /* Connection-state observer / detach safety (see design doc §16). */
      bool detached;
    } _internal;
  } az_iot_adu_client_t;

  /* --- Lifecycle ----------------------------------------------------------- */

  /**
   * Configuration for az_iot_adu_client_initialize(). Obtain a zero-initialized
   * instance from az_iot_adu_client_config_options_default() and set the required
   * fields before calling initialize.
   *
   * NOTE: named az_iot_adu_client_config_options (not ..._options) to avoid
   * colliding with azure-sdk-for-c's own az_iot_adu_client_options, which is
   * visible here because the platform-hook signatures use upstream parsing types.
   * The `config_` qualifier keeps our configuration struct distinct without a
   * bare `_t` tag.
   */
  typedef struct az_iot_adu_client_config_options
  {
    /* Platform operations (download/install/apply/...). MUST be non-NULL. */
    const az_iot_adu_platform_hooks* hooks;
    /* Pure-primitive crypto hooks (RSA verify + SHA-256). MUST be non-NULL. */
    const az_iot_adu_crypto_hooks* crypto;
    /* Caller-owned RSA root public keys that anchor manifest trust. The core
     * copies the small descriptor array into its fixed store (key BYTES are
     * referenced, not copied, so they MUST outlive the client). Capped at
     * AZ_IOT_ADU_MAX_ROOT_KEYS. For Microsoft-signed updates, pass
     * az_iot_adu_microsoft_root_keys(). */
    const az_iot_adu_root_key* root_keys;
    size_t root_key_count;
    /* Caller-owned device properties, DEEP-COPIED into the cache. May be
     * mutated/freed by the caller after initialize returns. MUST be non-NULL. */
    const az_iot_adu_device_properties* device_props;
    /* Caller-owned cache the client copies device_props into. No hidden
     * allocation; the buffer MUST outlive the client. MUST be non-NULL. */
    uint8_t* device_props_buffer;
    size_t device_props_buffer_size;

  } az_iot_adu_client_config_options;

  /* Returns an options struct with all fields zero-initialized. Set hooks, crypto,
   * root_keys/root_key_count, device_props and device_props_buffer/size on the
   * returned struct before passing it to az_iot_adu_client_initialize(). */
  AZ_NODISCARD az_iot_adu_client_config_options az_iot_adu_client_config_options_default(void);

  /**
   * Initialize the ADU client.
   *
   *   connection: the connection client this device is provisioned with. The SDK
   *     builds the device-update channel from it; the application does not
   *     implement any transport. It need NOT be connected: the ADU bootstrap
   *     check runs before the device registers, and the fields the channel needs
   *     (DPS id scope, registration id, credential) are set at init time.
   *   options: configuration (hooks, crypto, trust store, device properties and
   *     the caller-owned cache); see az_iot_adu_client_config_options. Returns
   *     AZ_IOT_ERR_INVALID_ARG if any required field is NULL,
   *     AZ_IOT_ERR_NOT_ENOUGH_SPACE if root_key_count exceeds
   *     AZ_IOT_ADU_MAX_ROOT_KEYS or the buffer is too small for device_props.
   *
   * NOTE: named *_initialize (not *_init) to avoid colliding with
   * azure-sdk-for-c's az_iot_adu_client_init(), which is visible here because the
   * platform-hook signatures use upstream parsing types.
   */
  AZ_NODISCARD az_iot_result az_iot_adu_client_initialize(
      az_iot_adu_client_t* client,
      az_iot_connection_client* connection,
      const az_iot_adu_client_config_options* options);

  /**
   * Return Microsoft's compiled-in ADU root public keys (const, static storage).
   * Convenience for the common case; equivalent to passing your own array to
   * az_iot_adu_client_initialize().
   */
  const az_iot_adu_root_key* az_iot_adu_microsoft_root_keys(size_t* out_count);

  void az_iot_adu_client_destroy(az_iot_adu_client_t* client);

  /**
   * Resume a workflow after device reboot. The application SHOULD call this during
   * startup. If no persisted state exists, this is a no-op. (Phase 5.)
   */
  AZ_NODISCARD az_iot_result az_iot_adu_client_resume(az_iot_adu_client_t* client);

  /* --- Runtime ------------------------------------------------------------- */

  /**
   * Drive the ADU state machine. The application MUST call this from its do_work
   * loop. Non-blocking: processes at most one chunk of work per invocation. Not
   * AZ_NODISCARD: a pump whose result is typically observed via state, not return.
   */
  az_iot_result az_iot_adu_client_do_work(az_iot_adu_client_t* client);

  /** Check if cancellation has been requested (called from within platform hooks). */
  bool az_iot_adu_is_cancelled(const az_iot_adu_client_t* client);

  /** Get the current ADU agent state. */
  az_iot_adu_state az_iot_adu_client_get_state(const az_iot_adu_client_t* client);

  /**
   * Ask for an ONBOARDING update — the day-0/pre-registration route.
   *
   * Use this while the device has no device record with the service yet. It is
   * the permissive route: it needs no registry entry, and it does not send
   * `installedUpdateId`.
   *
   * The application chooses the route because it is the only party that knows:
   * it persists its provisioning result across boots, while the SDK sees only
   * the current process. The service cannot be asked either — "no device
   * record" and "malformed request" share one error code, so probing would
   * mask real errors.
   *
   * Asynchronous. Records the request; the NEXT az_iot_adu_client_do_work()
   * issues it, and retries on a later tick if the channel is not ready. The
   * result arrives through the engine, not this return value. Returns
   * AZ_IOT_ERR_INVALID_ARG if @p client is NULL.
   *
   * Single-threaded contract: MUST be called on the do_work thread or be
   * externally serialized with do_work().
   */
  AZ_NODISCARD az_iot_result
  az_iot_adu_client_request_onboarding_update(az_iot_adu_client_t* client);

  /**
   * Ask for a REGULAR (software) update — the operational route.
   *
   * Use this once the device is provisioned and has a device record. It sends
   * `installedUpdateId`, which is how the service knows what to offer next.
   *
   * Calling it on a device that has no device record yet is rejected by the
   * service as an invalid request; use
   * az_iot_adu_client_request_onboarding_update() until then.
   *
   * Asynchronous, with the same contract as
   * az_iot_adu_client_request_onboarding_update().
   */
  AZ_NODISCARD az_iot_result az_iot_adu_client_request_update(az_iot_adu_client_t* client);

  /**
   * Update the cached device properties and request a report. Deep-copies
   * device_props into the client cache and sets a pending flag; the NEXT
   * do_work() publishes. Multiple calls coalesce into a single report. After this
   * returns, the caller MAY mutate or free device_props. Returns
   * AZ_IOT_ERR_NOT_ENOUGH_SPACE if the cache buffer is too small.
   *
   * Single-threaded contract: MUST be called on the do_work thread or be
   * externally serialized with do_work().
   */
  AZ_NODISCARD az_iot_result az_iot_adu_client_update_device_properties(
      az_iot_adu_client_t* client,
      const az_iot_adu_device_properties* device_props);

  /* --- Agent core-library API (library mode / bring-your-own state machine) - */
  /*
   * The functions below let a caller build their OWN ADU agent on top of the
   * SDK's vetted parse + trust + report code, WITHOUT adopting the managed state
   * machine, a channel, or any transport. They take spans/structs only, perform no
   * hidden allocation, and (where they verify) are fail-closed. The managed
   * az_iot_adu_client is implemented in terms of the same internal cores, so both
   * modes share one copy of the security-critical path. See
   * docs/eng/adu-feature-support.md Part C and adu-client-design.md §5.3.
   */

  /**
   * Streaming read callback used by az_iot_adu_verify_file_hash(). Read up to
   * @p buffer_size bytes starting at @p offset into @p buffer and set
   * @p out_read to the number of bytes produced (0 signals end-of-file). MUST
   * return AZ_IOT_ADU_RESULT_SUCCESS on a successful read (including the final
   * 0-byte read at end-of-file); any other value is treated as a read error.
   */
  typedef int32_t (*az_iot_adu_read_chunk_callback)(
      size_t offset,
      uint8_t* buffer,
      size_t buffer_size,
      size_t* out_read,
      void* read_ctx);

  /**
   * Validate and parse a deployment payload with NO channel, state machine, or
   * transport. Performs the full manifest trust chain (compact JWS split,
   * base64url decode, root-key `kid` resolution, `alg=RS256` enforcement, both
   * RSA signature checks via the crypto hooks, and the SHA-256 manifest binding),
   * and only then parses the update manifest. FAIL-CLOSED: on any error
   * @p out_request and @p out_manifest are left zeroed and a non-OK result is
   * returned.
   *
   *   request_json: the desired-property patch carrying the "deviceUpdate"
   *     component (the same shape the managed client consumes). MUTATED IN PLACE
   *     (the manifest is unescaped within the buffer) and MUST outlive
   *     @p out_request / @p out_manifest, whose az_spans point into it. No heap.
   *   crypto: RSA-verify + SHA-256 primitives (as for the managed client).
   *   root_keys / root_key_count: trusted RSA root public keys anchoring manifest
   *     trust; pass az_iot_adu_microsoft_root_keys() for Microsoft-signed updates.
   *   out_request: filled service request (workflow id/action, file urls, ...).
   *   out_manifest: filled, VERIFIED update manifest. Left empty when the request
   *     is a Cancel action (inspect out_request->workflow.action).
   *
   * Returns AZ_IOT_OK on a verified parse (or a parsed Cancel request),
   * AZ_IOT_ERR_NOT_FOUND when the patch carries no deviceUpdate/service object,
   * AZ_IOT_ERR_INVALID_ARG on bad arguments or malformed input, or
   * AZ_IOT_ERR_AUTH when manifest verification fails.
   */
  AZ_NODISCARD az_iot_result az_iot_adu_parse_update_request(
      az_span request_json,
      const az_iot_adu_crypto_hooks* crypto,
      const az_iot_adu_root_key* root_keys,
      size_t root_key_count,
      az_iot_adu_client_update_request* out_request,
      az_iot_adu_client_update_manifest* out_manifest);

  /**
   * Verify one downloaded file's SHA-256 against the signed manifest, streaming
   * the file back through @p read_chunk. Standalone (no client/state machine) so a
   * bring-your-own-state-machine agent performs the same integrity check the
   * managed client does after each download. Requires the incremental SHA-256
   * crypto hooks (sha256_init/update/final).
   *
   * Returns AZ_IOT_OK when the hash matches, AZ_IOT_ERR_INVALID_ARG on bad
   * arguments, or AZ_IOT_ERR_AUTH on a missing sha256 entry, a hook/read error,
   * or a hash mismatch.
   */
  AZ_NODISCARD az_iot_result az_iot_adu_verify_file_hash(
      const az_iot_adu_client_update_manifest_file* file,
      const az_iot_adu_crypto_hooks* crypto,
      az_iot_adu_read_chunk_callback read_chunk,
      void* read_ctx);

  /**
   * Build the agent-state report payload from a caller's own outcome data,
   * WITHOUT the state machine or a channel. Emits the same reported-property JSON the
   * managed client publishes, into the caller-provided @p out_json buffer.
   *
   *   device_props: the device's identity/version (manufacturer, model, installed
   *     update id, custom properties). Caller-owned; only read during the call.
   *   result: the accumulated install result (overall + per-step), or NULL when
   *     no result is available yet.
   *   request: the in-progress deployment request (for the reported workflow id),
   *     or NULL when idle.
   *   state: the agent state to report (mapped to Idle / InProgress / Failed).
   *   out_json / out_size: caller-owned destination buffer; out_len receives the
   *     number of bytes written (MAY be NULL).
   *
   * Returns AZ_IOT_OK on success, AZ_IOT_ERR_INVALID_ARG on bad arguments, or
   * AZ_IOT_ERR_NOT_ENOUGH_SPACE if the payload does not fit @p out_json.
   */
  AZ_NODISCARD az_iot_result az_iot_adu_build_report(
      const az_iot_adu_device_properties* device_props,
      const az_iot_adu_client_install_result* result,
      const az_iot_adu_client_update_request* request,
      az_iot_adu_state state,
      uint8_t* out_json,
      size_t out_size,
      size_t* out_len);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_ADU_H */
