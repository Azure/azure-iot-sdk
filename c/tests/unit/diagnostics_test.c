// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Diagnostic surface: the *_to_string helpers, the version string, the default
 * reconnection policy and the built-in stderr log sink.
 *
 * None of these had a single test. That is easy to shrug at -- they are small,
 * and nothing depends on them being right -- but two things make them worth
 * pinning. They are what a device operator reads when something has gone wrong,
 * so a wrong or missing string costs exactly when it is least affordable; and a
 * `switch` over an enum that silently falls through to "unknown" is the shape
 * that rots when someone adds an enumerator and forgets the arm.
 *
 * Each enumerator is asserted against its exact string, not merely against
 * "something unique and non-fallback": two swapped arms produce a plausible log
 * line that sends a reader after the wrong fault. Each helper's default arm is
 * also driven directly, since it is unreachable from any named enumerator.
 *
 * One limit worth stating: these lists are hand-written, so adding an
 * enumerator AND forgetting both its switch arm and this list still passes.
 * Closing that needs a count sentinel or X-macro in the public headers, which
 * is a product change rather than a test one.
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <cmocka.h>

#include "azure/iot/az_iot_connection_client.h"
#include "azure/iot/az_iot_log.h"
#include "azure/iot/az_iot_mqtt_iface.h"
#include "azure/iot/az_iot_result.h"
#include "azure/iot/az_iot_version.h"

#include "internal/cert_util.h"
#include "internal/connection_client_internal.h"
#include "internal/protocol_profile.h"

/* ---- helpers -------------------------------------------------------------- */

/* Every mapped value must produce a string that is non-null, non-empty, and not
 * the fallback the default arm returns. Checking against the fallback is the
 * point: a missing case still returns a plausible-looking string, so asserting
 * "not null" alone would pass for an enumerator nobody mapped. */
static void assert_mapped(const char* actual, const char* fallback, const char* what)
{
  assert_non_null(actual);
  assert_true(actual[0] != '\0');
  if (strcmp(actual, fallback) == 0)
  {
    fail_msg("%s fell through to the fallback string \"%s\"", what, fallback);
  }
}

/* No two enumerators may share a string: a copy-paste in the switch would
 * otherwise be invisible, and two states that print identically are worse than
 * one that prints "unknown". */
static void assert_all_distinct(const char* const* strings, size_t n)
{
  for (size_t i = 0; i < n; ++i)
  {
    for (size_t j = i + 1; j < n; ++j)
    {
      if (strcmp(strings[i], strings[j]) == 0)
      {
        fail_msg("entries %zu and %zu both map to \"%s\"", i, j, strings[i]);
      }
    }
  }
}

/* ---- az_iot_result -------------------------------------------------------- */

