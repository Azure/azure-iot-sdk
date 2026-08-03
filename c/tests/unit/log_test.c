// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/* SPDX-License-Identifier: MIT */
/* Unit tests for the logging facade.
 *
 * The facade is the only way SDK code and MQTT adapters are allowed to trace,
 * so the properties that matter are: nothing escapes when no sink is
 * registered, the minimum level is honoured before any formatting happens, and
 * a message longer than the buffer is truncated rather than dropped.
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>

#include "azure/iot/az_iot_log.h"

typedef struct capture
{
    int count;
    az_iot_log_level level;
    char msg[AZ_IOT_LOG_MESSAGE_MAX];
    const char* file;
    int line;
} capture;

static capture g_capture;

static void capture_sink(
    void* user_ctx, az_iot_log_level level, const char* file, int line, const char* msg)
{
    capture* c = (capture*)user_ctx;
    c->count++;
    c->level = level;
    c->file = file;
    c->line = line;
    if (msg != NULL)
    {
        size_t n = strlen(msg);
        if (n >= sizeof(c->msg)) n = sizeof(c->msg) - 1;
        memcpy(c->msg, msg, n);
        c->msg[n] = '\0';
    }
}

static int setup(void** state)
{
    memset(&g_capture, 0, sizeof(g_capture));
    az_iot_log_sink sink;
    sink.sink = capture_sink;
    sink.user_ctx = &g_capture;
    sink.min_level = AZ_IOT_LOG_LEVEL_TRACE;
    az_iot_log_set_global_sink(&sink);
    *state = &g_capture;
    return 0;
}

static int teardown(void** state)
{
    (void)state;
    az_iot_log_set_global_sink(NULL);
    return 0;
}

static void nothing_is_emitted_without_a_sink(void** state)
{
    (void)state;
    az_iot_log_set_global_sink(NULL);
    memset(&g_capture, 0, sizeof(g_capture));

    assert_false(az_iot_log_is_enabled(AZ_IOT_LOG_LEVEL_ERROR));
    AZ_IOT_LOG_ERROR("dropped");
    AZ_IOT_LOG_ERRORF("also %s", "dropped");
    assert_int_equal(g_capture.count, 0);
}

static void plain_and_formatted_messages_reach_the_sink(void** state)
{
    (void)state;
    AZ_IOT_LOG_WARN("plain message");
    assert_int_equal(g_capture.count, 1);
    assert_int_equal(g_capture.level, AZ_IOT_LOG_LEVEL_WARN);
    assert_string_equal(g_capture.msg, "plain message");
    assert_non_null(g_capture.file);
    assert_true(g_capture.line > 0);

    AZ_IOT_LOG_DEBUGF("device %s port %u", "dev-1", 8883u);
    assert_int_equal(g_capture.count, 2);
    assert_int_equal(g_capture.level, AZ_IOT_LOG_LEVEL_DEBUG);
    assert_string_equal(g_capture.msg, "device dev-1 port 8883");
}

static void every_level_is_routed(void** state)
{
    (void)state;
    AZ_IOT_LOG_TRACE("t");
    assert_int_equal(g_capture.level, AZ_IOT_LOG_LEVEL_TRACE);
    AZ_IOT_LOG_DEBUG("d");
    assert_int_equal(g_capture.level, AZ_IOT_LOG_LEVEL_DEBUG);
    AZ_IOT_LOG_INFO("i");
    assert_int_equal(g_capture.level, AZ_IOT_LOG_LEVEL_INFO);
    AZ_IOT_LOG_WARN("w");
    assert_int_equal(g_capture.level, AZ_IOT_LOG_LEVEL_WARN);
    AZ_IOT_LOG_ERROR("e");
    assert_int_equal(g_capture.level, AZ_IOT_LOG_LEVEL_ERROR);
    assert_int_equal(g_capture.count, 5);
}

static void levels_below_the_minimum_are_dropped(void** state)
{
    (void)state;
    az_iot_log_sink sink;
    sink.sink = capture_sink;
    sink.user_ctx = &g_capture;
    sink.min_level = AZ_IOT_LOG_LEVEL_WARN;
    az_iot_log_set_global_sink(&sink);
    memset(&g_capture, 0, sizeof(g_capture));

    assert_false(az_iot_log_is_enabled(AZ_IOT_LOG_LEVEL_INFO));
    assert_true(az_iot_log_is_enabled(AZ_IOT_LOG_LEVEL_WARN));

    AZ_IOT_LOG_DEBUG("no");
    AZ_IOT_LOG_INFOF("no %d", 1);
    assert_int_equal(g_capture.count, 0);

    AZ_IOT_LOG_WARN("yes");
    assert_int_equal(g_capture.count, 1);
}

static void an_oversized_message_is_truncated_not_dropped(void** state)
{
    (void)state;
    char big[AZ_IOT_LOG_MESSAGE_MAX * 2];
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';

    AZ_IOT_LOG_ERRORF("%s", big);

    /* The diagnostic still arrives, just shortened to the buffer. */
    assert_int_equal(g_capture.count, 1);
    assert_int_equal(strlen(g_capture.msg), AZ_IOT_LOG_MESSAGE_MAX - 1);
}

static void a_precision_bounded_argument_is_not_over_read(void** state)
{
    (void)state;
    /* The DPS failure path logs a response body that is not NUL-terminated,
     * using a precision to bound it. */
    const char payload[] = { 'a', 'b', 'c', 'd' };
    AZ_IOT_LOG_ERRORF("body: %.*s", 2, payload);
    assert_string_equal(g_capture.msg, "body: ab");
}

static void null_message_and_file_reach_the_sink_as_text(void** state)
{
    (void)state;
    /* emit is public, so a sink can be handed a NULL that the SDK never
     * produced itself. Sinks format both with "%s" -- the built-in stderr one
     * does -- so the substitution has to happen before the call, not in the
     * sink. */
    az_iot_log_emit(AZ_IOT_LOG_LEVEL_ERROR, NULL, 7, NULL);
    assert_int_equal(g_capture.count, 1);
    assert_string_equal(g_capture.msg, "");
    assert_string_equal(g_capture.file, "?");
    assert_int_equal(g_capture.line, 7);

    az_iot_log_emitf(AZ_IOT_LOG_LEVEL_WARN, NULL, 9, "value %d", 3);
    assert_int_equal(g_capture.count, 2);
    assert_string_equal(g_capture.msg, "value 3");
    assert_string_equal(g_capture.file, "?");

    /* A NULL format has no message to build, so nothing is emitted. */
    az_iot_log_emitf(AZ_IOT_LOG_LEVEL_WARN, __FILE__, __LINE__, NULL);
    assert_int_equal(g_capture.count, 2);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(nothing_is_emitted_without_a_sink, setup, teardown),
        cmocka_unit_test_setup_teardown(plain_and_formatted_messages_reach_the_sink, setup, teardown),
        cmocka_unit_test_setup_teardown(every_level_is_routed, setup, teardown),
        cmocka_unit_test_setup_teardown(levels_below_the_minimum_are_dropped, setup, teardown),
        cmocka_unit_test_setup_teardown(an_oversized_message_is_truncated_not_dropped, setup, teardown),
        cmocka_unit_test_setup_teardown(a_precision_bounded_argument_is_not_over_read, setup, teardown),
        cmocka_unit_test_setup_teardown(null_message_and_file_reach_the_sink_as_text, setup, teardown),
    };
    return cmocka_run_group_tests_name("log", tests, NULL, NULL);
}
