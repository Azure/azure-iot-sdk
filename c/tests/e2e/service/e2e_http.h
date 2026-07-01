// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/* SPDX-License-Identifier: MIT */
/* Internal helper for the e2e service client: a minimal, non-blocking HTTPS/1.1
 * client used for the IoT Hub service REST APIs (direct methods, twin get/patch).
 *
 * It drives the vendored `az_amqp` sample reference transport (TLS over the
 * platform's native stack) directly, one request per connection. The client is
 * a pump-able state machine so the caller can interleave device MQTT pumping
 * while a direct-method call waits for the device to respond.
 *
 * This header is INTERNAL to the az_iot_e2e_service static library and is never
 * included by test translation units.
 */
#ifndef AZ_IOT_E2E_HTTP_H
#define AZ_IOT_E2E_HTTP_H

#include "az_amqp_sample_transport.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define E2E_HTTP_REQUEST_MAX 4096
#define E2E_HTTP_RESPONSE_MAX 16384

typedef enum e2e_http_phase_tag
{
    E2E_HTTP_PHASE_CONNECTING,
    E2E_HTTP_PHASE_WRITING,
    E2E_HTTP_PHASE_READING,
    E2E_HTTP_PHASE_DONE,
    E2E_HTTP_PHASE_FAILED,
} e2e_http_phase;

typedef struct e2e_http_request_tag
{
    az_amqp_sample_transport transport_storage;
    az_amqp_transport transport;
    e2e_http_phase phase;
    bool open_called;

    char request[E2E_HTTP_REQUEST_MAX];
    int request_len;
    int request_sent;

    uint8_t response[E2E_HTTP_RESPONSE_MAX];
    int response_len;
    bool headers_done;
    int body_start;
    int content_length; /* -1 until parsed / unknown */
    int http_status;

    const char* err;
} e2e_http_request_t;

/* Prepare (but do not send) an HTTPS request to @p host (port 443, TLS). @p method
 * is e.g. "GET"/"POST"/"PATCH"; @p path is the origin-form request target incl. any
 * query string. @p authorization is the full SAS token for the Authorization header.
 * @p json_body may be NULL for a body-less request. Returns false only on a request
 * that will not fit the request buffer. */
bool e2e_http_begin(
    e2e_http_request_t* r,
    const char* host,
    const char* method,
    const char* path,
    const char* authorization,
    const char* json_body);

/* Advance the request state machine a little without blocking on the device.
 * Returns 0 while pending, 1 when the response is complete, -1 on error. */
int e2e_http_poll(e2e_http_request_t* r);

/* Valid once e2e_http_poll returns 1: the parsed HTTP status code. */
int e2e_http_status(const e2e_http_request_t* r);

/* Valid once e2e_http_poll returns 1: the response body bytes and length. */
const uint8_t* e2e_http_body(const e2e_http_request_t* r, int* out_len);

/* A static description of the last failure, or NULL. */
const char* e2e_http_error(const e2e_http_request_t* r);

/* Close the underlying transport (best-effort). */
void e2e_http_end(e2e_http_request_t* r);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_E2E_HTTP_H */
