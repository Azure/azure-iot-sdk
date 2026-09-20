// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* authentication/hsm_sign_callback - sample.
 *
 * The other half of design decision D8: a secure element whose key cannot be
 * named by a URI and whose stack has no OpenSSL ENGINE or provider to name it
 * with. The only operation the hardware exposes is "sign these bytes", so the
 * provider implements the vtable's sign() hook and the TLS layer drives the
 * handshake signature through it.
 *
 * This is the path for BearSSL, mbedTLS, a vendor TLS stack, or any bare-metal
 * port -- anywhere a key handle cannot be handed to the TLS library. It is NOT
 * a Paho path: Paho takes its client key as a file path and exposes neither the
 * SSL_CTX nor a key callback (upstream has none either), so the Paho adapter
 * refuses a sign()-only credential with AZ_IOT_ERR_NOT_SUPPORTED instead of
 * connecting without a client key. Use hsm_pkcs11 with Paho.
 *
 * What the sample shows, and what it does not:
 *
 *   - It DOES show the whole seam: a provider that implements sign(), the SDK
 *     gating that hook on vtable version >= 2, and an adapter reading
 *     tls.sign / tls.sign_ctx out of az_iot_mqtt_connect_options and invoking
 *     it -- which is the exact line an integrator writes in their own adapter.
 *   - It does NOT speak MQTT. The adapter below is a stand-in that performs the
 *     signature and then reports that it has no transport, so the sample runs
 *     anywhere with no broker, no hardware and no network.
 *
 * Replace two things to make it real: sign() with a call into your secure
 * element, and the stand-in adapter with your own az_iot_mqtt_iface
 * implementation (see docs/how_to_byo_mqtt_client.md).
 *
 * Takes no configuration and always runs.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "azure/iot/az_iot.h"

/* ------------------------------------------------------------------------- */
/* the provider: no key, no URI -- only the ability to sign                   */
/* ------------------------------------------------------------------------- */

typedef struct
{
  az_iot_certificate_provider base;
  int sign_calls;
} sign_provider;

/* A device certificate is public, so it is an ordinary PEM. Stand-in text here
 * because the sample never opens a connection; a real device would load its
 * own, usually from the same secure element. */
static const char k_device_cert_pem[] = "-----BEGIN CERTIFICATE-----\n"
                                        "(the device certificate, which is public)\n"
                                        "-----END CERTIFICATE-----\n";

static az_iot_result sign_load(
    az_iot_certificate_provider* self,
    az_iot_cert_role role,
    az_iot_certificate_material* out)
{
  (void)self;
  (void)role;
  memset(out, 0, sizeof(*out));
  out->client_cert_pem = k_device_cert_pem;
  /* Deliberately nothing else. No client_key_pem, no client_key_path, no
   * client_key_uri, no crypto_engine_id: the key exists only inside the
   * element, and sign() below is the only way to use it. The SDK accepts this
   * shape precisely because the sign() hook is present -- drop the hook and
   * open() refuses the credential with AZ_IOT_ERR_CREDENTIAL_INCOMPLETE. */
  return AZ_IOT_OK;
}

static void sign_release(az_iot_certificate_provider* self, az_iot_certificate_material* material)
{
  (void)self;
  (void)material;
}

static void sign_deinit(az_iot_certificate_provider* self) { (void)self; }

/* THE HOOK. Replace the body with a call into your secure element:
 * ATECC608 ATCA_Sign, a TPM2_Sign, a vendor SDK, a remote KMS.
 *
 * Contract: sign `digest_len` bytes at `digest` with the device private key,
 * write at most `out_sig_cap` bytes to `out_sig`, and report the length in
 * *out_sig_len. Return AZ_IOT_ERR_NOT_ENOUGH_SPACE when the buffer is too
 * small, and leave *out_sig_len untouched on any failure. */
