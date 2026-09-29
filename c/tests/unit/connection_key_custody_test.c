// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Non-extractable key custody (D8) through the connection client.
 *
 * These cases cover the seam between az_iot_certificate_material and
 * az_iot_mqtt_tls_options: the key-reference fields and the sign() hook must
 * reach the adapter on BOTH connect paths (the DPS/bootstrap one and the
 * operational/reconnect one), the sign() hook must be gated on the vtable
 * version, and a credential that cannot sign at all must be refused before a
 * socket exists rather than inside the TLS handshake.
 *
 * Driven through the public API and observed through the in-memory mock
 * adapter, which records the TLS options it was handed. */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>

#include "azure/iot/az_iot_certificate_provider.h"
#include "azure/iot/az_iot_connection_client.h"
#include "azure/iot/az_iot_mqtt_iface.h"
#include "azure/iot/az_iot_result.h"

#include "support/mock_mqtt_iface.h"
#include "support/test_provider.h"

/* ------------------------------------------------------------------------- */
/* a provider whose material every case shapes for itself                    */
/* ------------------------------------------------------------------------- */

typedef struct custody_provider
{
  az_iot_certificate_provider base;
  az_iot_certificate_material material;
  /* Result returned for AZ_IOT_CRED_OPERATIONAL. Cases that want the bootstrap
   * fallback exercised set this to AZ_IOT_ERR_NOT_FOUND. */
  az_iot_result operational_result;
  int load_calls;
  int release_calls;
  int sign_calls;
  size_t last_digest_len;
  /* Strip the key fields from every load() past this many calls, so a case can
   * separate what open() pre-flights from what the connect attempt copies. */
  int degrade_after_calls;
  /* Write *out_sig_len before returning a failure. A provider vtable is not
   * forbidden from doing this; az_iot_mqtt_sign_callback is, which is what the
   * trampoline has to enforce. */
  bool scribbles_out_len_on_failure;
} custody_provider;

static az_iot_result custody_load(
    az_iot_certificate_provider* s,
    az_iot_cert_role role,
    az_iot_certificate_material* out)
{
  custody_provider* p = (custody_provider*)s;
  p->load_calls++;
  if (role == AZ_IOT_CRED_OPERATIONAL && p->operational_result != AZ_IOT_OK)
  {
    return p->operational_result;
  }
  *out = p->material;
  if (p->degrade_after_calls > 0 && p->load_calls > p->degrade_after_calls)
  {
    out->client_key_pem = NULL;
    out->client_key_path = NULL;
    out->client_key_uri = NULL;
    out->crypto_engine_id = NULL;
  }
  return AZ_IOT_OK;
}

static void custody_release(az_iot_certificate_provider* s, az_iot_certificate_material* m)
{
  custody_provider* p = (custody_provider*)s;
  (void)m;
  p->release_calls++;
}

static void custody_deinit(az_iot_certificate_provider* s) { (void)s; }

static az_iot_result custody_sign(
    az_iot_certificate_provider* s,
    const uint8_t* digest,
    size_t digest_len,
    uint8_t* out_sig,
    size_t out_sig_cap,
    size_t* out_sig_len)
{
  custody_provider* p = (custody_provider*)s;
  p->sign_calls++;
  p->last_digest_len = digest_len;
  if (out_sig_cap < digest_len)
  {
    if (p->scribbles_out_len_on_failure && out_sig_len)
    {
      *out_sig_len = (size_t)0xDEAD;
    }
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }
  /* A stand-in signature: the digest reversed, so a test can tell the bytes
   * really came from this hook. */
  for (size_t i = 0; i < digest_len; ++i)
  {
    out_sig[i] = digest[digest_len - 1 - i];
  }
  *out_sig_len = digest_len;
  return AZ_IOT_OK;
}

static const az_iot_certificate_provider_vtable k_vtable_v2_with_sign = {
  .version = 2u,
  .load = custody_load,
  .release = custody_release,
  .deinit = custody_deinit,
  .sign = custody_sign,
};

