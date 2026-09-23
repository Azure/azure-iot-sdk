// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* authentication/hsm_pkcs11_gen1 - sample.
 *
 * Connect to a Classic IoT Hub with a device private key that NEVER LEAVES the
 * hardware (design decision D8). The provider hands the SDK a key REFERENCE --
 * an RFC 7512 "pkcs11:" URI plus the id of the OpenSSL provider that owns it --
 * instead of a key, and the Paho adapter signs the TLS handshake through that
 * token.
 *
 * The device certificate is still an ordinary PEM file: a certificate is public
 * and there is nothing to protect. Only the key is custodial.
 *
 * What the integrator supplies is the token: any PKCS#11 module works
 * (SoftHSM2, a TPM through tpm2-pkcs11, an ATECC608, a smart card). The URI
 * names the object inside it; the SDK never learns anything else about it.
 *
 * Key custody is generation-agnostic -- the token signs a TLS handshake and
 * neither MQTT version is visible to it. hsm_pkcs11_gen2 is the identical
 * sample against an AEG hub; the only difference is which telemetry client and
 * which MQTT adapter are built, so the custody code stands out as the part that
 * does not change.
 *
 * Requirements on the host:
 *   - OpenSSL 3.0+ and an OpenSSL 3.x provider for the token, e.g.
 *     `pkcs11-provider` for PKCS#11 or `tpm2-openssl` for TPM 2.0. The provider
 *     must be findable by OpenSSL (installed in the modules directory, or
 *     OPENSSL_MODULES pointing at it) and configured with the PKCS#11 module it
 *     should drive.
 *   - The Paho adapter built with OpenSSL 3.0+ (AZ_IOT_PAHO_KEY_CUSTODY, on by
 *     default when OpenSSL 3.0+ is found).
 *
 * A key reference that cannot be resolved fails at connect time with
 * AZ_IOT_ERR_TLS and a log line naming the URI, rather than in the middle of
 * the handshake.
 *
 * Environment (each overrides the matching SAMPLE_* constant):
 *   AZ_IOT_DPS_ID_SCOPE        DPS id scope
 *   AZ_IOT_DPS_REGISTRATION_ID registration id / device id
 *   AZ_IOT_CLIENT_CERT         device certificate PEM path
 *   AZ_IOT_CLIENT_KEY_URI      key reference, e.g.
 *                              "pkcs11:token=aziot;object=device-key;type=private"
 *   AZ_IOT_CRYPTO_ENGINE_ID    OpenSSL provider id, e.g. "pkcs11" or "tpm2"
 *   AZ_IOT_TRUSTED_CA          trusted CA PEM path (optional; system store if unset)
 *
 * Diagnostics (optional):
 *   AZ_IOT_PAHO_TRACE          Paho MQTT trace + verbose OpenSSL TLS errors.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "azure/iot/adapters/az_iot_adapter_paho.h"
#include "azure/iot/az_iot.h"

#include "sample_utils.h"

/* ---- Fill these in, or set the matching env vars (env wins) --------------- */
#define SAMPLE_ID_SCOPE "" /* AZ_IOT_DPS_ID_SCOPE        */
#define SAMPLE_REGISTRATION_ID "" /* AZ_IOT_DPS_REGISTRATION_ID */
#define SAMPLE_CLIENT_CERT "" /* AZ_IOT_CLIENT_CERT         */
#define SAMPLE_CLIENT_KEY_URI "" /* AZ_IOT_CLIENT_KEY_URI      */
#define SAMPLE_CRYPTO_ENGINE_ID "pkcs11" /* AZ_IOT_CRYPTO_ENGINE_ID    */
#define SAMPLE_TRUSTED_CA "" /* AZ_IOT_TRUSTED_CA (optional) */

typedef struct
{
  char* id_scope;
  char* reg_id;
  char* cert;
  char* key_uri;
  char* engine_id;
  char* ca;
  char* dps_global_endpoint; /* optional; NULL selects the SDK default */
} hsm_config;

static int is_set(const char* s) { return s != NULL && s[0] != '\0'; }

static void hsm_config_release(hsm_config* c)
{
  free(c->id_scope);
  free(c->reg_id);
  free(c->cert);
  free(c->key_uri);
  free(c->engine_id);
  free(c->ca);
  free(c->dps_global_endpoint);
  memset(c, 0, sizeof(*c));
}