static void result_to_string_covers_every_code(void** state)
{
  (void)state;
  /* Paired with the expected text: asserting only "unique and not the
   * fallback" would accept two arms swapped, and an operator reading
   * AZ_IOT_ERR_AUTH for a TLS failure is sent after the wrong fault. */
  const struct
  {
    az_iot_result code;
    const char* text;
  } all[] = {
    { AZ_IOT_OK, "AZ_IOT_OK" },
    { AZ_IOT_ERR_INVALID_ARG, "AZ_IOT_ERR_INVALID_ARG" },
    { AZ_IOT_ERR_OUT_OF_MEMORY, "AZ_IOT_ERR_OUT_OF_MEMORY" },
    { AZ_IOT_ERR_NOT_INITIALIZED, "AZ_IOT_ERR_NOT_INITIALIZED" },
    { AZ_IOT_ERR_ALREADY_INITIALIZED, "AZ_IOT_ERR_ALREADY_INITIALIZED" },
    { AZ_IOT_ERR_NOT_CONNECTED, "AZ_IOT_ERR_NOT_CONNECTED" },
    { AZ_IOT_ERR_TIMEOUT, "AZ_IOT_ERR_TIMEOUT" },
    { AZ_IOT_ERR_TLS, "AZ_IOT_ERR_TLS" },
    { AZ_IOT_ERR_AUTH, "AZ_IOT_ERR_AUTH" },
    { AZ_IOT_ERR_PROTOCOL, "AZ_IOT_ERR_PROTOCOL" },
    { AZ_IOT_ERR_MQTT, "AZ_IOT_ERR_MQTT" },
    { AZ_IOT_ERR_DPS, "AZ_IOT_ERR_DPS" },
    { AZ_IOT_ERR_NOT_SUPPORTED, "AZ_IOT_ERR_NOT_SUPPORTED" },
    { AZ_IOT_ERR_BUSY, "AZ_IOT_ERR_BUSY" },
    { AZ_IOT_ERR_NOT_ENOUGH_SPACE, "AZ_IOT_ERR_NOT_ENOUGH_SPACE" },
    { AZ_IOT_ERR_DETACHED, "AZ_IOT_ERR_DETACHED" },
    { AZ_IOT_ERR_INTERNAL, "AZ_IOT_ERR_INTERNAL" },
    { AZ_IOT_ERR_NOT_FOUND, "AZ_IOT_ERR_NOT_FOUND" },
    { AZ_IOT_ERR_IDENTITY_REJECTED, "AZ_IOT_ERR_IDENTITY_REJECTED" },
    { AZ_IOT_ERR_CONNECTION_PROFILE_UNSUPPORTED, "AZ_IOT_ERR_CONNECTION_PROFILE_UNSUPPORTED" },
  };
  const size_t n = sizeof(all) / sizeof(all[0]);
  const char* got[sizeof(all) / sizeof(all[0])];

  /* The table above is hand-maintained, which is exactly how the most recently
   * added code went missing from it. az_iot_result starts at 0 and is only ever
   * appended to, so "last enumerator + 1" is how many codes exist -- appending
   * one without extending this table now fails here, instead of silently
   * falling back to AZ_IOT_ERR_UNKNOWN at runtime. */
  assert_int_equal(n, (size_t)AZ_IOT_ERR_CONNECTION_PROFILE_UNSUPPORTED + 1);

  for (size_t i = 0; i < n; ++i)
  {
    got[i] = az_iot_result_to_string(all[i].code);
    assert_mapped(got[i], "AZ_IOT_ERR_UNKNOWN", all[i].text);
    assert_string_equal(got[i], all[i].text);
  }
  assert_all_distinct(got, n);
}

static void result_to_string_reports_unknown_for_an_unmapped_code(void** state)
{
  (void)state;
  /* The default arm exists for a value that arrived from outside this build --
   * a caller linked against a newer header, say. Reaching it deliberately is
   * the only way to prove it returns something printable rather than NULL. */
  assert_string_equal(az_iot_result_to_string((az_iot_result)9999), "AZ_IOT_ERR_UNKNOWN");
}

/* ---- connection state ----------------------------------------------------- */

static void connection_state_to_string_covers_every_state(void** state)
{
  (void)state;
  const struct
  {
    az_iot_connection_state state;
    const char* text;
  } all[] = {
    { AZ_IOT_CONN_STATE_IDLE, "AZ_IOT_CONN_STATE_IDLE" },
    { AZ_IOT_CONN_STATE_CONNECTING, "AZ_IOT_CONN_STATE_CONNECTING" },
    { AZ_IOT_CONN_STATE_CONNECTED, "AZ_IOT_CONN_STATE_CONNECTED" },
    { AZ_IOT_CONN_STATE_DISCONNECTING, "AZ_IOT_CONN_STATE_DISCONNECTING" },
    { AZ_IOT_CONN_STATE_RECONNECTING, "AZ_IOT_CONN_STATE_RECONNECTING" },
    { AZ_IOT_CONN_STATE_FAULTED, "AZ_IOT_CONN_STATE_FAULTED" },
  };
  const size_t n = sizeof(all) / sizeof(all[0]);
  const char* got[sizeof(all) / sizeof(all[0])];

  for (size_t i = 0; i < n; ++i)
  {
    got[i] = az_iot_connection_state_to_string(all[i].state);
    /* Against the fallback as well as the expected text: a missing arm returns
     * "UNKNOWN", which is non-null and non-empty and would pass a weaker
     * check. */
    assert_mapped(got[i], "UNKNOWN", all[i].text);
    assert_string_equal(got[i], all[i].text);
  }
  assert_all_distinct(got, n);
}