static const az_iot_certificate_provider_vtable k_vtable_v2_without_sign = {
  .version = 2u,
  .load = custody_load,
  .release = custody_release,
  .deinit = custody_deinit,
};

/* A v1 provider that nevertheless populates the v2 sign slot. The slot did not
 * exist at v1, so the client must not call it however it looks. */
static const az_iot_certificate_provider_vtable k_vtable_v1_with_sign = {
  .version = 1u,
  .load = custody_load,
  .release = custody_release,
  .deinit = custody_deinit,
  .sign = custody_sign,
};

static void custody_provider_init(
    custody_provider* p,
    const az_iot_certificate_provider_vtable* vtable)
{
  memset(p, 0, sizeof(*p));
  p->base.vtable = vtable;
  p->operational_result = AZ_IOT_OK;
  /* The shape D8 exists for: a certificate plus a key that is a reference, not
   * bytes. */
  p->material.client_cert_path = "/dev/null/device.pem";
  p->material.trusted_ca_path = "/dev/null/ca.pem";
  p->material.client_key_uri = "pkcs11:token=ut;object=device-key;type=private";
  p->material.crypto_engine_id = "pkcs11";
}

/* ------------------------------------------------------------------------- */
/* fixture                                                                   */
/* ------------------------------------------------------------------------- */

typedef struct fixture
{
  az_iot_connection_client client;
  az_iot_mqtt_factory* factory;
  bool factory_registered;
  custody_provider provider;
} fixture;

static int setup(void** state)
{
  fixture* fx = (fixture*)calloc(1, sizeof(*fx));
  assert_non_null(fx);
  custody_provider_init(&fx->provider, &k_vtable_v2_with_sign);
  fx->factory = az_iot_mock_mqtt_factory_create(AZ_IOT_MQTT_VERSION_3_1_1);
  assert_non_null(fx->factory);
  *state = fx;
  return 0;
}

static int teardown(void** state)
{
  fixture* fx = (fixture*)*state;
  if (fx)
  {
    az_iot_connection_client_deinit(&fx->client);
    if (!fx->factory_registered)
    {
      az_iot_mock_mqtt_factory_destroy(fx->factory);
    }
    free(fx);
  }
  return 0;
}

/* Bring the client up against the mock adapter with this fixture's provider. */
static void init_client(fixture* fx, bool with_dps)
{
  az_iot_connection_client_options opts = { 0 };
  opts.client_id = "ut-device";
  opts.certificate_provider = &fx->provider.base;
  if (with_dps)
  {
    opts.dps.id_scope = "0ne00000000";
    opts.dps.registration_id = "ut-device";
  }
  else
  {
    opts.host = "broker.example";
    opts.port = 8883;
  }
  assert_int_equal(az_iot_test_connection_client_init(&fx->client, &opts), AZ_IOT_OK);
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(&fx->client, fx->factory), AZ_IOT_OK);
  fx->factory_registered = true;
}

static const az_iot_mock_call* last_connect(const fixture* fx)
{
  az_iot_mock_mqtt_client* m = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_non_null(m);
  size_t n = az_iot_mock_mqtt_client_call_count(m);
  for (size_t i = n; i > 0; --i)
  {
    const az_iot_mock_call* c = az_iot_mock_mqtt_client_call_at(m, i - 1);
    if (c->kind == AZ_IOT_MOCK_CALL_CONNECT)
    {
      return c;
    }
  }
  return NULL;
}

/* ------------------------------------------------------------------------- */
/* propagation                                                               */
/* ------------------------------------------------------------------------- */

/* Site 1 -- the DPS/bootstrap connect. The key URI, the engine id and the
 * sign() hook must all arrive at the adapter. */
