// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* az_mqtt adapter, compiled in as for the mbedTLS backend: a client certificate with only a PSA
 * key id is accepted and the id reaches az_mqtt's TLS options. No connect: the handshake needs
 * the key in a PSA key store. */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define AZ_IOT_AZ_MQTT_MBEDTLS
#include "az_iot_mqtt_az_mqtt_v5.c"

#include <cmocka.h>

static void a_psa_only_credential_reaches_the_tls_options(void** state)
{
  (void)state;
  az_iot_mqtt_limits const limits = {
    .send_buffer_size = 1024,
    .receive_buffer_size = 1024,
    .max_in_flight_messages = 1,
    .message_store_size = 0,
    .connect_strings_size = 256,
  };
  size_t const size = az_iot_az_mqtt_get_workspace_size(&limits);
  uint8_t* workspace = (uint8_t*)malloc(size);
  assert_non_null(workspace);
  az_iot_mqtt_client* c = NULL;
  assert_int_equal(
      az_iot_az_mqtt_get_adapter()->init(AZ_IOT_MQTT_VERSION_5, &limits, workspace, size, &c),
      AZ_IOT_OK);
  az_iot_mqtt_connect_options o = { 0 };
  o.host = "broker.example";
  o.client_id = "c";
  o.tls.trusted_ca_pem = "CA";
  o.tls.client_cert_pem = "CERT";
  o.tls.client_key_psa_id = 0x7FFF0001u;
  assert_int_equal(_azm_check_options(&o), AZ_IOT_OK);

  _azm_client* m = _azm_self(c);
  _AZM(client_options) options;
  memset(&options, 0, sizeof(options));
  assert_true(_azm_copy_options(m, &o, &options));
  assert_ptr_equal(options.tls_options, &m->tls);
  assert_int_equal(m->tls.client_key_psa_id, 0x7FFF0001u);
  assert_int_equal(az_span_size(m->tls.client_key_pem), 0);
  assert_int_equal(az_span_size(m->tls.client_key_path), 0);
  assert_int_equal(az_span_size(m->tls.client_key_uri), 0);
  assert_true(az_result_succeeded(az_mqtt_tls_options_check(&m->tls)));
  _azm_release_owned(m);

  c->iface->destroy(c);
  free(workspace);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(a_psa_only_credential_reaches_the_tls_options),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
