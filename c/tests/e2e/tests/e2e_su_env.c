// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */

#include "e2e_su_env.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "support/test_env.h"

const char* e2e_su_env_require(const char* name)
{
  const char* v = az_iot_test_env(name);
  if (v == NULL)
  {
    printf("software updates e2e: required variable %s is not set\n", name);
  }
  return v;
}

/**
 * @brief Parses `[scheme://]host[:port][/]` from `https_proxy`.
 *
 * @return 0 when unset or parsed; non-zero for a form this suite cannot use
 *   (credentials, or a host or port that does not fit).
 */
static int load_proxy(e2e_su_env* env)
{
  const char* v = az_iot_test_env("https_proxy");
  if (v == NULL)
  {
    return 0;
  }
  const char* host = strstr(v, "://");
  host = (host != NULL) ? host + 3 : v;
  if (strchr(host, '@') != NULL)
  {
    printf("software updates e2e: https_proxy with credentials is not supported\n");
    return 1;
  }
  size_t len = strcspn(host, ":/");
  if (len == 0 || len >= sizeof(env->proxy_host))
  {
    printf("software updates e2e: https_proxy host is empty or too long\n");
    return 1;
  }
  memcpy(env->proxy_host, host, len);
  env->proxy_host[len] = '\0';
  env->proxy_port = 0;
  if (host[len] == ':')
  {
    char* end = NULL;
    unsigned long port = strtoul(host + len + 1, &end, 10);
    if (end == host + len + 1 || port == 0 || port > 65535 || (*end != '\0' && *end != '/'))
    {
      printf("software updates e2e: https_proxy port is not valid\n");
      return 1;
    }
    env->proxy_port = (uint16_t)port;
  }
  return 0;
}

int e2e_su_env_load(e2e_su_env* env)
{
  memset(env, 0, sizeof(*env));
  env->dps_host = e2e_su_env_require("AZ_IOT_E2E_SU_DPS_HOST");
  env->id_scope = e2e_su_env_require("AZ_IOT_E2E_SU_ID_SCOPE");
  env->registration_id = e2e_su_env_require("AZ_IOT_E2E_SU_REG_ID");
  env->cert_path = e2e_su_env_require("AZ_IOT_E2E_SU_CERT");
  env->key_path = e2e_su_env_require("AZ_IOT_E2E_SU_KEY");
  env->trusted_ca = e2e_su_env_require("AZ_IOT_E2E_SU_TRUSTED_CA");

  int rc = load_proxy(env);
  if (env->dps_host == NULL || env->id_scope == NULL || env->registration_id == NULL
      || env->cert_path == NULL || env->key_path == NULL || env->trusted_ca == NULL)
  {
    rc = 1;
  }
  return rc;
}

void e2e_su_env_apply(
    const e2e_su_env* env,
    az_iot_connection_client_options* conn,
    az_iot_certificate_provider_pem_options* pem)
{
  pem->client_cert_pem_path = env->cert_path;
  pem->client_key_pem_path = env->key_path;
  pem->trusted_ca_pem_path = env->trusted_ca;

  conn->host = NULL; /* DPS mode */
  conn->client_id = env->registration_id;
  conn->dps.id_scope = env->id_scope;
  conn->dps.registration_id = env->registration_id;
  conn->dps.global_endpoint = env->dps_host;
  if (env->proxy_host[0] != '\0')
  {
    conn->proxy.host = env->proxy_host;
    conn->proxy.port = env->proxy_port;
  }
}
