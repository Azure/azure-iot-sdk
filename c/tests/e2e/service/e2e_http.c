// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/* SPDX-License-Identifier: MIT */
#include "e2e_http.h"

#include "az_amqp_sample_wait.h"

#include <azure/amqp/az_amqp_common.h>
#include <azure/amqp/az_amqp_transport.h>
#include <azure/core/az_span.h>

#include <stdio.h>
#include <string.h>

#define E2E_HTTPS_PORT 443
#define E2E_HTTP_WAIT_MS 20

/* Wait briefly for the transport to become ready for @p status's interest, so the
 * pump does not spin. Kept short so an interleaved device pump stays responsive. */
static void http_wait(e2e_http_request* r, az_amqp_transport_status status)
{
    az_amqp_io_interest interest = (status == AZ_AMQP_TRANSPORT_STATUS_WANT_WRITE)
        ? AZ_AMQP_IO_INTEREST_WRITE
        : AZ_AMQP_IO_INTEREST_READ;
    az_amqp_sample_wait_for_io(&r->transport_storage, interest, E2E_HTTP_WAIT_MS);
}

/* Case-insensitive test of whether @p line (length @p line_len) begins with the
 * lowercase ASCII @p prefix. */
static bool ci_starts_with(const uint8_t* line, int line_len, const char* prefix)
{
    int i = 0;
    for (; prefix[i] != '\0'; i++)
    {
        if (i >= line_len)
        {
            return false;
        }
        uint8_t c = line[i];
        if (c >= 'A' && c <= 'Z')
        {
            c = (uint8_t)(c - 'A' + 'a');
        }
        if (c != (uint8_t)prefix[i])
        {
            return false;
        }
    }
    return true;
}

/* Parse the status line and Content-Length once the header terminator is present. */
static void http_parse_headers(e2e_http_request* r)
{
    if (r->headers_done)
    {
        return;
    }

    int header_end = -1;
    for (int i = 0; i + 3 < r->response_len; i++)
    {
        if (r->response[i] == '\r' && r->response[i + 1] == '\n' && r->response[i + 2] == '\r'
            && r->response[i + 3] == '\n')
        {
            header_end = i;
            break;
        }
    }
    if (header_end < 0)
    {
        return; /* headers not fully received yet */
    }
    r->headers_done = true;
    r->body_start = header_end + 4;

    /* Status line: "HTTP/1.1 <code> <reason>". */
    for (int i = 0; i < r->response_len && i < 16; i++)
    {
        if (r->response[i] == ' ' && i + 3 < r->response_len)
        {
            r->http_status = (r->response[i + 1] - '0') * 100 + (r->response[i + 2] - '0') * 10
                + (r->response[i + 3] - '0');
            break;
        }
    }

    /* Header lines up to header_end. */
    int line_start = 0;
    for (int i = 0; i + 1 <= header_end; i++)
    {
        if (r->response[i] == '\r' && r->response[i + 1] == '\n')
        {
            const uint8_t* line = r->response + line_start;
            int line_len = i - line_start;
            if (ci_starts_with(line, line_len, "content-length:"))
            {
                int p = (int)strlen("content-length:");
                while (p < line_len && (line[p] == ' ' || line[p] == '\t'))
                {
                    p++;
                }
                int value = 0;
                bool any = false;
                while (p < line_len && line[p] >= '0' && line[p] <= '9')
                {
                    value = value * 10 + (line[p] - '0');
                    any = true;
                    p++;
                }
                if (any)
                {
                    r->content_length = value;
                }
            }
            line_start = i + 2;
            i++; /* skip the '\n' */
        }
    }
}

