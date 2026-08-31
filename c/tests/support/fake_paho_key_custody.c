// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Implementation of the key-custody test double. See fake_paho_key_custody.h. */

#include "fake_paho_key_custody.h"

#include "az_iot_paho_key_custody.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static az_iot_result s_prepare_result = AZ_IOT_OK;
static int s_prepare_calls;
static int s_release_calls;

/* Kept after release() so a test can look for the file the adapter should have
 * removed. */
static char s_last_path[512];

/* The temporary directory, spelled the way each platform spells it. */
static const char* temp_dir(void)
{
  static const char* const names[] = { "TMPDIR", "TEMP", "TMP" };
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
  {
    const char* v = getenv(names[i]);
    if (v && v[0] != '\0')
    {
      return v;
    }
  }
#if defined(_WIN32)
  return ".";
#else
  return "/tmp";
#endif
}

static bool file_exists(const char* path)
{
  FILE* f = fopen(path, "rb");
  if (!f)
  {
    return false;
  }
  fclose(f);
  return true;
}

void fake_custody_reset(void)
{
  if (s_last_path[0] != '\0')
  {
    remove(s_last_path);
    s_last_path[0] = '\0';
  }
  s_prepare_result = AZ_IOT_OK;
  s_prepare_calls = 0;
  s_release_calls = 0;
}

void fake_custody_set_prepare_result(az_iot_result rc) { s_prepare_result = rc; }

const char* fake_custody_reference_path(void)
{
  return s_last_path[0] != '\0' ? s_last_path : NULL;
}

bool fake_custody_reference_file_exists(void)
{
  return s_last_path[0] != '\0' && file_exists(s_last_path);
}

int fake_custody_prepare_calls(void) { return s_prepare_calls; }
int fake_custody_release_calls(void) { return s_release_calls; }

/* ------------------------------------------------------------------------- */
/* the surface az_iot_mqtt_paho.c links against                              */
/* ------------------------------------------------------------------------- */

/* Same rule as the real one: a key URI or a sign() hook means the key is not
 * something the adapter can simply read off disk. */
bool az_iot_paho_key_custody_requested(const az_iot_mqtt_tls_options* tls)
{
  return tls != NULL && (tls->client_key_uri != NULL || tls->sign != NULL);
}

az_iot_result az_iot_paho_key_custody_prepare(
    az_iot_paho_key_custody* state,
    const az_iot_mqtt_tls_options* tls,
    const char** out_private_key_path)
{
  ++s_prepare_calls;
  if (!state || !tls || !out_private_key_path)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (!az_iot_paho_key_custody_requested(tls))
  {
    *out_private_key_path = tls->client_key_path;
    return AZ_IOT_OK;
  }
  if (s_prepare_result != AZ_IOT_OK)
  {
    return s_prepare_result;
  }

  /* A distinct name per prepare(), so "the file from the previous connect was
   * removed" is a real observation and not the same path being rewritten. */
  static unsigned s_seq;
  char path[512];
  snprintf(path, sizeof(path), "%s/az-iot-ut-keyref-%u.pem", temp_dir(), ++s_seq);

  FILE* f = fopen(path, "w");
  if (!f)
  {
    return AZ_IOT_ERR_OUT_OF_MEMORY;
  }
  /* Contents are irrelevant: nothing in this suite decodes the file. */
  fputs("-----BEGIN PKCS#11 PROVIDER URI-----\nfake\n-----END PKCS#11 PROVIDER URI-----\n", f);
  fclose(f);

  size_t n = strlen(path);
  state->key_ref_path = (char*)malloc(n + 1);
  if (!state->key_ref_path)
  {
    remove(path);
    return AZ_IOT_ERR_OUT_OF_MEMORY;
  }
  memcpy(state->key_ref_path, path, n + 1);
  memcpy(s_last_path, path, n + 1);

  *out_private_key_path = state->key_ref_path;
  return AZ_IOT_OK;
}

void az_iot_paho_key_custody_release(az_iot_paho_key_custody* state)
{
  ++s_release_calls;
  if (!state)
  {
    return;
  }
  if (state->key_ref_path)
  {
    remove(state->key_ref_path);
    free(state->key_ref_path);
    state->key_ref_path = NULL;
  }
}