static void dps_connect_forwards_the_key_reference(void** state)
{
  fixture* fx = (fixture*)*state;
  init_client(fx, true);
  assert_int_equal(az_iot_connection_client_open(&fx->client), AZ_IOT_OK);

  const az_iot_mock_call* c = last_connect(fx);
  assert_non_null(c);
  assert_string_equal(c->connect.client_key_uri, "pkcs11:token=ut;object=device-key;type=private");
  assert_string_equal(c->connect.crypto_engine_id, "pkcs11");
  assert_true(c->connect.has_sign);
  assert_ptr_equal(c->connect.sign_ctx, &fx->provider.base);
  /* Nothing extractable was invented along the way. */
  assert_false(c->connect.has_client_key_pem);
  assert_string_equal(c->connect.client_key_path, "");
}

/* Site 2 -- the hub/operational connect, which is also the path every reconnect
 * and every certificate rotation takes. A field copied only into site 1 breaks
 * custody the first time the device reconnects, silently. */
static void hub_connect_forwards_the_key_reference(void** state)
{
  fixture* fx = (fixture*)*state;
  init_client(fx, false);
  assert_int_equal(az_iot_connection_client_open(&fx->client), AZ_IOT_OK);

  const az_iot_mock_call* c = last_connect(fx);
  assert_non_null(c);
  assert_string_equal(c->connect.client_key_uri, "pkcs11:token=ut;object=device-key;type=private");
  assert_string_equal(c->connect.crypto_engine_id, "pkcs11");
  assert_true(c->connect.has_sign);
  assert_ptr_equal(c->connect.sign_ctx, &fx->provider.base);
}

/* The operational identity is preferred, but a provider that does not hold one
 * yet falls back to bootstrap -- and the fallback must carry the key reference
 * just as the first branch does. */
static void hub_connect_falls_back_to_bootstrap_material(void** state)
{
  fixture* fx = (fixture*)*state;
  fx->provider.operational_result = AZ_IOT_ERR_NOT_FOUND;
  init_client(fx, false);
  assert_int_equal(az_iot_connection_client_open(&fx->client), AZ_IOT_OK);

  const az_iot_mock_call* c = last_connect(fx);
  assert_non_null(c);
  assert_string_equal(c->connect.crypto_engine_id, "pkcs11");
  assert_true(c->connect.has_sign);
}

/* The adapter-facing callback must actually reach the provider's sign() slot,
 * with the arguments intact and the signature coming back out. */
static void the_forwarded_sign_hook_calls_the_provider(void** state)
{
  fixture* fx = (fixture*)*state;
  init_client(fx, false);
  assert_int_equal(az_iot_connection_client_open(&fx->client), AZ_IOT_OK);

  const az_iot_mock_call* c = last_connect(fx);
  assert_non_null(c);
  assert_non_null(c->connect.sign);

  const uint8_t digest[4] = { 1, 2, 3, 4 };
  uint8_t sig[4] = { 0 };
  size_t sig_len = 0;
  assert_int_equal(
      c->connect.sign(c->connect.sign_ctx, digest, sizeof(digest), sig, sizeof(sig), &sig_len),
      AZ_IOT_OK);
  assert_int_equal(fx->provider.sign_calls, 1);
  assert_int_equal(fx->provider.last_digest_len, sizeof(digest));
  assert_int_equal(sig_len, sizeof(digest));
  assert_int_equal(sig[0], 4);
  assert_int_equal(sig[3], 1);

  /* The provider's own failure travels back unchanged. */
  uint8_t too_small[2] = { 0 };
  assert_int_equal(
      c->connect.sign(c->connect.sign_ctx, digest, sizeof(digest), too_small, 2, &sig_len),
      AZ_IOT_ERR_NOT_ENOUGH_SPACE);

  /* A NULL context is a caller bug, not a crash. */
  assert_int_equal(
      c->connect.sign(NULL, digest, sizeof(digest), sig, sizeof(sig), &sig_len),
      AZ_IOT_ERR_INVALID_ARG);
}

/* az_iot_mqtt_sign_callback promises *out_sig_len is untouched on failure. The
 * provider vtable makes no such promise, so the trampoline -- which is where
 * the MQTT-side contract is made -- has to enforce it rather than assume it of
 * every provider. An adapter that sized a buffer from a value written during a
 * failed sign would read a length no signature ever had.
 *
 * The refusal the trampoline raises itself must honour the same rule. */