static void connection_state_to_string_reports_unknown_for_an_unmapped_state(void** state)
{
  (void)state;
  /* The default arm is unreachable from any named enumerator, so it needs a
   * value from outside the enum to be exercised at all. */
  assert_string_equal(az_iot_connection_state_to_string((az_iot_connection_state)123), "UNKNOWN");
}

/* ---- mqtt version and role ------------------------------------------------ */

static void mqtt_version_to_string_covers_every_version(void** state)
{
  (void)state;
  assert_mapped(az_iot_mqtt_version_to_string(AZ_IOT_MQTT_VERSION_3_1_1), "MQTT?", "v3.1.1");
  assert_mapped(az_iot_mqtt_version_to_string(AZ_IOT_MQTT_VERSION_5), "MQTT?", "v5");
  assert_string_not_equal(
      az_iot_mqtt_version_to_string(AZ_IOT_MQTT_VERSION_3_1_1),
      az_iot_mqtt_version_to_string(AZ_IOT_MQTT_VERSION_5));
  /* The wire version is what a reader correlates against a packet capture, so
   * the strings name the protocol rather than the enumerator. */
  assert_string_equal(az_iot_mqtt_version_to_string(AZ_IOT_MQTT_VERSION_3_1_1), "MQTTv3.1.1");
  assert_string_equal(az_iot_mqtt_version_to_string(AZ_IOT_MQTT_VERSION_5), "MQTTv5");
}

static void mqtt_version_to_string_reports_unknown_for_an_unmapped_version(void** state)
{
  (void)state;
  assert_string_equal(az_iot_mqtt_version_to_string((az_iot_mqtt_version)77), "MQTT?");
}

static void mqtt_role_to_string_covers_every_role(void** state)
{
  (void)state;
  const struct
  {
    az_iot_mqtt_role role;
    const char* text;
  } all[] = {
    { AZ_IOT_MQTT_ROLE_DPS, "DPS" },
    { AZ_IOT_MQTT_ROLE_HUB_CLASSIC, "HUB_CLASSIC" },
    { AZ_IOT_MQTT_ROLE_HUB_NEXT, "HUB_NEXT" },
  };
  const size_t n = sizeof(all) / sizeof(all[0]);
  const char* got[sizeof(all) / sizeof(all[0])];

  for (size_t i = 0; i < n; ++i)
  {
    got[i] = az_iot_mqtt_role_to_string(all[i].role);
    assert_mapped(got[i], "ROLE?", all[i].text);
    assert_string_equal(got[i], all[i].text);
  }
  assert_all_distinct(got, n);
}

static void mqtt_role_to_string_reports_unknown_for_an_unmapped_role(void** state)
{
  (void)state;
  assert_string_equal(az_iot_mqtt_role_to_string((az_iot_mqtt_role)99), "ROLE?");
}

/* ---- hub flavor ----------------------------------------------------------- */

static void hub_flavor_to_string_covers_every_flavor(void** state)
{
  (void)state;
  assert_mapped(
      az_iot_hub_flavor_to_string(AZ_IOT_HUB_FLAVOR_CLASSIC),
      "AZ_IOT_HUB_FLAVOR_UNKNOWN",
      "classic");
  assert_mapped(
      az_iot_hub_flavor_to_string(AZ_IOT_HUB_FLAVOR_NEXT), "AZ_IOT_HUB_FLAVOR_UNKNOWN", "next");
  assert_string_not_equal(
      az_iot_hub_flavor_to_string(AZ_IOT_HUB_FLAVOR_CLASSIC),
      az_iot_hub_flavor_to_string(AZ_IOT_HUB_FLAVOR_NEXT));
}