bool e2e_http_begin(
    e2e_http_request* r,
    const char* host,
    const char* method,
    const char* path,
    const char* authorization,
    const char* json_body)
{
    memset(r, 0, sizeof(*r));
    r->phase = E2E_HTTP_PHASE_CONNECTING;
    r->content_length = -1;
    r->http_status = 0;

    int body_len = (json_body != NULL) ? (int)strlen(json_body) : 0;
    int n;
    if (json_body != NULL)
    {
        n = snprintf(
            r->request,
            sizeof(r->request),
            "%s %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Authorization: %s\r\n"
            "Content-Type: application/json; charset=utf-8\r\n"
            "Content-Length: %d\r\n"
            "Connection: close\r\n"
            "\r\n"
            "%s",
            method,
            path,
            host,
            authorization,
            body_len,
            json_body);
    }
    else
    {
        n = snprintf(
            r->request,
            sizeof(r->request),
            "%s %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Authorization: %s\r\n"
            "Connection: close\r\n"
            "\r\n",
            method,
            path,
            host,
            authorization);
    }
    if (n < 0 || n >= (int)sizeof(r->request))
    {
        r->phase = E2E_HTTP_PHASE_FAILED;
        r->err = "http: request too large";
        return false;
    }
    r->request_len = n;

    az_amqp_transport_options transport_options = { 0 };
    transport_options.host_name = az_span_create_from_str((char*)(uintptr_t)host);
    transport_options.port = E2E_HTTPS_PORT;
    transport_options.tls_enabled = true;
    if (az_result_failed(
            az_amqp_sample_transport_init(&r->transport, &r->transport_storage, &transport_options)))
    {
        r->phase = E2E_HTTP_PHASE_FAILED;
        r->err = "http: transport init failed";
        return false;
    }
    return true;
}

bool e2e_http_begin_ex(
    e2e_http_request* r,
    const char* host,
    const char* method,
    const char* path,
    const char* authorization,
    const char* content_type,
    const char* extra_header,
    const void* body,
    size_t body_len,
    const char* client_cert_pem,
    const char* client_key_pem)
{
    memset(r, 0, sizeof(*r));
    r->phase = E2E_HTTP_PHASE_CONNECTING;
    r->content_length = -1;
    r->http_status = 0;

    az_amqp_transport_options transport_options = { 0 };

    /* Build the header block, then append the (possibly binary) body. */
    const int cap = (int)sizeof(r->request);
    int len = snprintf(r->request, (size_t)cap, "%s %s HTTP/1.1\r\nHost: %s\r\n",
                       method, path, host);
    if (len < 0 || len >= cap) goto too_large;

    if (authorization != NULL && authorization[0] != '\0')
    {
        int n = snprintf(r->request + len, (size_t)(cap - len), "Authorization: %s\r\n", authorization);
        if (n < 0 || n >= cap - len) goto too_large;
        len += n;
    }
    if (body != NULL && body_len > 0)
    {
        if (content_type != NULL && content_type[0] != '\0')
        {
            int n = snprintf(r->request + len, (size_t)(cap - len), "Content-Type: %s\r\n", content_type);
            if (n < 0 || n >= cap - len) goto too_large;
            len += n;
        }
        int n = snprintf(r->request + len, (size_t)(cap - len), "Content-Length: %d\r\n", (int)body_len);
        if (n < 0 || n >= cap - len) goto too_large;
        len += n;
    }
    if (extra_header != NULL && extra_header[0] != '\0')
    {
        int n = snprintf(r->request + len, (size_t)(cap - len), "%s\r\n", extra_header);
        if (n < 0 || n >= cap - len) goto too_large;
        len += n;
    }
    {
        int n = snprintf(r->request + len, (size_t)(cap - len), "Connection: close\r\n\r\n");
        if (n < 0 || n >= cap - len) goto too_large;
        len += n;
    }
    if (body != NULL && body_len > 0)
    {
        if ((size_t)(cap - len) < body_len) goto too_large;
        memcpy(r->request + len, body, body_len);
        len += (int)body_len;
    }
    r->request_len = len;

    transport_options.host_name = az_span_create_from_str((char*)(uintptr_t)host);
    transport_options.port = E2E_HTTPS_PORT;
    transport_options.tls_enabled = true;
    if (client_cert_pem != NULL && client_cert_pem[0] != '\0')
    {
        transport_options.tls.client_certificate =
            az_span_create_from_str((char*)(uintptr_t)client_cert_pem);
    }
    if (client_key_pem != NULL && client_key_pem[0] != '\0')
    {
        transport_options.tls.client_private_key =
            az_span_create_from_str((char*)(uintptr_t)client_key_pem);
    }
    if (az_result_failed(
            az_amqp_sample_transport_init(&r->transport, &r->transport_storage, &transport_options)))
    {
        r->phase = E2E_HTTP_PHASE_FAILED;
        r->err = "http: transport init failed";
        return false;
    }
    return true;

too_large:
    r->phase = E2E_HTTP_PHASE_FAILED;
    r->err = "http: request too large";
    return false;
}

