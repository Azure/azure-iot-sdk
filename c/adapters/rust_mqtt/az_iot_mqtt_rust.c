// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Rust MQTT adapter shell.
 *
 * Wraps a runtime-installed FFI table (`az_iot_rust_mqtt_ffi`) into
 * the standard `az_iot_mqtt_iface` vtable. The Rust cdylib is expected
 * to call `az_iot_rust_mqtt_install()` once at startup. Until that
 * happens, the v5 factory returns NULL and the adapter is effectively
 * absent. This keeps the C library free of any link-time Rust dependency
 * for P0.
 */
#include <stdlib.h>
#include <string.h>

#include "azure/iot/adapters/az_iot_adapter_rust_mqtt.h"
#include "az_iot_mqtt_rust_ffi.h"
#include "internal/mqtt_limits.h"

/* Globally installed FFI table. NULL means "no Rust runtime present". */
static az_iot_rust_mqtt_ffi g_ffi;
static int g_ffi_installed = 0;

AZ_NODISCARD az_iot_result az_iot_rust_mqtt_install(const az_iot_rust_mqtt_ffi* table)
{
  if (table == NULL)
  {
    memset(&g_ffi, 0, sizeof(g_ffi));
    g_ffi_installed = 0;
    return AZ_IOT_OK;
  }
  if (table->create == NULL || table->destroy == NULL || table->connect == NULL
      || table->disconnect == NULL || table->subscribe == NULL || table->unsubscribe == NULL
      || table->publish == NULL || table->process_loop == NULL || table->set_inbound_cb == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  g_ffi = *table;
  g_ffi_installed = 1;
  return AZ_IOT_OK;
}

/* ------------------------------------------------------------------------- */
/* Per-client wrapper. The first member must be `iface` so the core can
 * dispatch generically through `az_iot_mqtt_client`. */

typedef struct rust_client
{
  const az_iot_mqtt_iface* iface;
  az_iot_rust_mqtt_client* handle;
  /* Factory client: the allocation destroy() frees. NULL: built in an application workspace. */
  void* allocation;
} rust_client;

static az_iot_result rust_iface_connect(
    az_iot_mqtt_client* self,
    const az_iot_mqtt_connect_options* opts)
{
  rust_client* c = (rust_client*)self;
  /* Refuse what this adapter cannot carry, rather than passing it to a Rust
   * runtime that predates these fields and would silently ignore them: a
   * connect that quietly bypasses the caller's proxy, or falls back to 8883
   * when WebSockets were required, is worse than a refused connect. The FFI
   * table carries no capability flag to negotiate this, so the refusal is
   * unconditional until one is added together with runtime support. */
  if (opts != NULL
      && (opts->transport != AZ_IOT_MQTT_TRANSPORT_TCP
          || (opts->proxy.host != NULL && opts->proxy.host[0] != '\0')))
  {
    return AZ_IOT_ERR_NOT_SUPPORTED;
  }
  return g_ffi.connect(c->handle, opts);
}

static az_iot_result rust_iface_disconnect(az_iot_mqtt_client* self)
{
  rust_client* c = (rust_client*)self;
  return g_ffi.disconnect(c->handle);
}

static az_iot_result rust_iface_subscribe(
    az_iot_mqtt_client* self,
    const char* topic_filter,
    az_iot_mqtt_qos qos,
    uint16_t* out_packet_id)
{
  rust_client* c = (rust_client*)self;
  return g_ffi.subscribe(c->handle, topic_filter, qos, out_packet_id);
}

static az_iot_result rust_iface_unsubscribe(
    az_iot_mqtt_client* self,
    const char* topic_filter,
    uint16_t* out_packet_id)
{
  rust_client* c = (rust_client*)self;
  return g_ffi.unsubscribe(c->handle, topic_filter, out_packet_id);
}

static az_iot_result rust_iface_publish(
    az_iot_mqtt_client* self,
    const az_iot_mqtt_message* msg,
    uint16_t* out_packet_id)
{
  rust_client* c = (rust_client*)self;
  return g_ffi.publish(c->handle, msg, out_packet_id);
}

static az_iot_result rust_iface_process_loop(az_iot_mqtt_client* self, uint32_t timeout_ms)
{
  rust_client* c = (rust_client*)self;
  return g_ffi.process_loop(c->handle, timeout_ms);
}

static void rust_iface_set_inbound_cb(
    az_iot_mqtt_client* self,
    az_iot_mqtt_event_callback cb,
    void* user_ctx)
{
  rust_client* c = (rust_client*)self;
  g_ffi.set_inbound_cb(c->handle, cb, user_ctx);
}

static void rust_iface_destroy(az_iot_mqtt_client* self)
{
  if (self == NULL)
  {
    return;
  }
  rust_client* c = (rust_client*)self;
  if (c->handle != NULL && g_ffi_installed)
  {
    g_ffi.destroy(c->handle);
  }
  void* const allocation = c->allocation;
  memset(c, 0, sizeof(*c));
  free(allocation);
}

static const az_iot_mqtt_iface s_rust_iface_v5 = {
  .version = AZ_IOT_MQTT_VERSION_5,
  .connect = rust_iface_connect,
  .disconnect = rust_iface_disconnect,
  .subscribe = rust_iface_subscribe,
  .unsubscribe = rust_iface_unsubscribe,
  .publish = rust_iface_publish,
  .process_loop = rust_iface_process_loop,
  .set_inbound_cb = rust_iface_set_inbound_cb,
  .destroy = rust_iface_destroy,
};

/* ------------------------------------------------------------------------- */
/* Adapter (application workspace) and factory. */

/* AZ_IOT_RUST_MQTT_WORKSPACE_SIZE relies on this bound. */
typedef char rust_client_fits_AZ_IOT_RUST_MQTT_WORKSPACE_SIZE
    [sizeof(rust_client) + sizeof(void*) - 1u <= AZ_IOT_RUST_MQTT_WORKSPACE_SIZE ? 1 : -1];

size_t az_iot_rust_mqtt_get_workspace_size(const az_iot_mqtt_limits* limits)
{
  return az_iot_mqtt__limits_are_valid(limits) ? sizeof(rust_client) + sizeof(void*) - 1u : 0;
}

/** @brief az_iot_mqtt_adapter::init: the wrapper in @p workspace; the Rust client allocates its
 * own state. */
static az_iot_result rust_adapter_init(
    az_iot_mqtt_version mqtt_version,
    const az_iot_mqtt_limits* limits,
    uint8_t* workspace,
    size_t workspace_size,
    az_iot_mqtt_client** out_client)
{
  if (out_client == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  *out_client = NULL;
  if (workspace == NULL || !az_iot_mqtt__limits_are_valid(limits))
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (mqtt_version != AZ_IOT_MQTT_VERSION_5 || !g_ffi_installed)
  {
    return AZ_IOT_ERR_NOT_SUPPORTED;
  }
  if (workspace_size < az_iot_rust_mqtt_get_workspace_size(limits))
  {
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }

  az_iot_rust_mqtt_client* handle = g_ffi.create(AZ_IOT_MQTT_VERSION_5);
  if (handle == NULL)
  {
    return AZ_IOT_ERR_OUT_OF_MEMORY;
  }
  uintptr_t const align = sizeof(void*);
  rust_client* c = (rust_client*)(void*)(((uintptr_t)workspace + align - 1u) & ~(align - 1u));
  memset(c, 0, sizeof(*c));
  c->iface = &s_rust_iface_v5;
  c->handle = handle;
  *out_client = (az_iot_mqtt_client*)c;
  return AZ_IOT_OK;
}

static const az_iot_mqtt_adapter s_rust_adapter = {
  .get_workspace_size = az_iot_rust_mqtt_get_workspace_size,
  .init = rust_adapter_init,
};

const az_iot_mqtt_adapter* az_iot_rust_mqtt_get_adapter(void) { return &s_rust_adapter; }

static az_iot_mqtt_client* rust_factory_create(void* factory_ctx)
{
  (void)factory_ctx;
  static const az_iot_mqtt_limits limits = { 1, 1, 1, 0, 0 }; /* The Rust client sizes its own. */
  size_t const size = az_iot_rust_mqtt_get_workspace_size(&limits);
  if (size == 0)
  {
    return NULL;
  }
  uint8_t* allocation = (uint8_t*)malloc(size);
  az_iot_mqtt_client* client = NULL;
  if (allocation == NULL
      || rust_adapter_init(AZ_IOT_MQTT_VERSION_5, &limits, allocation, size, &client) != AZ_IOT_OK)
  {
    free(allocation);
    return NULL;
  }
  ((rust_client*)client)->allocation = allocation;
  return client;
}

az_iot_mqtt_factory* az_iot_rust_mqtt_factory_create_v5(void)
{
  if (!g_ffi_installed)
  {
    return NULL;
  }
  az_iot_mqtt_factory* f = (az_iot_mqtt_factory*)calloc(1, sizeof(*f));
  if (f == NULL)
  {
    return NULL;
  }
  f->version = AZ_IOT_MQTT_VERSION_5;
  f->create = rust_factory_create;
  f->factory_ctx = f;
  return f;
}

void az_iot_rust_mqtt_factory_destroy(az_iot_mqtt_factory* factory) { free(factory); }
