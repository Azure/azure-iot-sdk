// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Response header parsing of the e2e HTTPS client (status, Content-Length and
 * Retry-After, wherever they appear in the header block) and its retry decision.
 * No network is used. */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>

#include <cmocka.h>

#include <stdlib.h>
#include <string.h>

#include "e2e_http.h"

/** @brief Parses @p response as a fresh request's received bytes. */
static e2e_http_request* parse(const char* response)
{
  e2e_http_request* r = (e2e_http_request*)calloc(1, sizeof(*r));
  assert_non_null(r);
  size_t len = strlen(response);
  assert_true(len <= sizeof(r->response));
  memcpy(r->response, response, len);
  r->response_len = (int)len;
  r->content_length = -1;
  e2e_http_parse_headers(r);
  return r;
}

static void test_last_header_is_parsed(void** state)
{
  (void)state;
  e2e_http_request* r
      = parse("HTTP/1.1 429 Too Many Requests\r\nContent-Length: 0\r\nRetry-After: 10\r\n\r\n");
  assert_true(r->headers_done);
  assert_int_equal(429, e2e_http_status(r));
  assert_int_equal(0, r->content_length);
  assert_int_equal(10, e2e_http_retry_after(r));
  free(r);

  r = parse("HTTP/1.1 200 OK\r\nretry-after:\t3 \r\ncontent-length: 2\r\n\r\n{}");
  assert_int_equal(2, r->content_length);
  assert_int_equal(3, e2e_http_retry_after(r));
  free(r);
}

static void test_unusable_retry_after_is_zero(void** state)
{
  (void)state;
  static const char* const responses[] = {
    "HTTP/1.1 503 x\r\nRetry-After: Wed, 21 Oct 2015 07:28:00 GMT\r\n\r\n",
    "HTTP/1.1 503 x\r\nRetry-After: 86401\r\n\r\n",
    "HTTP/1.1 503 x\r\nRetry-After: 99999999999999999999\r\n\r\n",
    "HTTP/1.1 503 x\r\nRetry-After:\r\n\r\n",
    "HTTP/1.1 503 x\r\nRetry-After: 5x\r\n\r\n",
    "HTTP/1.1 503 x\r\nContent-Length: 0\r\n\r\n",
  };
  for (size_t i = 0; i < sizeof(responses) / sizeof(responses[0]); ++i)
  {
    e2e_http_request* r = parse(responses[i]);
    assert_true(r->headers_done);
    assert_int_equal(503, e2e_http_status(r));
    assert_int_equal(0, e2e_http_retry_after(r));
    free(r);
  }
}

static void test_no_body_statuses_complete_at_the_headers(void** state)
{
  (void)state;
  /* 204 and 304 have no body: complete without Content-Length or a close. */
  e2e_http_request* r = parse("HTTP/1.1 204 No Content\r\nx-ms-request-id: 1\r\n\r\n");
  assert_true(e2e_http_response_complete(r));
  free(r);
  r = parse("HTTP/1.1 304 Not Modified\r\n\r\n");
  assert_true(e2e_http_response_complete(r));
  free(r);
  /* Other statuses wait for Content-Length bytes, or for the close when it is absent. */
  r = parse("HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\n{}");
  assert_false(e2e_http_response_complete(r));
  free(r);
  r = parse("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n{}");
  assert_true(e2e_http_response_complete(r));
  free(r);
  r = parse("HTTP/1.1 200 OK\r\n\r\n{}");
  assert_false(e2e_http_response_complete(r));
  free(r);
  r = parse("HTTP/1.1 204 No Content\r\n");
  assert_false(e2e_http_response_complete(r));
  free(r);
}

static void test_incomplete_headers_are_not_parsed(void** state)
{
  (void)state;
  e2e_http_request* r = parse("HTTP/1.1 429 x\r\nRetry-After: 10\r\n");
  assert_false(r->headers_done);
  assert_int_equal(0, e2e_http_retry_after(r));
  free(r);
}

static void test_status_line_is_parsed_before_the_headers_end(void** state)
{
  (void)state;
  e2e_http_request* r = parse("HTTP/1.1 403 Forbidden\r\n");
  assert_false(r->headers_done);
  assert_int_equal(403, e2e_http_status(r));
  free(r);

  /* Not before the status line itself is complete. */
  r = parse("HTTP/1.1 403 Forb");
  assert_int_equal(0, e2e_http_status(r));
  free(r);
}