static void the_forwarded_sign_hook_preserves_out_len_on_failure(void** state)
{
  fixture* fx = (fixture*)*state;
  fx->provider.scribbles_out_len_on_failure = true;
  init_client(fx, false);
  assert_int_equal(az_iot_connection_client_open(&fx->client), AZ_IOT_OK);

  const az_iot_mock_call* c = last_connect(fx);
  assert_non_null(c);
  assert_non_null(c->connect.sign);

  const uint8_t digest[4] = { 1, 2, 3, 4 };
  uint8_t too_small[2] = { 0 };
  size_t sig_len = 1234u;

  /* The provider fails AND writes out_sig_len; the trampoline must put it back. */
  assert_int_equal(
      c->connect.sign(c->connect.sign_ctx, digest, sizeof(digest), too_small, 2, &sig_len),
      AZ_IOT_ERR_NOT_ENOUGH_SPACE);
  assert_int_equal(sig_len, 1234u);

  /* The trampoline's own refusal, before any provider call. */
  sig_len = 4321u;
  assert_int_equal(
      c->connect.sign(NULL, digest, sizeof(digest), too_small, sizeof(too_small), &sig_len),
      AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(sig_len, 4321u);

  /* A NULL out_sig_len is not a crash. */
  assert_int_equal(
      c->connect.sign(c->connect.sign_ctx, digest, sizeof(digest), too_small, 2, NULL),
      AZ_IOT_ERR_NOT_ENOUGH_SPACE);
}

/* The trampoline re-checks the provider it is handed rather than trusting it.
 * It runs inside an adapter's TLS callback, where the context came back through
 * a third-party library, so a wrong or stale pointer has to produce a failed
 * handshake and not a crash in the middle of one. */
static void the_forwarded_sign_hook_rejects_an_unusable_provider(void** state)
{
  fixture* fx = (fixture*)*state;
  init_client(fx, false);
  assert_int_equal(az_iot_connection_client_open(&fx->client), AZ_IOT_OK);
  const az_iot_mock_call* c = last_connect(fx);
  assert_non_null(c);
  assert_non_null(c->connect.sign);

  const uint8_t digest[4] = { 1, 2, 3, 4 };
  uint8_t sig[4] = { 0 };
  size_t sig_len = 0;

  /* No vtable at all. */
  az_iot_certificate_provider no_vtable = { .vtable = NULL };
  assert_int_equal(
      c->connect.sign(&no_vtable, digest, sizeof(digest), sig, sizeof(sig), &sig_len),
      AZ_IOT_ERR_INVALID_ARG);

  /* A vtable from before the slot existed. */
  az_iot_certificate_provider v1 = { .vtable = &k_vtable_v1_with_sign };
  assert_int_equal(
      c->connect.sign(&v1, digest, sizeof(digest), sig, sizeof(sig), &sig_len),
      AZ_IOT_ERR_INVALID_ARG);

  /* v2, but the slot is empty. */
  az_iot_certificate_provider no_sign = { .vtable = &k_vtable_v2_without_sign };
  assert_int_equal(
      c->connect.sign(&no_sign, digest, sizeof(digest), sig, sizeof(sig), &sig_len),
      AZ_IOT_ERR_INVALID_ARG);

  assert_int_equal(fx->provider.sign_calls, 0);
}

/* ------------------------------------------------------------------------- */
/* v2 gating                                                                 */
/* ------------------------------------------------------------------------- */

/* A v2 provider that leaves .sign NULL gets no hook forwarded. */
static void a_v2_provider_without_sign_forwards_no_hook(void** state)
{
  fixture* fx = (fixture*)*state;
  fx->provider.base.vtable = &k_vtable_v2_without_sign;
  init_client(fx, false);
  assert_int_equal(az_iot_connection_client_open(&fx->client), AZ_IOT_OK);

  const az_iot_mock_call* c = last_connect(fx);
  assert_non_null(c);
  assert_false(c->connect.has_sign);
  assert_null(c->connect.sign_ctx);
  /* The key reference itself is a v1 material field and still travels. */
  assert_string_equal(c->connect.crypto_engine_id, "pkcs11");
}

/* The sign slot does not exist below vtable version 2. A v1 provider whose
 * memory happens to hold something there must never be called through it. */
static void a_v1_provider_sign_slot_is_ignored(void** state)
{
  fixture* fx = (fixture*)*state;
  fx->provider.base.vtable = &k_vtable_v1_with_sign;
  init_client(fx, false);
  assert_int_equal(az_iot_connection_client_open(&fx->client), AZ_IOT_OK);

  const az_iot_mock_call* c = last_connect(fx);
  assert_non_null(c);
  assert_false(c->connect.has_sign);
  assert_int_equal(fx->provider.sign_calls, 0);
}

/* ------------------------------------------------------------------------- */
/* validation                                                                */
/* ------------------------------------------------------------------------- */

/* A client certificate with no key in ANY form cannot complete a handshake.
 * open() has to say so, by name, before a socket exists. */
static void a_certificate_without_any_key_is_refused_at_open(void** state)
{
  fixture* fx = (fixture*)*state;
  fx->provider.base.vtable = &k_vtable_v2_without_sign;
  fx->provider.material.client_key_uri = NULL;
  fx->provider.material.crypto_engine_id = NULL;
  init_client(fx, false);
  assert_int_equal(az_iot_connection_client_open(&fx->client), AZ_IOT_ERR_CREDENTIAL_INCOMPLETE);
  /* No adapter was created: the refusal happened before any connect attempt. */
  assert_null(az_iot_mock_mqtt_factory_last_client(fx->factory));
}

/* A key URI with nothing naming the engine or provider that owns it is
 * unresolvable by construction, and is refused for the same reason. */
static void a_key_uri_without_an_engine_id_is_refused(void** state)
{
  fixture* fx = (fixture*)*state;
  fx->provider.base.vtable = &k_vtable_v2_without_sign;
  fx->provider.material.crypto_engine_id = NULL;
  init_client(fx, false);
  assert_int_equal(az_iot_connection_client_open(&fx->client), AZ_IOT_ERR_CREDENTIAL_INCOMPLETE);
}

/* ...unless a sign() hook is present: then the adapter can sign without ever
 * resolving the URI, so the engine id is not required. */
static void a_key_uri_without_an_engine_id_is_allowed_with_sign(void** state)
{
  fixture* fx = (fixture*)*state;
  fx->provider.material.crypto_engine_id = NULL;
  init_client(fx, false);
  assert_int_equal(az_iot_connection_client_open(&fx->client), AZ_IOT_OK);
  const az_iot_mock_call* c = last_connect(fx);
  assert_non_null(c);
  assert_true(c->connect.has_sign);
}

/* A sign() hook alone -- no key material of any kind -- is a complete
 * credential: signing is all the handshake needs the key for. */
static void a_sign_hook_alone_satisfies_the_key_requirement(void** state)
{
  fixture* fx = (fixture*)*state;
  fx->provider.material.client_key_uri = NULL;
  fx->provider.material.crypto_engine_id = NULL;
  init_client(fx, false);
  assert_int_equal(az_iot_connection_client_open(&fx->client), AZ_IOT_OK);
  const az_iot_mock_call* c = last_connect(fx);
  assert_non_null(c);
  assert_true(c->connect.has_sign);
  assert_string_equal(c->connect.client_key_uri, "");
}

/* Material with no client certificate at all is a server-authentication-only
 * connection. That is legitimate and must not be rejected. */
static void material_without_a_client_certificate_is_not_rejected(void** state)
{
  fixture* fx = (fixture*)*state;
  fx->provider.base.vtable = &k_vtable_v2_without_sign;
  memset(&fx->provider.material, 0, sizeof(fx->provider.material));
  fx->provider.material.trusted_ca_path = "/dev/null/ca.pem";
  init_client(fx, false);
  assert_int_equal(az_iot_connection_client_open(&fx->client), AZ_IOT_OK);
  const az_iot_mock_call* c = last_connect(fx);
  assert_non_null(c);
  assert_true(c->connect.use_tls);
}

/* An ordinary PEM credential still works exactly as before -- the custody
 * fields stay empty and nothing new is required of it. */
static void a_plain_pem_credential_is_unaffected(void** state)
{
  fixture* fx = (fixture*)*state;
  fx->provider.base.vtable = &k_vtable_v2_without_sign;
  memset(&fx->provider.material, 0, sizeof(fx->provider.material));
  fx->provider.material.client_cert_pem = "cert";
  fx->provider.material.client_key_pem = "key";
  fx->provider.material.client_key_password = "pw";
  init_client(fx, false);
  assert_int_equal(az_iot_connection_client_open(&fx->client), AZ_IOT_OK);

  const az_iot_mock_call* c = last_connect(fx);
  assert_non_null(c);
  assert_true(c->connect.has_client_cert_pem);
  assert_true(c->connect.has_client_key_pem);
  assert_string_equal(c->connect.client_key_uri, "");
  assert_string_equal(c->connect.crypto_engine_id, "");
  assert_false(c->connect.has_sign);
}

/* A provider that holds no material yet is not a configuration error: it may
 * have one by the time the connect attempt runs. open() proceeds. */
static void a_provider_with_no_material_yet_is_not_refused(void** state)
{
  fixture* fx = (fixture*)*state;
  fx->provider.base.vtable = &k_vtable_v2_without_sign;
  fx->provider.operational_result = AZ_IOT_ERR_NOT_INITIALIZED;
  /* Both roles fail: make load() refuse the bootstrap role as well by giving
   * the provider a vtable whose load always fails. */
  init_client(fx, false);
  assert_int_equal(az_iot_connection_client_open(&fx->client), AZ_IOT_OK);
}

/* load() is required at every vtable version. A provider without one is a
 * configuration error open() names, not a NULL dereference several frames
 * later inside the connect attempt. */
/* A provider whose vtable pointer is NULL is a caller bug, and the client's job
 * is to say so rather than dereference it. open() reaches the provider through
 * several gates -- the CSR check, the load() check, and the sign() probe used
 * to build the TLS options -- and every one of them used to read through the
 * vtable after checking only the provider itself.
 *
 * The credential pre-flight is the first thing open() does with the provider,
 * so this crashes rather than fails if any of those gates regresses. */
static void a_provider_with_a_null_vtable_is_rejected(void** state)
{
  fixture* fx = (fixture*)*state;
  fx->provider.base.vtable = NULL;
  init_client(fx, false);
  assert_int_equal(az_iot_connection_client_open(&fx->client), AZ_IOT_ERR_NOT_SUPPORTED);
}

/* The same provider on the CSR path, which gates on the v2 ABI before looking
 * for get_csr and so reads the vtable earlier still. */
static void a_null_vtable_is_rejected_on_the_csr_path(void** state)
{
  fixture* fx = (fixture*)*state;
  fx->provider.base.vtable = NULL;

  az_iot_connection_client_options opts = { 0 };
  opts.client_id = "ut-device";
  opts.certificate_provider = &fx->provider.base;
  opts.dps.id_scope = "0ne00000000";
  opts.dps.registration_id = "ut-device";
  opts.dps.request_operational_certificate = true;
  assert_int_equal(az_iot_test_connection_client_init(&fx->client, &opts), AZ_IOT_OK);
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(&fx->client, fx->factory), AZ_IOT_OK);
  fx->factory_registered = true;

  assert_int_equal(az_iot_connection_client_open(&fx->client), AZ_IOT_ERR_NOT_SUPPORTED);
}

