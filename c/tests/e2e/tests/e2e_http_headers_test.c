// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Response header parsing of the e2e HTTPS client: status, Content-Length and
 * Retry-After, wherever they appear in the header block. No network is used. */

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

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_last_header_is_parsed),
    cmocka_unit_test(test_unusable_retry_after_is_zero),
    cmocka_unit_test(test_no_body_statuses_complete_at_the_headers),
    cmocka_unit_test(test_incomplete_headers_are_not_parsed),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
