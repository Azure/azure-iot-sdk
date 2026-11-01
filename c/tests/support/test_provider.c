// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
#include "support/test_provider.h"

#include <stdlib.h>
#include <string.h>

#include "support/mock_mqtt_iface.h"

static az_iot_result test_provider_load(
    az_iot_certificate_provider* self,
    az_iot_cert_role role,
    uint8_t index,
    az_iot_certificate_material* out_material)
{
  if (index != 0)
  {
    return AZ_IOT_ERR_NOT_FOUND;
  }
  (void)role;
  if (self == NULL || out_material == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  memset(out_material, 0, sizeof(*out_material));
  out_material->trusted_ca_path = ((az_iot_test_provider*)self)->trusted_ca_path;
  return AZ_IOT_OK;
}

static void test_provider_release(
    az_iot_certificate_provider* self,
    az_iot_certificate_material* material)
{
  (void)self;
  (void)material;
}

static void test_provider_deinit(az_iot_certificate_provider* self) { (void)self; }

static const az_iot_certificate_provider_vtable k_test_provider_vtable = {
  .version = AZ_IOT_CERTIFICATE_PROVIDER_VTABLE_VERSION,
  .load = test_provider_load,
  .release = test_provider_release,
  .deinit = test_provider_deinit,
};

void az_iot_test_provider_init(az_iot_test_provider* provider, const char* trusted_ca_path)
{
  memset(provider, 0, sizeof(*provider));
  provider->base.vtable = &k_test_provider_vtable;
  provider->trusted_ca_path = trusted_ca_path;
}

/** @brief Clients alive at once that az_iot_test_connection_client_init() gives a workspace. */
#define TEST_CONNECTION_WORKSPACE_COUNT 32

/**
 * @brief A default-size workspace, an MQTT workspace allocated for the client when it needs one,
 * and the client they are lent to; NULL client: free.
 */
typedef struct test_connection_workspace
{
  const az_iot_connection_client* client;
  uint8_t workspace[AZ_IOT_CONNECTION_CLIENT_DEFAULT_WORKSPACE_SIZE];
  uint8_t* mqtt_workspace;
} test_connection_workspace;

static test_connection_workspace s_connection_workspaces[TEST_CONNECTION_WORKSPACE_COUNT];

/** @brief The workspace lent to @p client, or NULL. */
static test_connection_workspace* test_connection_workspace_lent_to(
    const az_iot_connection_client* client)
{
  for (size_t i = 0; i < TEST_CONNECTION_WORKSPACE_COUNT; ++i)
  {
    if (s_connection_workspaces[i].client == client)
    {
      return &s_connection_workspaces[i];
    }
  }
  return NULL;
}

/**
 * @brief Lends @p client a workspace: the one it holds, else a free one; NULL when none is free.
 * Live clients have distinct addresses, so they never share one.
 */
static test_connection_workspace* test_connection_workspace_lend(
    const az_iot_connection_client* client)
{
  test_connection_workspace* lent = test_connection_workspace_lent_to(client);
  if (lent == NULL)
  {
    lent = test_connection_workspace_lent_to(NULL);
  }
  if (lent != NULL)
  {
    lent->client = client;
  }
  return lent;
}

/** @brief Takes @p lent back: frees its MQTT workspace and makes it free. */
static void test_connection_workspace_return(test_connection_workspace* lent)
{
  free(lent->mqtt_workspace);
  lent->mqtt_workspace = NULL;
  lent->client = NULL;
}

az_iot_result az_iot_test_connection_client_init(
    az_iot_connection_client* client,
    const az_iot_connection_client_options* opts)
{
  /* Stateless, so one instance serves every client. */
  static az_iot_test_provider s_provider = { .base = { .vtable = &k_test_provider_vtable } };
  if (client == NULL || opts == NULL)
  {
    return az_iot_connection_client_init(client, opts);
  }
  az_iot_connection_client_options with_defaults = *opts;
  if (with_defaults.certificate_provider == NULL)
  {
    with_defaults.certificate_provider = &s_provider.base;
  }
  bool const lend_workspace = with_defaults.workspace == NULL
      && az_iot_connection_client_get_workspace_size(&opts->limits)
          <= AZ_IOT_CONNECTION_CLIENT_DEFAULT_WORKSPACE_SIZE;
  const az_iot_mqtt_limits* mqtt_limits = &with_defaults.mqtt_limits;
  if (with_defaults.mqtt_adapter != NULL && mqtt_limits->send_buffer_size == 0
      && mqtt_limits->receive_buffer_size == 0 && mqtt_limits->max_in_flight_messages == 0
      && mqtt_limits->message_store_size == 0 && mqtt_limits->connect_strings_size == 0)
  {
    with_defaults.mqtt_limits = az_iot_mock_mqtt_limits;
  }
  /* 0 when the adapter or limits are invalid: init() reports that. */
  size_t const mqtt_workspace_size = with_defaults.mqtt_adapter != NULL
          && with_defaults.mqtt_workspace == NULL
          && with_defaults.mqtt_adapter->get_workspace_size != NULL
      ? with_defaults.mqtt_adapter->get_workspace_size(&with_defaults.mqtt_limits)
      : 0;
  test_connection_workspace* lent = NULL;
  if (lend_workspace || mqtt_workspace_size > 0)
  {
    lent = test_connection_workspace_lend(client);
    if (lent == NULL)
    {
      return AZ_IOT_ERR_OUT_OF_MEMORY;
    }
  }
  if (lend_workspace)
  {
    with_defaults.workspace = lent->workspace;
    with_defaults.workspace_size = sizeof(lent->workspace);
  }
  if (mqtt_workspace_size > 0)
  {
    free(lent->mqtt_workspace);
    lent->mqtt_workspace = (uint8_t*)malloc(mqtt_workspace_size);
    if (lent->mqtt_workspace == NULL)
    {
      test_connection_workspace_return(lent);
      return AZ_IOT_ERR_OUT_OF_MEMORY;
    }
    with_defaults.mqtt_workspace = lent->mqtt_workspace;
    with_defaults.mqtt_workspace_size = mqtt_workspace_size;
  }
  az_iot_result result = az_iot_connection_client_init(client, &with_defaults);
  if (result != AZ_IOT_OK && lent != NULL)
  {
    test_connection_workspace_return(lent);
  }
  return result;
}

az_iot_result az_iot_test_connection_client_deinit(az_iot_connection_client* client)
{
  az_iot_result result = az_iot_connection_client_deinit(client);
  test_connection_workspace* lent
      = result == AZ_IOT_OK ? test_connection_workspace_lent_to(client) : NULL;
  if (lent != NULL)
  {
    /* A later deinit (a teardown after an explicit one) must not wipe a returned loan. */
    if (client->opts.mqtt_workspace == lent->mqtt_workspace)
    {
      client->opts.mqtt_workspace = NULL;
      client->opts.mqtt_workspace_size = 0;
    }
    if (client->opts.workspace == lent->workspace)
    {
      client->opts.workspace = NULL;
      client->opts.workspace_size = 0;
    }
    test_connection_workspace_return(lent);
  }
  return result;
}