static az_amqp_transport_status read_closed(
    az_amqp_transport* transport,
    az_span destination,
    size_t* out_bytes)
{
  (void)transport;
  (void)destination;
  *out_bytes = 0;
  return AZ_AMQP_TRANSPORT_STATUS_CLOSED;
}

/** @brief A sent request whose connection closes after @p received arrived. */
static e2e_http_request* closed_after(const char* received)
{
  static const az_amqp_transport_vtable closing = { NULL, NULL, read_closed, NULL, NULL, NULL };
  e2e_http_request* r = parse(received);
  r->transport.vtable = &closing;
  r->phase = E2E_HTTP_PHASE_READING;
  r->request_len = 10;
  r->request_sent = 10;
  return r;
}

static void test_close_before_the_headers_end_fails(void** state)
{
  (void)state;
  /* Nothing received: a failure an idempotent request may retry. */
  e2e_http_request* r = closed_after("");
  assert_int_equal(-1, e2e_http_poll(r));
  assert_non_null(e2e_http_error(r));
  assert_int_equal(0, e2e_http_status(r));
  assert_true(e2e_http_retry_is_safe(r, true));
  free(r);

  /* A status line only: a failure that keeps the status and is not retried. */
  r = closed_after("HTTP/1.1 500 x\r\n");
  assert_int_equal(-1, e2e_http_poll(r));
  assert_int_equal(500, e2e_http_status(r));
  assert_false(e2e_http_retry_is_safe(r, true));
  free(r);

  /* The whole header block: complete. */
  r = closed_after("HTTP/1.1 200 OK\r\n\r\n{}");
  assert_int_equal(1, e2e_http_poll(r));
  assert_int_equal(200, e2e_http_status(r));
  free(r);
}

static void test_retry_is_safe_only_before_a_response(void** state)
{
  (void)state;
  e2e_http_request* r = (e2e_http_request*)calloc(1, sizeof(*r));
  assert_non_null(r);
  r->request_len = 100;

  /* Nothing sent: any method may be sent again. */
  r->phase = E2E_HTTP_PHASE_CONNECTING;
  assert_true(e2e_http_retry_is_safe(r, false));
  r->phase = E2E_HTTP_PHASE_FAILED;
  assert_true(e2e_http_retry_is_safe(r, false));

  /* Partly or fully sent without a response: only an idempotent request. */
  r->request_sent = 40;
  assert_false(e2e_http_retry_is_safe(r, false));
  assert_true(e2e_http_retry_is_safe(r, true));
  r->phase = E2E_HTTP_PHASE_READING;
  r->request_sent = 100;
  assert_false(e2e_http_retry_is_safe(r, false));
  assert_true(e2e_http_retry_is_safe(r, true));

  /* A status line arrived, or the response is complete: never. */
  r->http_status = 503;
  assert_false(e2e_http_retry_is_safe(r, true));
  r->http_status = 0;
  r->phase = E2E_HTTP_PHASE_DONE;
  assert_false(e2e_http_retry_is_safe(r, true));
  free(r);
}

static void test_phase_names(void** state)
{
  (void)state;
  assert_string_equal("connecting", e2e_http_phase_name(E2E_HTTP_PHASE_CONNECTING));
  assert_string_equal("sending", e2e_http_phase_name(E2E_HTTP_PHASE_WRITING));
  assert_string_equal("reading", e2e_http_phase_name(E2E_HTTP_PHASE_READING));
  assert_string_equal("done", e2e_http_phase_name(E2E_HTTP_PHASE_DONE));
  assert_string_equal("failed", e2e_http_phase_name(E2E_HTTP_PHASE_FAILED));
  assert_string_equal("failed", e2e_http_phase_name((e2e_http_phase)99));
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_last_header_is_parsed),
    cmocka_unit_test(test_unusable_retry_after_is_zero),
    cmocka_unit_test(test_no_body_statuses_complete_at_the_headers),
    cmocka_unit_test(test_incomplete_headers_are_not_parsed),
    cmocka_unit_test(test_status_line_is_parsed_before_the_headers_end),
    cmocka_unit_test(test_close_before_the_headers_end_fails),
    cmocka_unit_test(test_retry_is_safe_only_before_a_response),
    cmocka_unit_test(test_phase_names),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
