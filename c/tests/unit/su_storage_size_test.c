// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */

/* Device-property storage limits through the public API. Built twice (tests/CMakeLists.txt):
 * with the default AZ_IOT_SU_COMPATIBILITY_STORAGE_SIZE and AZ_IOT_SU_INSTALLED_ID_STORAGE_SIZE,
 * and with both overridden for the SDK sources and the test alike. Expectations derive from the
 * macros, so both builds check the exact-fit and one-over boundaries. */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>

#include "azure/iot/az_iot_connection_client.h"
#include "azure/iot/az_iot_log.h"
#include "azure/iot/az_iot_su.h"

#include "support/test_provider.h"

#define COMPAT_SIZE ((size_t)AZ_IOT_SU_COMPATIBILITY_STORAGE_SIZE)
#define INSTALLED_SIZE ((size_t)AZ_IOT_SU_INSTALLED_ID_STORAGE_SIZE)

/* Three custom properties with 32-byte names and 64-byte values: 3 * (32 + 1 + 64 + 1). */
#define EXAMPLE_COMPAT_NEED 294u

typedef struct
{
  az_iot_connection_client conn;
  az_iot_su_client su;
  AZ_IOT_SU_DEVICE_PROPERTIES_STORAGE(dp_buf);
  char names[3][33];
  char values[3][65];
  az_iot_su_custom_property custom[3];
  char long_value[AZ_IOT_SU_COMPATIBILITY_STORAGE_SIZE + 1];
  char long_provider[AZ_IOT_SU_INSTALLED_ID_STORAGE_SIZE + 1];
} fixture;

static char g_last_error[AZ_IOT_LOG_MESSAGE_MAX];

static void capture_error(
    void* user_ctx,
    az_iot_log_level level,
    const char* component,
    const char* file,
    int line,
    const char* msg)
{
  (void)user_ctx;
  (void)component;
  (void)file;
  (void)line;
  if (level == AZ_IOT_LOG_LEVEL_ERROR)
  {
    snprintf(g_last_error, sizeof(g_last_error), "%s", msg);
  }
}

static az_iot_result stub_sha256_init(const az_iot_crypto* self, az_iot_sha256_ctx* ctx)
{
  (void)self;
  (void)ctx;
  return AZ_IOT_OK;
}

static az_iot_result stub_sha256_update(
    const az_iot_crypto* self,
    az_iot_sha256_ctx* ctx,
    const uint8_t* data,
    size_t len)
{
  (void)self;
  (void)ctx;
  (void)data;
  (void)len;
  return AZ_IOT_OK;
}

static az_iot_result stub_sha256_final(
    const az_iot_crypto* self,
    az_iot_sha256_ctx* ctx,
    uint8_t out[AZ_IOT_SHA256_SIZE])
{
  (void)self;
  (void)ctx;
  memset(out, 0, AZ_IOT_SHA256_SIZE);
  return AZ_IOT_OK;
}

static az_iot_result stub_verify_rs256(
    const az_iot_crypto* self,
    const uint8_t* modulus,
    size_t modulus_len,
    const uint8_t* exponent,
    size_t exponent_len,
    const uint8_t* data,
    size_t data_len,
    const uint8_t* signature,
    size_t signature_len)
{
  (void)self;
  (void)modulus;
  (void)modulus_len;
  (void)exponent;
  (void)exponent_len;
  (void)data;
  (void)data_len;
  (void)signature;
  (void)signature_len;
  return AZ_IOT_ERR_AUTH;
}

static const az_iot_crypto k_stub_crypto = {
  .version = AZ_IOT_CRYPTO_VERSION,
  .sha256_init = stub_sha256_init,
  .sha256_update = stub_sha256_update,
  .sha256_final = stub_sha256_final,
  .verify_rs256 = stub_verify_rs256,
};

static const az_iot_su_platform_hooks k_hooks = { 0 };

static int setup(void** state)
{
  fixture* fx = (fixture*)calloc(1, sizeof(*fx));
  assert_non_null(fx);
  az_iot_connection_client_options opts = { 0 };
  opts.device_id = "ut-device";
  opts.dps.id_scope = "0ne00000000";
  opts.dps.registration_id = "ut-device";
  opts.crypto = &k_stub_crypto;
  assert_int_equal(az_iot_test_connection_client_init(&fx->conn, &opts), AZ_IOT_OK);

  for (size_t i = 0; i < 3; ++i)
  {
    memset(fx->names[i], 'a' + (char)i, 32);
    memset(fx->values[i], 'x', 64);
    fx->custom[i] = (az_iot_su_custom_property){ fx->names[i], fx->values[i] };
  }

  static az_iot_log_sink sink = { capture_error, NULL, AZ_IOT_LOG_LEVEL_ERROR };
  az_iot_log_set_global_sink(&sink);
  g_last_error[0] = '\0';
  *state = fx;
  return 0;
}