static az_iot_result sign_sign(
    az_iot_certificate_provider* self,
    const uint8_t* digest,
    size_t digest_len,
    uint8_t* out_sig,
    size_t out_sig_cap,
    size_t* out_sig_len)
{
  sign_provider* p = (sign_provider*)self;
  p->sign_calls++;

  if (out_sig_cap < digest_len)
  {
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }

  /* REPLACE ME: this stands in for the hardware signature so the sample runs
   * with no device attached. It is not a signature and must never ship. */
  for (size_t i = 0; i < digest_len; ++i)
  {
    out_sig[i] = (uint8_t)(digest[i] ^ 0xA5u);
  }
  *out_sig_len = digest_len;
  return AZ_IOT_OK;
}

static const az_iot_certificate_provider_vtable k_sign_vtable = {
  /* Version 2 or later, or the SDK will not look at .sign at all. */
  .version = AZ_IOT_CERTIFICATE_PROVIDER_VTABLE_VERSION,
  .load = sign_load,
  .release = sign_release,
  .deinit = sign_deinit,
  .sign = sign_sign,
};

/* ------------------------------------------------------------------------- */
/* a stand-in adapter, to show where tls.sign is consumed                     */
/* ------------------------------------------------------------------------- */

/* Everything here except connect() is the minimum an az_iot_mqtt_iface needs to
 * be well-formed. connect() is the interesting one: it is where a real adapter
 * would install the signature callback into its TLS stack. */
typedef struct
{
  az_iot_mqtt_client base; /* MUST be first */
  az_iot_mqtt_iface iface_storage;
} standin_client;

static az_iot_result standin_connect(
    az_iot_mqtt_client* self,
    const az_iot_mqtt_connect_options* opts)
{
  (void)self;

  if (opts->tls.sign == NULL)
  {
    fprintf(stderr, "[hsm_sign_callback] no sign() hook reached the adapter\n");
    return AZ_IOT_ERR_CREDENTIAL_INCOMPLETE;
  }

  fprintf(
      stderr,
      "[hsm_sign_callback] adapter received a sign() hook (ctx=%p); "
      "a real adapter installs it in its TLS stack here\n",
      opts->tls.sign_ctx);

  /* Drive it once over a stand-in digest, which is what the TLS stack does per
   * handshake for the CertificateVerify message. */
  static const uint8_t digest[32] = { 0 };
  uint8_t signature[72];
  size_t signature_len = 0;
  az_iot_result r = opts->tls.sign(
      opts->tls.sign_ctx, digest, sizeof(digest), signature, sizeof(signature), &signature_len);
  if (r != AZ_IOT_OK)
  {
    fprintf(stderr, "[hsm_sign_callback] sign() failed: %s\n", az_iot_result_to_string(r));
    return r;
  }
  fprintf(
      stderr,
      "[hsm_sign_callback] sign() produced %zu bytes without the key ever leaving the element\n",
      signature_len);

  /* No transport in this sample. A real adapter would connect here. */
  return AZ_IOT_ERR_NOT_SUPPORTED;
}

static az_iot_result standin_disconnect(az_iot_mqtt_client* self)
{
  (void)self;
  return AZ_IOT_ERR_NOT_CONNECTED;
}

static az_iot_result standin_subscribe(
    az_iot_mqtt_client* self,
    const char* topic_filter,
    az_iot_mqtt_qos qos,
    uint16_t* out_packet_id)
{
  (void)self;
  (void)topic_filter;
  (void)qos;
  (void)out_packet_id;
  return AZ_IOT_ERR_NOT_CONNECTED;
}

static az_iot_result standin_unsubscribe(
    az_iot_mqtt_client* self,
    const char* topic_filter,
    uint16_t* out_packet_id)
{
  (void)self;
  (void)topic_filter;
  (void)out_packet_id;
  return AZ_IOT_ERR_NOT_CONNECTED;
}