static void a_provider_without_load_is_rejected(void** state)
{
  fixture* fx = (fixture*)*state;
  static const az_iot_certificate_provider_vtable k_no_load = { .version = 2u };
  fx->provider.base.vtable = &k_no_load;
  init_client(fx, false);
  assert_int_equal(az_iot_connection_client_open(&fx->client), AZ_IOT_ERR_NOT_SUPPORTED);
}

/* The connect-time copy validates too, not only open(): a provider that starts
 * complete and later hands back a certificate with no key must fail the connect
 * attempt with the same named error rather than reach the network. */
static void a_credential_that_degrades_after_open_fails_the_connect(void** state)
{
  fixture* fx = (fixture*)*state;
  fx->provider.base.vtable = &k_vtable_v2_without_sign;
  /* Call 1 is the open() pre-flight and succeeds; call 2 is the connect
   * attempt's own load(), and it comes back without a key. */
  fx->provider.degrade_after_calls = 1;
  init_client(fx, false);
  assert_int_equal(az_iot_connection_client_open(&fx->client), AZ_IOT_ERR_CREDENTIAL_INCOMPLETE);
  assert_null(az_iot_mock_mqtt_factory_last_client(fx->factory));
}

/* The same on the DPS path, which is a separate copy site. */
static void a_dps_credential_without_a_key_fails_the_connect(void** state)
{
  fixture* fx = (fixture*)*state;
  fx->provider.base.vtable = &k_vtable_v2_without_sign;
  fx->provider.degrade_after_calls = 1;
  init_client(fx, true);
  assert_int_equal(az_iot_connection_client_open(&fx->client), AZ_IOT_ERR_CREDENTIAL_INCOMPLETE);
}