static int teardown(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_log_set_global_sink(NULL);
  az_iot_connection_client_deinit(&fx->conn);
  free(fx);
  return 0;
}

static az_iot_result init_client(fixture* fx, const az_iot_su_device_properties* dp)
{
  az_iot_su_client_config_options opts = az_iot_su_client_config_options_default();
  opts.hooks = &k_hooks;
  opts.device_properties = dp;
  opts.device_properties_buffer = fx->dp_buf;
  opts.device_properties_buffer_size = sizeof(fx->dp_buf);
  return az_iot_su_client_init(&fx->su, &fx->conn, &opts);
}

/* manufacturer of @p len bytes: it takes len + 1 bytes of compatibility storage. */
static const char* manufacturer_of(fixture* fx, size_t len)
{
  memset(fx->long_value, 'm', len);
  fx->long_value[len] = '\0';
  return fx->long_value;
}

/* Provider of @p len bytes; with name "n" and version "v" the ID takes len + 5 bytes. */
static const char* provider_of(fixture* fx, size_t len)
{
  memset(fx->long_provider, 'p', len);
  fx->long_provider[len] = '\0';
  return fx->long_provider;
}

static void defaults_cover_both_storages(void** state)
{
  (void)state;
  assert_int_equal(AZ_IOT_SU_DEVICE_PROPERTIES_BUFFER_SIZE, COMPAT_SIZE + INSTALLED_SIZE);
#ifndef AZ_IOT_TEST_SU_STORAGE_OVERRIDDEN
  assert_int_equal(COMPAT_SIZE, 256);
  assert_int_equal(INSTALLED_SIZE, 192);
#endif
}

/* Three 32-byte names with 64-byte values need 294 bytes: rejected by the default build, both
 * at init and on replacement, and accepted by a build sized for them. */
static void three_long_custom_properties_need_294_bytes(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_su_device_properties dp
      = { .custom_properties = fx->custom, .custom_properties_count = 3 };
  bool fits = EXAMPLE_COMPAT_NEED <= COMPAT_SIZE;

  assert_int_equal(az_iot_su_device_properties_buffer_size(&dp), fits ? EXAMPLE_COMPAT_NEED : 0);
  az_iot_result r = init_client(fx, &dp);
  if (fits)
  {
    assert_int_equal(r, AZ_IOT_OK);
    az_iot_su_client_deinit(&fx->su);
  }
  else
  {
    assert_int_equal(r, AZ_IOT_ERR_NOT_ENOUGH_SPACE);
    assert_non_null(strstr(g_last_error, "need 294 bytes"));
    assert_non_null(strstr(g_last_error, "AZ_IOT_SU_COMPATIBILITY_STORAGE_SIZE"));
  }

  az_iot_su_device_properties small = { .manufacturer = "m" };
  assert_int_equal(init_client(fx, &small), AZ_IOT_OK);
  g_last_error[0] = '\0';
  assert_int_equal(
      az_iot_su_client_update_device_properties(&fx->su, &dp),
      fits ? AZ_IOT_OK : AZ_IOT_ERR_NOT_ENOUGH_SPACE);
  if (!fits)
  {
    assert_non_null(strstr(g_last_error, "AZ_IOT_SU_COMPATIBILITY_STORAGE_SIZE"));
    assert_string_equal(fx->su._internal.device_properties.manufacturer, "m");
  }
  az_iot_su_client_deinit(&fx->su);
}