static int hsm_config_load(hsm_config* c)
{
  memset(c, 0, sizeof(*c));
  c->id_scope = sample_env_dup("AZ_IOT_DPS_ID_SCOPE", SAMPLE_ID_SCOPE);
  c->reg_id = sample_env_dup("AZ_IOT_DPS_REGISTRATION_ID", SAMPLE_REGISTRATION_ID);
  c->cert = sample_env_dup("AZ_IOT_CLIENT_CERT", SAMPLE_CLIENT_CERT);
  c->key_uri = sample_env_dup("AZ_IOT_CLIENT_KEY_URI", SAMPLE_CLIENT_KEY_URI);
  c->engine_id = sample_env_dup("AZ_IOT_CRYPTO_ENGINE_ID", SAMPLE_CRYPTO_ENGINE_ID);
  c->ca = sample_env_dup("AZ_IOT_TRUSTED_CA", SAMPLE_TRUSTED_CA);
  c->dps_global_endpoint = sample_env_dup("AZ_IOT_DPS_GLOBAL_ENDPOINT", NULL);

  return (is_set(c->id_scope) && is_set(c->reg_id) && is_set(c->cert) && is_set(c->key_uri)
          && is_set(c->engine_id))
      ? 0
      : 1;
}

static void print_usage(void)
{
  fprintf(
      stderr,
      "[hsm_pkcs11_gen1] not configured; skipping.\n"
      "  Set these env vars (or edit the SAMPLE_* constants) and re-run:\n"
      "    AZ_IOT_DPS_ID_SCOPE        DPS id scope\n"
      "    AZ_IOT_DPS_REGISTRATION_ID registration id / device id\n"
      "    AZ_IOT_CLIENT_CERT         device certificate PEM path\n"
      "    AZ_IOT_CLIENT_KEY_URI      pkcs11: URI of the device private key\n"
      "    AZ_IOT_CRYPTO_ENGINE_ID    OpenSSL provider id (\"pkcs11\", \"tpm2\")\n"
      "    AZ_IOT_TRUSTED_CA          trusted CA PEM path (optional)\n");
}

/* ------------------------------------------------------------------------- */
/* the provider: a certificate on disk, a key that stays in the token         */
/* ------------------------------------------------------------------------- */

/* Everything below is what an HSM integration actually looks like. load() fills
 * client_key_uri + crypto_engine_id and leaves client_key_pem/path NULL,
 * because there is no key to hand over -- only a name for one.
 *
 * get_csr()/sign() are not implemented here: this sample uses a certificate the
 * device was provisioned with. See custom_provider_template for the CSR side
 * and hsm_sign_callback for the sign() hook. */
typedef struct
{
  az_iot_certificate_provider base;
  const hsm_config* config;
} hsm_provider;

static az_iot_result hsm_load(
    az_iot_certificate_provider* self,
    az_iot_cert_role role,
    az_iot_certificate_material* out)
{
  hsm_provider* p = (hsm_provider*)self;
  (void)role; /* one static identity for both roles */
  memset(out, 0, sizeof(*out));
  out->client_cert_path = p->config->cert;
  out->trusted_ca_path = is_set(p->config->ca) ? p->config->ca : NULL;
  /* The key: a reference, not material. */
  out->client_key_uri = p->config->key_uri;
  out->crypto_engine_id = p->config->engine_id;
  return AZ_IOT_OK;
}

static void hsm_release(az_iot_certificate_provider* self, az_iot_certificate_material* material)
{
  (void)self;
  (void)material; /* nothing was allocated */
}

static void hsm_deinit(az_iot_certificate_provider* self) { (void)self; }

static const az_iot_certificate_provider_vtable k_hsm_vtable = {
  .version = AZ_IOT_CERTIFICATE_PROVIDER_VTABLE_VERSION,
  .load = hsm_load,
  .release = hsm_release,
  .deinit = hsm_deinit,
};

/* ------------------------------------------------------------------------- */

typedef struct
{
  az_iot_connection_state conn_state;
  az_iot_result conn_reason;
  int send_done;
  az_iot_result send_status;
} user_context;

static void on_conn_state(const az_iot_connection_state_event* event, void* user_ctx)
{
  /* Hub lifecycle only: the provisioning session reports on its own scope,
   * and storing its state here would overwrite the hub state this code acts
   * on. */
  if (event->scope != AZ_IOT_CONN_SCOPE_HUB)
  {
    return;
  }

  user_context* ctx = (user_context*)user_ctx;
  ctx->conn_state = event->state;
  ctx->conn_reason = event->reason;
}

static void on_send_done(az_iot_result status, void* user_ctx)
{
  user_context* ctx = (user_context*)user_ctx;
  ctx->send_status = status;
  ctx->send_done = 1;
}