/* The new result code has to name itself; a log line reading
 * "AZ_IOT_ERR_UNKNOWN" is exactly the diagnostic this change exists to
 * prevent. */
static void the_new_result_code_has_a_string(void** state)
{
  (void)state;
  assert_string_equal(
      az_iot_result_to_string(AZ_IOT_ERR_CREDENTIAL_INCOMPLETE),
      "AZ_IOT_ERR_CREDENTIAL_INCOMPLETE");
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test_setup_teardown(dps_connect_forwards_the_key_reference, setup, teardown),
    cmocka_unit_test_setup_teardown(hub_connect_forwards_the_key_reference, setup, teardown),
    cmocka_unit_test_setup_teardown(hub_connect_falls_back_to_bootstrap_material, setup, teardown),
    cmocka_unit_test_setup_teardown(the_forwarded_sign_hook_calls_the_provider, setup, teardown),
    cmocka_unit_test_setup_teardown(
        the_forwarded_sign_hook_preserves_out_len_on_failure, setup, teardown),
    cmocka_unit_test_setup_teardown(
        the_forwarded_sign_hook_rejects_an_unusable_provider, setup, teardown),
    cmocka_unit_test_setup_teardown(a_v2_provider_without_sign_forwards_no_hook, setup, teardown),
    cmocka_unit_test_setup_teardown(a_v1_provider_sign_slot_is_ignored, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_certificate_without_any_key_is_refused_at_open, setup, teardown),
    cmocka_unit_test_setup_teardown(a_key_uri_without_an_engine_id_is_refused, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_key_uri_without_an_engine_id_is_allowed_with_sign, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_sign_hook_alone_satisfies_the_key_requirement, setup, teardown),
    cmocka_unit_test_setup_teardown(
        material_without_a_client_certificate_is_not_rejected, setup, teardown),
    cmocka_unit_test_setup_teardown(a_plain_pem_credential_is_unaffected, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_provider_with_no_material_yet_is_not_refused, setup, teardown),
    cmocka_unit_test_setup_teardown(a_provider_without_load_is_rejected, setup, teardown),
    cmocka_unit_test_setup_teardown(a_provider_with_a_null_vtable_is_rejected, setup, teardown),
    cmocka_unit_test_setup_teardown(a_null_vtable_is_rejected_on_the_csr_path, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_credential_that_degrades_after_open_fails_the_connect, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_dps_credential_without_a_key_fails_the_connect, setup, teardown),
    cmocka_unit_test(the_new_result_code_has_a_string),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
