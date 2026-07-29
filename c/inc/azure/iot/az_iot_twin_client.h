// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/* SPDX-License-Identifier: MIT */
#ifndef AZ_IOT_TWIN_CLIENT_H
#define AZ_IOT_TWIN_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "az_iot_result.h"
#include "az_iot_connection_client.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One twin section (desired or reported) as reported by the service.
 *
 * `version` is the authoritative version of the section at publish time; 0 is
 * the protocol's "no authoritative version" sentinel. `payload` is opaque to
 * the SDK (IoT Hub's JSON merge-patch convention is a layer above) and is NULL
 * when the service sent no payload for this section. It is valid only for the
 * duration of the callback -- copy anything you need to keep. */
typedef struct az_iot_twin_section
{
    uint64_t       version;
    const uint8_t* payload;
    size_t         payload_len;
} az_iot_twin_section;

/* A view of the device twin delivered by the service.
 *
 * IoT Hub Next returns the two sections separately, each with its own
 * authoritative version, so `desired` and `reported` are populated and
 * `document` is NULL. Classic returns a single JSON document holding both and
 * carries no per-section version, so it populates `document` and leaves the
 * sections zeroed. */
typedef struct az_iot_twin_state
{
    az_iot_twin_section desired;
    az_iot_twin_section reported;
    const uint8_t*      document;      /* Classic only: the whole twin document */
    size_t              document_len;
} az_iot_twin_state;

/* Outcome of a reported-properties patch.
 *
 * Reported writes use optimistic concurrency on IoT Hub Next: the SDK sends the
 * version it believes is authoritative, and the service rejects the write if it
 * has moved on. VERSION_MISMATCH means another writer got there first --
 * re-read the twin, re-apply your changes, and retry. `version` is the new
 * authoritative version on OK, and the unchanged current version otherwise.
 * Classic has no such signal and always reports OK with version 0. */
typedef enum az_iot_twin_patch_status
{
    AZ_IOT_TWIN_PATCH_UNSPECIFIED = 0,
    AZ_IOT_TWIN_PATCH_OK = 1,
    AZ_IOT_TWIN_PATCH_VERSION_MISMATCH = 2,
    AZ_IOT_TWIN_PATCH_PAYLOAD_INVALID = 3,
    AZ_IOT_TWIN_PATCH_PAYLOAD_TOO_LARGE = 4,
    AZ_IOT_TWIN_PATCH_INTERNAL_ERROR = 5
} az_iot_twin_patch_status;

typedef struct az_iot_twin_patch_result
{
    az_iot_twin_patch_status status;
    uint64_t                 version;
} az_iot_twin_patch_result;

/* `status` reports whether the exchange completed at all (e.g. AZ_IOT_OK, or
 * AZ_IOT_ERR_* if it could not be delivered); `twin` is NULL unless status is
 * AZ_IOT_OK. */
typedef void (*az_iot_twin_get_callback)(
    az_iot_result status,
    const az_iot_twin_state* twin,
    void* user_ctx);

/* `status` reports whether the exchange completed; `result` carries the
 * service's verdict and is NULL unless status is AZ_IOT_OK. */
typedef void (*az_iot_twin_patch_ack_callback)(
    az_iot_result status,
    const az_iot_twin_patch_result* result,
    void* user_ctx);

/* Backend-initiated full-state push (IoT Hub Next only). The service sends this
 * on connection establishment when the device's twin versions are behind and
 * the matching opts.twin_push bit was advertised in the birth, and may also
 * send it mid-connection as a recovery mechanism. Sections the service chose
 * not to push have a NULL payload. */
typedef void (*az_iot_twin_push_callback)(
    const az_iot_twin_state* twin,
    void* user_ctx);

typedef void (*az_iot_twin_desired_callback)(
    const uint8_t* desired_patch,
    size_t desired_patch_len,
    uint64_t version,
    void* user_ctx);

#ifndef AZ_IOT_TWIN_MAX_PENDING
#define AZ_IOT_TWIN_MAX_PENDING 8
#endif

/* Bytes of protobuf framing the SDK wraps around a reported-properties patch on
 * IoT Hub Next: the if_match field plus the payload's tag and length prefix.
 * Size an encode buffer to your largest patch plus this. */
#define AZ_IOT_TWIN_ENCODE_OVERHEAD 24

/* Desired-property subscriber registry capacity (compile-time configurable).
 * Two pools: feature-client slots (e.g. ADU) are notified before application
 * slots. See az_iot_twin_client_subscribe_desired(). */
#ifndef AZ_IOT_TWIN_MAX_DESIRED_FEATURE_SUBS
#define AZ_IOT_TWIN_MAX_DESIRED_FEATURE_SUBS 2
#endif
#ifndef AZ_IOT_TWIN_MAX_DESIRED_APP_SUBS
#define AZ_IOT_TWIN_MAX_DESIRED_APP_SUBS 2
#endif

typedef struct az_iot_twin_desired_sub
{
    az_iot_twin_desired_callback cb;
    void*                  user_ctx;
    bool                   in_use;
} az_iot_twin_desired_sub;

typedef struct az_iot_twin_client
{
    struct
    {
        az_iot_connection_client* conn;
        /* Desired-property subscriber registry. Feature-client slots are
         * dispatched before application slots (two-pass). */
        az_iot_twin_desired_sub   desired_feature_subs[AZ_IOT_TWIN_MAX_DESIRED_FEATURE_SUBS];
        az_iot_twin_desired_sub   desired_app_subs[AZ_IOT_TWIN_MAX_DESIRED_APP_SUBS];
        bool                        dispatching;
        uint32_t                    next_rid;

        /* Hub-Next only. The device's view of the authoritative versions,
         * seeded from the birth-ack of the current connection and advanced by
         * every response and patch the service sends. `reported_version` is
         * what the SDK sends as if_match on the next reported patch.
         * `nonce` is the connection those versions belong to, so they can be
         * re-seeded when the client reconnects. */
        uint64_t                  desired_version;
        uint64_t                  reported_version;
        uint8_t                   nonce[16];
        bool                      nonce_valid;

        az_iot_twin_push_callback push_cb;
        void*                     push_user_ctx;

        /* Caller-provided scratch used to frame outbound Hub-Next patches. */
        uint8_t*                  encode_buffer;
        size_t                    encode_buffer_len;

        struct
        {
            bool     in_use;
            uint32_t rid;                 /* Classic correlation */
            uint8_t  corr[16];            /* Hub-Next correlation (UUID) */
            int      kind;  /* internal enum */
            union {
                az_iot_twin_get_callback       get_cb;
                az_iot_twin_patch_ack_callback patch_cb;
            } cb;
            void* user_ctx;
        } pending[AZ_IOT_TWIN_MAX_PENDING];
    } _internal;
} az_iot_twin_client;

AZ_NODISCARD az_iot_result az_iot_twin_client_init(
    az_iot_twin_client* client,
    az_iot_connection_client* conn);

void az_iot_twin_client_destroy(az_iot_twin_client* client);

AZ_NODISCARD az_iot_result az_iot_twin_client_get(az_iot_twin_client* twin, az_iot_twin_get_callback cb, void* user_ctx);

AZ_NODISCARD az_iot_result az_iot_twin_client_patch_reported(
    az_iot_twin_client* twin,
    const uint8_t* patch,
    size_t patch_len,
    az_iot_twin_patch_ack_callback cb,
    void* user_ctx);

/* Register/unregister an application subscriber for desired-property patches.
 * Each subscriber receives the full patch on every desired update and decides
 * whether it carries keys it cares about. Returns AZ_IOT_ERR_NOT_SUPPORTED when
 * the application pool is full, AZ_IOT_ERR_BUSY if called from within a dispatch.
 */
AZ_NODISCARD az_iot_result az_iot_twin_client_subscribe_desired(
    az_iot_twin_client* twin,
    az_iot_twin_desired_callback cb,
    void* user_ctx);

AZ_NODISCARD az_iot_result az_iot_twin_client_unsubscribe_desired(
    az_iot_twin_client* twin,
    az_iot_twin_desired_callback cb,
    void* user_ctx);

/* Register the callback for backend-initiated twin pushes (IoT Hub Next only;
 * Classic never invokes it). Pass cb = NULL to clear. A push only arrives if
 * the application advertised it via the connection client's
 * opts.twin_push.push_desired / .push_reported. */
AZ_NODISCARD az_iot_result az_iot_twin_client_set_push_callback(
    az_iot_twin_client* twin,
    az_iot_twin_push_callback cb,
    void* user_ctx);

/* Provide the scratch buffer the SDK uses to frame an outbound reported patch
 * on IoT Hub Next (the protobuf envelope around your payload). The SDK never
 * allocates and never declares a payload buffer of its own; without one,
 * az_iot_twin_client_patch_reported() returns AZ_IOT_ERR_NOT_ENOUGH_SPACE on
 * Hub-Next. Size it to your largest patch plus AZ_IOT_TWIN_ENCODE_OVERHEAD.
 * The buffer must outlive the client. Not used by Classic, which publishes the
 * patch payload verbatim. */
AZ_NODISCARD az_iot_result az_iot_twin_client_set_encode_buffer(
    az_iot_twin_client* twin,
    uint8_t* buffer,
    size_t buffer_len);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_TWIN_CLIENT_H */