int e2e_http_poll(e2e_http_request* r)
{
    az_amqp_transport_status status;
    switch (r->phase)
    {
        case E2E_HTTP_PHASE_CONNECTING:
            status = r->open_called ? r->transport.vtable->process(&r->transport)
                                    : r->transport.vtable->open(&r->transport);
            r->open_called = true;
            if (status == AZ_AMQP_TRANSPORT_STATUS_OK)
            {
                r->phase = E2E_HTTP_PHASE_WRITING;
                return 0;
            }
            if (status == AZ_AMQP_TRANSPORT_STATUS_WANT_READ
                || status == AZ_AMQP_TRANSPORT_STATUS_WANT_WRITE)
            {
                http_wait(r, status);
                return 0;
            }
            r->phase = E2E_HTTP_PHASE_FAILED;
            r->err = "http: connect/handshake failed";
            return -1;

        case E2E_HTTP_PHASE_WRITING:
        {
            size_t wrote = 0;
            az_span src = az_span_create(
                (uint8_t*)r->request + r->request_sent, r->request_len - r->request_sent);
            status = r->transport.vtable->write(&r->transport, src, &wrote);
            if (status == AZ_AMQP_TRANSPORT_STATUS_OK)
            {
                r->request_sent += (int)wrote;
                if (r->request_sent >= r->request_len)
                {
                    r->phase = E2E_HTTP_PHASE_READING;
                }
                else if (wrote == 0)
                {
                    http_wait(r, AZ_AMQP_TRANSPORT_STATUS_WANT_WRITE);
                }
                return 0;
            }
            if (status == AZ_AMQP_TRANSPORT_STATUS_WANT_READ
                || status == AZ_AMQP_TRANSPORT_STATUS_WANT_WRITE)
            {
                http_wait(r, status);
                return 0;
            }
            r->phase = E2E_HTTP_PHASE_FAILED;
            r->err = "http: send failed";
            return -1;
        }

        case E2E_HTTP_PHASE_READING:
        {
            int cap = E2E_HTTP_RESPONSE_MAX - r->response_len;
            if (cap <= 0)
            {
                http_parse_headers(r);
                r->phase = E2E_HTTP_PHASE_DONE;
                return 1;
            }
            size_t got = 0;
            az_span dst = az_span_create(r->response + r->response_len, cap);
            status = r->transport.vtable->read(&r->transport, dst, &got);
            if (status == AZ_AMQP_TRANSPORT_STATUS_OK)
            {
                r->response_len += (int)got;
                http_parse_headers(r);
                if (r->headers_done && r->content_length >= 0
                    && (r->response_len - r->body_start) >= r->content_length)
                {
                    r->phase = E2E_HTTP_PHASE_DONE;
                    return 1;
                }
                return 0;
            }
            if (status == AZ_AMQP_TRANSPORT_STATUS_WANT_READ
                || status == AZ_AMQP_TRANSPORT_STATUS_WANT_WRITE)
            {
                http_wait(r, status);
                return 0;
            }
            if (status == AZ_AMQP_TRANSPORT_STATUS_CLOSED)
            {
                http_parse_headers(r);
                r->phase = E2E_HTTP_PHASE_DONE;
                return 1;
            }
            r->phase = E2E_HTTP_PHASE_FAILED;
            r->err = "http: receive failed";
            return -1;
        }

        case E2E_HTTP_PHASE_DONE:
            return 1;

        case E2E_HTTP_PHASE_FAILED:
        default:
            return -1;
    }
}

int e2e_http_status(const e2e_http_request* r) { return r->http_status; }

const uint8_t* e2e_http_body(const e2e_http_request* r, int* out_len)
{
    int available = r->response_len - r->body_start;
    if (!r->headers_done || available < 0)
    {
        available = 0;
    }
    if (r->content_length >= 0 && r->content_length < available)
    {
        available = r->content_length;
    }
    if (out_len != NULL)
    {
        *out_len = available;
    }
    return r->response + r->body_start;
}

const char* e2e_http_error(const e2e_http_request* r) { return r->err; }

void e2e_http_end(e2e_http_request* r)
{
    if (r->transport.vtable == NULL || r->transport.vtable->close == NULL)
    {
        return;
    }
    for (int i = 0; i < 20; i++)
    {
        az_amqp_transport_status status = r->transport.vtable->close(&r->transport);
        if (status != AZ_AMQP_TRANSPORT_STATUS_WANT_READ
            && status != AZ_AMQP_TRANSPORT_STATUS_WANT_WRITE)
        {
            break;
        }
        http_wait(r, status);
    }
}