static az_iot_result standin_publish(
    az_iot_mqtt_client* self,
    const az_iot_mqtt_message* msg,
    uint16_t* out_packet_id)
{
  (void)self;
  (void)msg;
  (void)out_packet_id;
  return AZ_IOT_ERR_NOT_CONNECTED;
}

static az_iot_result standin_process_loop(az_iot_mqtt_client* self, uint32_t timeout_ms)
{
  (void)self;
  (void)timeout_ms;
  return AZ_IOT_OK;
}

static void standin_set_inbound_cb(
    az_iot_mqtt_client* self,
    az_iot_mqtt_event_callback cb,
    void* user_ctx)
{
  (void)self;
  (void)cb;
  (void)user_ctx;
}

static void standin_destroy(az_iot_mqtt_client* self) { free(self); }

static az_iot_mqtt_client* standin_create(void* factory_ctx)
{
  (void)factory_ctx;
  standin_client* c = (standin_client*)calloc(1, sizeof(*c));
  if (!c)
  {
    return NULL;
  }
  c->iface_storage.version = AZ_IOT_MQTT_VERSION_3_1_1;
  c->iface_storage.connect = standin_connect;
  c->iface_storage.disconnect = standin_disconnect;
  c->iface_storage.subscribe = standin_subscribe;
  c->iface_storage.unsubscribe = standin_unsubscribe;
  c->iface_storage.publish = standin_publish;
  c->iface_storage.process_loop = standin_process_loop;
  c->iface_storage.set_inbound_cb = standin_set_inbound_cb;
  c->iface_storage.destroy = standin_destroy;
  c->base.iface = &c->iface_storage;
  return &c->base;
}

/* ------------------------------------------------------------------------- */

static void on_conn_state(const az_iot_connection_state_event* event, void* user_ctx)
{
  az_iot_connection_state s = event->state;
  az_iot_result reason = event->reason;
  (void)user_ctx;
  fprintf(
      stderr,
      "[hsm_sign_callback] state=%s reason=%s\n",
      az_iot_connection_state_to_string(s),
      az_iot_result_to_string(reason));
}

int main(void)
{
  az_iot_log_sink log = az_iot_log_stderr_sink(AZ_IOT_LOG_LEVEL_INFO);
  az_iot_log_set_global_sink(&log);

  sign_provider provider = { .base = { .vtable = &k_sign_vtable }, .sign_calls = 0 };
  az_iot_connection_client connection_client = { 0 };
  az_iot_mqtt_factory factory
      = { .version = AZ_IOT_MQTT_VERSION_3_1_1, .create = standin_create, .factory_ctx = NULL };

  az_iot_connection_client_options copts = az_iot_connection_client_options_default();
  copts.host = "example.invalid";
  copts.client_id = "hsm-sign-callback-sample";
  copts.connection_profile = AZ_IOT_CONNECTION_PROFILE_CLASSIC;
  copts.certificate_provider = &provider.base;

  int rc = 1;
  if (az_iot_connection_client_init(&connection_client, &copts) != AZ_IOT_OK)
  {
    goto cleanup;
  }
  az_iot_connection_client_set_state_callback(&connection_client, on_conn_state, NULL);
  if (az_iot_connection_client_register_mqtt_factory(&connection_client, &factory) != AZ_IOT_OK)
  {
    goto cleanup;
  }

  /* open() reaches the adapter's connect(), which is where the hook is used.
   * The stand-in adapter has no transport, so the expected outcome is
   * AZ_IOT_ERR_NOT_SUPPORTED *after* a successful signature. */
  az_iot_result open_rc = az_iot_connection_client_open(&connection_client);
  fprintf(
      stderr,
      "[hsm_sign_callback] open() returned %s after %d sign() call(s)\n",
      az_iot_result_to_string(open_rc),
      provider.sign_calls);

  rc = (provider.sign_calls == 1) ? 0 : 1;

cleanup:
  az_iot_connection_client_destroy(&connection_client);
  return rc;
}