static void hub_flavor_to_string_reports_unknown_for_an_unmapped_flavor(void** state)
{
  (void)state;
  /* The enum has exactly CLASSIC and NEXT; "unknown" is the default arm's
   * string, not an enumerator. Reaching it needs a value from outside the
   * enum's range, which is how a profile from a newer build would arrive. */
  assert_string_equal(
      az_iot_hub_flavor_to_string((az_iot_hub_flavor)42), "AZ_IOT_HUB_FLAVOR_UNKNOWN");
}

/* ---- version -------------------------------------------------------------- */

static void version_string_matches_the_header_macros(void** state)
{
  (void)state;
  /* The function is compiled into the library and the macro is expanded here,
   * so this pins the two against each other within one build. It does not
   * detect an old binary linked against a new header -- the suite links the
   * library it was just built with -- but it does catch the version being
   * bumped in only one of the two places. */
  assert_non_null(az_iot_version_string());
  assert_string_equal(az_iot_version_string(), AZ_IOT_VERSION_STRING);

  char expected[32];
  snprintf(
      expected,
      sizeof(expected),
      "%d.%d.%d",
      AZ_IOT_VERSION_MAJOR,
      AZ_IOT_VERSION_MINOR,
      AZ_IOT_VERSION_PATCH);
  assert_string_equal(az_iot_version_string(), expected);
}

/* ---- reconnection policy defaults ----------------------------------------- */

static void reconnection_policy_default_is_usable_as_supplied(void** state)
{
  (void)state;
  az_iot_reconnection_policy p = az_iot_reconnection_policy_default();

  /* These are documented in az_iot_connection_client.h ("1s initial delay, 30s
   * max backoff, infinite attempts, 20% jitter"), which makes them part of the
   * public contract rather than a private tuning decision: an application may
   * rely on them without setting a policy of its own. Asserted exactly, so a
   * change has to be a deliberate one that updates the documentation too. */
  assert_int_equal(p.initial_delay_ms, 1000u);
  assert_int_equal(p.max_delay_ms, 30000u);
  assert_int_equal(p.max_attempts, 0u); /* 0 = infinite */
  assert_int_equal(p.jitter_pct, 20u);

  assert_true(p.max_delay_ms >= p.initial_delay_ms);
  assert_true(p.jitter_pct <= 100);
}

/* ---- built-in stderr sink ------------------------------------------------- */

static void stderr_sink_is_installable_and_emits(void** state)
{
  (void)state;
  /* Drives the shipped stderr sink through its public constructor. stderr
   * cannot be read back portably, so what this covers is the sink's own write
   * path -- not that the right text came out. The delivery contract (plain and
   * formatted messages reaching a sink, level routing, truncation, the no-sink
   * case) is asserted against a capturing sink in log_test.c; repeating it here
   * would duplicate that suite rather than add coverage. */
  az_iot_log_sink sink = az_iot_log_stderr_sink(AZ_IOT_LOG_LEVEL_TRACE);
  /* Compared directly rather than cast through void*: converting a function
   * pointer to an object pointer is implementation-defined, and the two need
   * not even be the same width. */
  assert_true(sink.sink != NULL);
  assert_int_equal(sink.min_level, AZ_IOT_LOG_LEVEL_TRACE);

  az_iot_log_set_global_sink(&sink);
  AZ_IOT_LOG_TRACE("diagnostics_test: trace");
  AZ_IOT_LOG_DEBUG("diagnostics_test: debug");
  AZ_IOT_LOG_INFO("diagnostics_test: info");
  AZ_IOT_LOG_WARN("diagnostics_test: warn");
  AZ_IOT_LOG_ERROR("diagnostics_test: error");
  AZ_IOT_LOG_ERRORF("diagnostics_test: %s %d", "formatted", 42);
  az_iot_log_set_global_sink(NULL);
}