int main(void)
{
  az_iot_log_sink log = az_iot_log_stderr_sink(AZ_IOT_LOG_LEVEL_INFO);
  az_iot_log_set_global_sink(&log);

  hsm_config config = { 0 };
  if (hsm_config_load(&config) != 0)
  {
    print_usage();
    hsm_config_release(&config);
    return 0; /* no-op when unconfigured */
  }

  fprintf(
      stderr,
      "[hsm_pkcs11_gen1] provisioning '%s'; key stays in the token (%s via '%s')\n",
      config.reg_id,
      config.key_uri,
      config.engine_id);

  int rc = 1;
  user_context user_ctx = { 0 };
  hsm_provider provider = { .base = { .vtable = &k_hsm_vtable }, .config = &config };
  az_iot_connection_client connection_client = { 0 };
  az_iot_gen1_telemetry_client telemetry = { 0 };

  az_iot_connection_client_options copts = az_iot_connection_client_options_default();
  copts.client_id = config.reg_id;
  /* This sample carries its own config struct (PKCS#11 URI and engine id), not
   * the shared sample_config, so it sets the DPS fields directly -- including
   * the optional endpoint, so it targets the same provisioning service as every
   * other sample rather than always the global one. */
  copts.dps.id_scope = config.id_scope;
  copts.dps.registration_id = config.reg_id;
  if (config.dps_global_endpoint != NULL)
  {
    copts.dps.global_endpoint = config.dps_global_endpoint;
  }
  copts.certificate_provider = &provider.base;

  if (az_iot_connection_client_init(&connection_client, &copts) != AZ_IOT_OK)
  {
    goto cleanup;
  }
  az_iot_connection_client_add_state_observer(&connection_client, on_conn_state, &user_ctx);

  /* One adapter covers both legs here: DPS speaks v3.1.1 and so does Classic. */
  if (az_iot_connection_client_register_mqtt_factory(
          &connection_client, az_iot_paho_factory_create_v3_1_1())
      != AZ_IOT_OK)
  {
    goto cleanup;
  }

  /* Declares the generation before provisioning runs, so a DPS assignment to an
   * AEG hub fails with AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH instead of
   * connecting into a client that cannot speak to it. */
  if (az_iot_gen1_telemetry_client_init(&telemetry, &connection_client) != AZ_IOT_OK)
  {
    goto cleanup;
  }

  az_iot_result open_rc = az_iot_connection_client_open(&connection_client);
  if (open_rc != AZ_IOT_OK)
  {
    /* AZ_IOT_ERR_CREDENTIAL_INCOMPLETE here means the material could never
     * complete a handshake -- typically a key URI with no crypto_engine_id. */
    fprintf(stderr, "[hsm_pkcs11_gen1] open failed: %s\n", az_iot_result_to_string(open_rc));
    goto cleanup;
  }

  for (int i = 0; i < 1200 && user_ctx.conn_state != AZ_IOT_CONN_STATE_CONNECTED; ++i)
  {
    (void)az_iot_connection_client_do_work(&connection_client, 50);
    if (user_ctx.conn_state == AZ_IOT_CONN_STATE_FAULTED)
    {
      break;
    }
  }

  if (user_ctx.conn_state == AZ_IOT_CONN_STATE_CONNECTED)
  {
    fprintf(stderr, "[hsm_pkcs11_gen1] connected; the handshake signed inside the token\n");

    static const uint8_t payload[] = "{\"custody\":\"hardware\"}";
    az_iot_telemetry_message msg = { 0 };
    msg.payload = payload;
    msg.payload_len = sizeof(payload) - 1;

    if (az_iot_gen1_telemetry_client_send(&telemetry, &msg, on_send_done, &user_ctx) == AZ_IOT_OK)
    {
      for (int i = 0; i < 600 && !user_ctx.send_done; ++i)
      {
        (void)az_iot_connection_client_do_work(&connection_client, 50);
      }
      if (user_ctx.send_done && user_ctx.send_status == AZ_IOT_OK)
      {
        fprintf(stderr, "[hsm_pkcs11_gen1] telemetry sent\n");
        rc = 0;
      }
    }
  }
  else
  {
    fprintf(
        stderr,
        "[hsm_pkcs11_gen1] failed to connect (state=%s, reason=%s)\n",
        az_iot_connection_state_to_string(user_ctx.conn_state),
        az_iot_result_to_string(user_ctx.conn_reason));
  }

  az_iot_connection_client_close(&connection_client);
  for (int i = 0; i < 100 && user_ctx.conn_state != AZ_IOT_CONN_STATE_IDLE; ++i)
  {
    (void)az_iot_connection_client_do_work(&connection_client, 50);
  }

cleanup:
  az_iot_gen1_telemetry_client_destroy(&telemetry);
  az_iot_connection_client_destroy(&connection_client);
  hsm_config_release(&config);
  return rc;
}
