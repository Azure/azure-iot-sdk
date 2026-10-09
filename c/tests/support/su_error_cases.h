// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */

#ifndef SU_ERROR_CASES_H
#define SU_ERROR_CASES_H

#include <stdio.h>

#include "../../src/features/su/internal/su_protocol_internal.h"

typedef enum
{
  SU_ERROR_INFO,
  SU_ERROR_MESSAGE,
  SU_ERROR_MESSAGE_NUMERIC,
  SU_ERROR_PRECEDENCE,
  SU_ERROR_NUMERIC,
  SU_ERROR_BODYLESS
} su_error_body;

typedef struct
{
  az_iot_su_operation operation;
  const char* code;
  int32_t status;
  int32_t numeric;
  az_iot_su_error_action action;
  su_error_body shape;
  const char* query;
  uint32_t delay_ms;
} su_error_case;

/* Expected actions are contract inputs, not outputs from the classifier.
 * Numeric buckets below come from su_protocol_internal.h, not HTTP * 1000.
 * Named cases deliberately use string-only bodies or contradictory signals:
 * no unverified per-name DPS numeric mapping is implied. */
#define SU_NAMED_SHAPES(X, code, route, op, status, action, numeric, query, delay)          \
  X(code##_##route##_info, op, #code, status, 0, action, SU_ERROR_INFO, query, delay)       \
  X(code##_##route##_message, op, #code, status, 0, action, SU_ERROR_MESSAGE, query, delay) \
  X(code##_##route##_message_numeric,                                                       \
    op,                                                                                     \
    #code,                                                                                  \
    400,                                                                                    \
    numeric,                                                                                \
    action,                                                                                 \
    SU_ERROR_MESSAGE_NUMERIC,                                                               \
    query,                                                                                  \
    delay)                                                                                  \
  X(code##_##route##_status_precedence,                                                     \
    op,                                                                                     \
    #code,                                                                                  \
    status >= 500 ? 400 : 503,                                                              \
    0,                                                                                      \
    action,                                                                                 \
    SU_ERROR_MESSAGE,                                                                       \
    query,                                                                                  \
    delay)                                                                                  \
  X(code##_##route##_precedence, op, #code, 400, numeric, action, SU_ERROR_PRECEDENCE, query, delay)

#define SU_ALL_ROUTES(X, code, status, action, numeric)                                        \
  SU_NAMED_SHAPES(X, code, regular, AZ_IOT_SU_OP_GET_UPDATE, status, action, numeric, "", 0)   \
  SU_NAMED_SHAPES(                                                                             \
      X, code, onboarding, AZ_IOT_SU_OP_GET_ONBOARDING_UPDATE, status, action, numeric, "", 0) \
  SU_NAMED_SHAPES(X, code, report, AZ_IOT_SU_OP_REPORT_STATUS, status, action, numeric, "", 0)

#define SU_FETCH_ROUTES(X, code, status, action, numeric)                                    \
  SU_NAMED_SHAPES(X, code, regular, AZ_IOT_SU_OP_GET_UPDATE, status, action, numeric, "", 0) \
  SU_NAMED_SHAPES(                                                                           \
      X, code, onboarding, AZ_IOT_SU_OP_GET_ONBOARDING_UPDATE, status, action, numeric, "", 0)

#define SU_THROTTLE_DELAY(X, route, op, label, query, delay) \
  X(throttle_##route##_typed_##label,                        \
    op,                                                      \
    "THROTTLED",                                             \
    429,                                                     \
    AZ_IOT_SU_ERR_THROTTLED,                                 \
    AZ_IOT_SU_ERROR_ACTION_RETRY_AFTER,                      \
    SU_ERROR_INFO,                                           \
    query,                                                   \
    delay)                                                   \
  X(throttle_##route##_bodyless_##label,                     \
    op,                                                      \
    "",                                                      \
    429,                                                     \
    0,                                                       \
    AZ_IOT_SU_ERROR_ACTION_RETRY_AFTER,                      \
    SU_ERROR_BODYLESS,                                       \
    query,                                                   \
    delay)

#define SU_THROTTLE_ROUTE(X, route, op)                             \
  SU_THROTTLE_DELAY(X, route, op, supplied, "&retry-after=2", 2000) \
  SU_THROTTLE_DELAY(X, route, op, missing, "", 0)                   \
  SU_THROTTLE_DELAY(X, route, op, zero, "&retry-after=0", 0)        \
  SU_THROTTLE_DELAY(X, route, op, invalid, "&retry-after=3s", 0)    \
  SU_THROTTLE_DELAY(X, route, op, negative, "&retry-after=-1", 0)   \
  SU_THROTTLE_DELAY(X, route, op, date, "&retry-after=Wed, 21 Oct 2015 07:28:00 GMT", 0)

#define SU_FALLBACK_ROUTE(X, route, op, conflict_action) \
  X(numeric_bad_request_##route,                         \
    op,                                                  \
    "",                                                  \
    503,                                                 \
    AZ_IOT_SU_ERR_GENERIC_BAD_REQUEST,                   \
    AZ_IOT_SU_ERROR_ACTION_FATAL,                        \
    SU_ERROR_NUMERIC,                                    \
    "",                                                  \
    0)                                                   \
  X(numeric_conflict_##route,                            \
    op,                                                  \
    "",                                                  \
    503,                                                 \
    AZ_IOT_SU_ERR_GENERIC_CONFLICT,                      \
    conflict_action,                                     \
    SU_ERROR_NUMERIC,                                    \
    "",                                                  \
    0)                                                   \
  X(numeric_server_##route,                              \
    op,                                                  \
    "",                                                  \
    400,                                                 \
    500001,                                              \
    AZ_IOT_SU_ERROR_ACTION_RETRY,                        \
    SU_ERROR_NUMERIC,                                    \
    "",                                                  \
    0)                                                   \
  X(numeric_throttle_##route,                            \
    op,                                                  \
    "",                                                  \
    400,                                                 \
    AZ_IOT_SU_ERR_THROTTLED,                             \
    AZ_IOT_SU_ERROR_ACTION_RETRY_AFTER,                  \
    SU_ERROR_NUMERIC,                                    \
    "",                                                  \
    0)                                                   \
  X(numeric_upstream_##route,                            \
    op,                                                  \
    "",                                                  \
    400,                                                 \
    AZ_IOT_SU_ERR_SERVICE_UNAVAILABLE,                   \
    AZ_IOT_SU_ERROR_ACTION_RETRY,                        \
    SU_ERROR_NUMERIC,                                    \
    "",                                                  \
    0)                                                   \
  X(open_terminal_##route,                               \
    op,                                                  \
    "FUTURE_CODE",                                       \
    400,                                                 \
    AZ_IOT_SU_ERR_ARGUMENT_INVALID,                      \
    AZ_IOT_SU_ERROR_ACTION_FATAL,                        \
    SU_ERROR_INFO,                                       \
    "",                                                  \
    0)                                                   \
  X(open_transient_##route,                              \
    op,                                                  \
    "FUTURE_CODE",                                       \
    400,                                                 \
    AZ_IOT_SU_ERR_SERVICE_UNAVAILABLE,                   \
    AZ_IOT_SU_ERROR_ACTION_RETRY,                        \
    SU_ERROR_INFO,                                       \
    "",                                                  \
    0)                                                   \
  X(bodyless_upstream_##route,                           \
    op,                                                  \
    "",                                                  \
    503,                                                 \
    0,                                                   \
    AZ_IOT_SU_ERROR_ACTION_RETRY,                        \
    SU_ERROR_BODYLESS,                                   \
    "",                                                  \
    0)

#define SU_ERROR_CASES(X)                                                                          \
  SU_ALL_ROUTES(                                                                                   \
      X, UNSUPPORTED_API_VERSION, 400, AZ_IOT_SU_ERROR_ACTION_FATAL, AZ_IOT_SU_ERR_SERVER_ERROR)   \
  SU_ALL_ROUTES(                                                                                   \
      X, UNSUPPORTED_AGENT_PROFILE, 400, AZ_IOT_SU_ERROR_ACTION_FATAL, AZ_IOT_SU_ERR_SERVER_ERROR) \
  SU_ALL_ROUTES(                                                                                   \
      X,                                                                                           \
      INVALID_COMPATIBILITY_PROPERTIES,                                                            \
      400,                                                                                         \
      AZ_IOT_SU_ERROR_ACTION_FATAL,                                                                \
      AZ_IOT_SU_ERR_SERVER_ERROR)                                                                  \
  SU_ALL_ROUTES(X, INVALID_REQUEST, 400, AZ_IOT_SU_ERROR_ACTION_FATAL, AZ_IOT_SU_ERR_SERVER_ERROR) \
  SU_FETCH_ROUTES(                                                                                 \
      X,                                                                                           \
      UNKNOWN_AGENT_INFO_VERSION,                                                                  \
      400,                                                                                         \
      AZ_IOT_SU_ERROR_ACTION_RESEND_AGENT_INFO,                                                    \
      AZ_IOT_SU_ERR_SERVER_ERROR)                                                                  \
  SU_FETCH_ROUTES(                                                                                 \
      X,                                                                                           \
      OUTDATED_AGENT_INFO,                                                                         \
      400,                                                                                         \
      AZ_IOT_SU_ERROR_ACTION_RESEND_AGENT_INFO,                                                    \
      AZ_IOT_SU_ERR_SERVER_ERROR)                                                                  \
  SU_ALL_ROUTES(                                                                                   \
      X,                                                                                           \
      UPDATE_ACCOUNT_NOT_LINKED,                                                                   \
      409,                                                                                         \
      AZ_IOT_SU_ERROR_ACTION_PROCEED,                                                              \
      AZ_IOT_SU_ERR_SERVER_ERROR)                                                                  \
  SU_NAMED_SHAPES(                                                                                 \
      X,                                                                                           \
      UNKNOWN_WORKFLOW_ID,                                                                         \
      report,                                                                                      \
      AZ_IOT_SU_OP_REPORT_STATUS,                                                                  \
      400,                                                                                         \
      AZ_IOT_SU_ERROR_ACTION_FATAL,                                                                \
      AZ_IOT_SU_ERR_SERVER_ERROR,                                                                  \
      "",                                                                                          \
      0)                                                                                           \
  SU_NAMED_SHAPES(                                                                                 \
      X,                                                                                           \
      REPORT_CONFLICT,                                                                             \
      report,                                                                                      \
      AZ_IOT_SU_OP_REPORT_STATUS,                                                                  \
      409,                                                                                         \
      AZ_IOT_SU_ERROR_ACTION_FATAL,                                                                \
      AZ_IOT_SU_ERR_SERVER_ERROR,                                                                  \
      "",                                                                                          \
      0)                                                                                           \
  SU_ALL_ROUTES(                                                                                   \
      X, INTERNAL_SERVER_ERROR, 500, AZ_IOT_SU_ERROR_ACTION_RETRY, AZ_IOT_SU_ERR_ARGUMENT_INVALID) \
  SU_ALL_ROUTES(                                                                                   \
      X, UPSTREAM_UNAVAILABLE, 503, AZ_IOT_SU_ERROR_ACTION_RETRY, AZ_IOT_SU_ERR_ARGUMENT_INVALID)  \
  SU_NAMED_SHAPES(                                                                                 \
      X,                                                                                           \
      UPSTREAM_UNAVAILABLE,                                                                        \
      regular_delayed,                                                                             \
      AZ_IOT_SU_OP_GET_UPDATE,                                                                     \
      503,                                                                                         \
      AZ_IOT_SU_ERROR_ACTION_RETRY,                                                                \
      AZ_IOT_SU_ERR_ARGUMENT_INVALID,                                                              \
      "&retry-after=2",                                                                            \
      2000)                                                                                        \
  SU_NAMED_SHAPES(                                                                                 \
      X,                                                                                           \
      UPSTREAM_UNAVAILABLE,                                                                        \
      onboarding_delayed,                                                                          \
      AZ_IOT_SU_OP_GET_ONBOARDING_UPDATE,                                                          \
      503,                                                                                         \
      AZ_IOT_SU_ERROR_ACTION_RETRY,                                                                \
      AZ_IOT_SU_ERR_ARGUMENT_INVALID,                                                              \
      "&retry-after=2",                                                                            \
      2000)                                                                                        \
  SU_NAMED_SHAPES(                                                                                 \
      X,                                                                                           \
      UPSTREAM_UNAVAILABLE,                                                                        \
      report_delayed,                                                                              \
      AZ_IOT_SU_OP_REPORT_STATUS,                                                                  \
      503,                                                                                         \
      AZ_IOT_SU_ERROR_ACTION_RETRY,                                                                \
      AZ_IOT_SU_ERR_ARGUMENT_INVALID,                                                              \
      "&retry-after=2",                                                                            \
      2000)                                                                                        \
  SU_THROTTLE_ROUTE(X, regular, AZ_IOT_SU_OP_GET_UPDATE)                                           \
  SU_THROTTLE_ROUTE(X, onboarding, AZ_IOT_SU_OP_GET_ONBOARDING_UPDATE)                             \
  SU_THROTTLE_ROUTE(X, report, AZ_IOT_SU_OP_REPORT_STATUS)                                         \
  X(typed_conflict_documented_bucket,                                                              \
    AZ_IOT_SU_OP_REPORT_STATUS,                                                                    \
    "REPORT_CONFLICT",                                                                             \
    409,                                                                                           \
    AZ_IOT_SU_ERR_GENERIC_CONFLICT,                                                                \
    AZ_IOT_SU_ERROR_ACTION_FATAL,                                                                  \
    SU_ERROR_INFO,                                                                                 \
    "",                                                                                            \
    0)                                                                                             \
  X(numeric_resend_regular,                                                                        \
    AZ_IOT_SU_OP_GET_UPDATE,                                                                       \
    "",                                                                                            \
    503,                                                                                           \
    AZ_IOT_SU_ERR_AGENT_INFO_RESEND_REQUIRED,                                                      \
    AZ_IOT_SU_ERROR_ACTION_RESEND_AGENT_INFO,                                                      \
    SU_ERROR_NUMERIC,                                                                              \
    "",                                                                                            \
    0)                                                                                             \
  X(numeric_resend_onboarding,                                                                     \
    AZ_IOT_SU_OP_GET_ONBOARDING_UPDATE,                                                            \
    "",                                                                                            \
    503,                                                                                           \
    AZ_IOT_SU_ERR_AGENT_INFO_RESEND_REQUIRED,                                                      \
    AZ_IOT_SU_ERROR_ACTION_RESEND_AGENT_INFO,                                                      \
    SU_ERROR_NUMERIC,                                                                              \
    "",                                                                                            \
    0)                                                                                             \
  SU_FALLBACK_ROUTE(X, regular, AZ_IOT_SU_OP_GET_UPDATE, AZ_IOT_SU_ERROR_ACTION_PROCEED)           \
  SU_FALLBACK_ROUTE(                                                                               \
      X, onboarding, AZ_IOT_SU_OP_GET_ONBOARDING_UPDATE, AZ_IOT_SU_ERROR_ACTION_PROCEED)           \
  SU_FALLBACK_ROUTE(X, report, AZ_IOT_SU_OP_REPORT_STATUS, AZ_IOT_SU_ERROR_ACTION_ALREADY_REPORTED)

/* A report has no agentInfo fields; corrective resends apply only to fetches. */

#define SU_DEFINE_ERROR_CASE(name, op, code, status, numeric, action, shape, query, delay) \
  static const su_error_case name = { op, code, status, numeric, action, shape, query, delay };
SU_ERROR_CASES(SU_DEFINE_ERROR_CASE)
#undef SU_DEFINE_ERROR_CASE

static size_t su_error_body_build(const su_error_case* row, char* body, size_t capacity)
{
  int n = 0;
  switch (row->shape)
  {
    case SU_ERROR_INFO:
      if (row->numeric == 0)
      {
        n = snprintf(
            body,
            capacity,
            "{\"message\":\"diagnostic prose\",\"trackingId\":\"catalog-track\","
            "\"info\":{\"aduErrorCode\":\"%s\"}}",
            row->code);
        break;
      }
      n = snprintf(
          body,
          capacity,
          "{\"errorCode\":%d,\"message\":\"diagnostic prose\",\"trackingId\":\"catalog-track\","
          "\"info\":{\"aduErrorCode\":\"%s\"}}",
          (int)row->numeric,
          row->code);
      break;
    case SU_ERROR_MESSAGE:
      n = snprintf(
          body, capacity, "{\"message\":\"%s\",\"trackingId\":\"catalog-track\"}", row->code);
      break;
    case SU_ERROR_MESSAGE_NUMERIC:
      n = snprintf(
          body,
          capacity,
          "{\"message\":\"%s\",\"errorCode\":%d,\"trackingId\":\"catalog-track\"}",
          row->code,
          (int)row->numeric);
      break;
    case SU_ERROR_PRECEDENCE:
      n = snprintf(
          body,
          capacity,
          "{\"message\":\"INVALID_REQUEST\",\"errorCode\":%d,\"trackingId\":\"catalog-track\","
          "\"info\":{\"aduErrorCode\":\"%s\"}}",
          (int)row->numeric,
          row->code);
      break;
    case SU_ERROR_NUMERIC:
      n = snprintf(
          body, capacity, "{\"errorCode\":%d,\"trackingId\":\"catalog-track\"}", (int)row->numeric);
      break;
    case SU_ERROR_BODYLESS:
      body[0] = '\0';
      return 0;
  }
  assert_true(n > 0 && (size_t)n < capacity);
  return (size_t)n;
}

#endif