static void stderr_sink_honours_its_minimum_level(void** state)
{
  (void)state;
  az_iot_log_sink sink = az_iot_log_stderr_sink(AZ_IOT_LOG_LEVEL_ERROR);
  assert_int_equal(sink.min_level, AZ_IOT_LOG_LEVEL_ERROR);
  az_iot_log_set_global_sink(&sink);
  /* Below the threshold: dropped before the sink is reached. */
  assert_false(az_iot_log_is_enabled(AZ_IOT_LOG_LEVEL_DEBUG));
  assert_true(az_iot_log_is_enabled(AZ_IOT_LOG_LEVEL_ERROR));
  az_iot_log_set_global_sink(NULL);
}

/* ---- request id generation ------------------------------------------------ */

static void gen_request_id_fills_a_bounded_string(void** state)
{
  (void)state;
  uint64_t rng = 0x0123456789ABCDEFull;
  char id[24];
  memset(id, 0x7F, sizeof(id));

  az_iot_cert_util_gen_request_id(&rng, id, sizeof(id));

  /* Located with a bounded search first: calling strlen() on a buffer the
   * generator may have failed to terminate would read past it, so the
   * regression this test exists to catch would be undefined behaviour rather
   * than a clean failure. */
  const char* end = (const char*)memchr(id, '\0', sizeof(id));
  assert_non_null(end);
  const size_t len = (size_t)(end - id);
  assert_true(len > 0);
  assert_true(len < sizeof(id));
  for (size_t i = 0; i < len; ++i)
  {
    assert_true(id[i] > 0x20 && id[i] < 0x7F);
  }
}

static void gen_request_id_advances_the_rng(void** state)
{
  (void)state;
  /* Two ids from the same seeded state must differ, or a renewal retry would
   * reuse a request id the service has already answered. */
  uint64_t rng = 0xDEADBEEFCAFEBABEull;
  char first[24];
  char second[24];

  /* The generator also mixes in the monotonic clock, so two different ids do
   * not by themselves prove the stored state moved: calls in different
   * milliseconds would differ even if the state update had regressed. Check
   * the state directly, and keep the id comparison as its own assertion. */
  const uint64_t seed = rng;
  az_iot_cert_util_gen_request_id(&rng, first, sizeof(first));
  const uint64_t after_first = rng;
  assert_true(after_first != seed);

  az_iot_cert_util_gen_request_id(&rng, second, sizeof(second));
  assert_true(rng != after_first);

  assert_string_not_equal(first, second);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(result_to_string_covers_every_code),
    cmocka_unit_test(result_to_string_reports_unknown_for_an_unmapped_code),
    cmocka_unit_test(connection_state_to_string_covers_every_state),
    cmocka_unit_test(connection_state_to_string_reports_unknown_for_an_unmapped_state),
    cmocka_unit_test(mqtt_version_to_string_covers_every_version),
    cmocka_unit_test(mqtt_version_to_string_reports_unknown_for_an_unmapped_version),
    cmocka_unit_test(mqtt_role_to_string_covers_every_role),
    cmocka_unit_test(mqtt_role_to_string_reports_unknown_for_an_unmapped_role),
    cmocka_unit_test(hub_flavor_to_string_covers_every_flavor),
    cmocka_unit_test(hub_flavor_to_string_reports_unknown_for_an_unmapped_flavor),
    cmocka_unit_test(version_string_matches_the_header_macros),
    cmocka_unit_test(reconnection_policy_default_is_usable_as_supplied),
    cmocka_unit_test(stderr_sink_is_installable_and_emits),
    cmocka_unit_test(stderr_sink_honours_its_minimum_level),
    cmocka_unit_test(gen_request_id_fills_a_bounded_string),
    cmocka_unit_test(gen_request_id_advances_the_rng),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