static void compatibility_storage_exact_fit_and_one_over(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_su_device_properties first = { .manufacturer = "m" };
  assert_int_equal(init_client(fx, &first), AZ_IOT_OK);

  az_iot_su_device_properties dp = { .manufacturer = manufacturer_of(fx, COMPAT_SIZE - 1) };
  assert_int_equal(az_iot_su_device_properties_buffer_size(&dp), COMPAT_SIZE);
  assert_int_equal(az_iot_su_client_update_device_properties(&fx->su, &dp), AZ_IOT_OK);
  assert_int_equal(strlen(fx->su._internal.device_properties.manufacturer), COMPAT_SIZE - 1);

  /* One byte over: rejected, logged with the macro, and the accepted properties are kept. */
  char* big = (char*)calloc(1, COMPAT_SIZE);
  assert_non_null(big);
  memset(big, 'z', COMPAT_SIZE - 2);
  /* COMPAT_SIZE - 1 bytes for manufacturer and 2 for model. */
  az_iot_su_device_properties over = { .manufacturer = big, .model = "x" };
  g_last_error[0] = '\0';
  assert_int_equal(az_iot_su_device_properties_buffer_size(&over), 0);
  assert_int_equal(
      az_iot_su_client_update_device_properties(&fx->su, &over), AZ_IOT_ERR_NOT_ENOUGH_SPACE);
  char expected[96];
  snprintf(expected, sizeof(expected), "need %lu bytes", (unsigned long)(COMPAT_SIZE + 1));
  assert_non_null(strstr(g_last_error, expected));
  assert_non_null(strstr(g_last_error, "AZ_IOT_SU_COMPATIBILITY_STORAGE_SIZE"));
  assert_int_equal(strlen(fx->su._internal.device_properties.manufacturer), COMPAT_SIZE - 1);
  assert_null(fx->su._internal.device_properties.model);
  free(big);
  az_iot_su_client_deinit(&fx->su);
}

static void installed_id_storage_exact_fit_and_one_over(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_su_device_properties dp = { .manufacturer = "m" };
  dp.installed_update_id
      = (az_iot_su_update_id_info){ provider_of(fx, INSTALLED_SIZE - 5), "n", "v" };
  assert_int_equal(az_iot_su_device_properties_buffer_size(&dp), 2 + INSTALLED_SIZE);
  assert_int_equal(init_client(fx, &dp), AZ_IOT_OK);
  az_iot_su_client_deinit(&fx->su);

  dp.installed_update_id.provider = provider_of(fx, INSTALLED_SIZE - 4);
  g_last_error[0] = '\0';
  assert_int_equal(init_client(fx, &dp), AZ_IOT_ERR_NOT_ENOUGH_SPACE);
  assert_non_null(strstr(g_last_error, "AZ_IOT_SU_INSTALLED_ID_STORAGE_SIZE"));
}

/* Both storages full at once fill the default cache exactly. */
static void the_default_cache_holds_both_storages_full(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_su_device_properties dp = { .manufacturer = manufacturer_of(fx, COMPAT_SIZE - 1) };
  dp.installed_update_id
      = (az_iot_su_update_id_info){ provider_of(fx, INSTALLED_SIZE - 5), "n", "v" };
  assert_int_equal(
      az_iot_su_device_properties_buffer_size(&dp), AZ_IOT_SU_DEVICE_PROPERTIES_BUFFER_SIZE);
  assert_int_equal(init_client(fx, &dp), AZ_IOT_OK);
  az_iot_su_client_deinit(&fx->su);
}

static void a_short_cache_is_logged(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_su_device_properties dp = { .manufacturer = "Contoso" };
  az_iot_su_client_config_options opts = az_iot_su_client_config_options_default();
  opts.hooks = &k_hooks;
  opts.device_properties = &dp;
  opts.device_properties_buffer = fx->dp_buf;
  opts.device_properties_buffer_size = az_iot_su_device_properties_buffer_size(&dp) - 1;
  assert_int_equal(az_iot_su_client_init(&fx->su, &fx->conn, &opts), AZ_IOT_ERR_NOT_ENOUGH_SPACE);
  assert_non_null(strstr(g_last_error, "device_properties_buffer holds 7 bytes"));
  assert_non_null(strstr(g_last_error, "need 8"));
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(defaults_cover_both_storages),
    cmocka_unit_test_setup_teardown(three_long_custom_properties_need_294_bytes, setup, teardown),
    cmocka_unit_test_setup_teardown(compatibility_storage_exact_fit_and_one_over, setup, teardown),
    cmocka_unit_test_setup_teardown(installed_id_storage_exact_fit_and_one_over, setup, teardown),
    cmocka_unit_test_setup_teardown(the_default_cache_holds_both_storages_full, setup, teardown),
    cmocka_unit_test_setup_teardown(a_short_cache_is_logged, setup, teardown),
  };
  return cmocka_run_group_tests_name("su_storage_size", tests, NULL, NULL);
}
