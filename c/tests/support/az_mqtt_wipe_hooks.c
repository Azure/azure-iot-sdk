// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* The hooks of az_mqtt_wipe_hooks.h, which is not included: malloc and free are the C library's. */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

void* az_mqtt_wipe_test_malloc(size_t size);
void* az_mqtt_wipe_test_calloc(size_t count, size_t size);
void az_mqtt_wipe_test_free(void* p);

char const* az_mqtt_wipe_test_secret = NULL;
int az_mqtt_wipe_test_frees = 0;
int az_mqtt_wipe_test_leaks = 0;

#define BLOCKS_MAX 256

static struct
{
  void* p;
  size_t size;
} g_blocks[BLOCKS_MAX];

static void record(void* p, size_t size)
{
  if (p == NULL)
  {
    return;
  }
  for (size_t i = 0; i < BLOCKS_MAX; i++)
  {
    if (g_blocks[i].p == NULL)
    {
      g_blocks[i].p = p;
      g_blocks[i].size = size;
      return;
    }
  }
  abort(); // More live blocks than recorded: raise BLOCKS_MAX.
}

/** @brief Whether @p size bytes at @p data hold @p text. */
static int holds(void const* data, size_t size, char const* text)
{
  uint8_t const* b = (uint8_t const*)data;
  size_t const n = strlen(text);
  for (size_t i = 0; i + n <= size; i++)
  {
    if (memcmp(b + i, text, n) == 0)
    {
      return 1;
    }
  }
  return 0;
}

void* az_mqtt_wipe_test_malloc(size_t size)
{
  void* p = malloc(size);
  record(p, size);
  return p;
}

void* az_mqtt_wipe_test_calloc(size_t count, size_t size)
{
  void* p = calloc(count, size);
  record(p, count * size);
  return p;
}

void az_mqtt_wipe_test_free(void* p)
{
  for (size_t i = 0; p != NULL && i < BLOCKS_MAX; i++)
  {
    if (g_blocks[i].p == p)
    {
      az_mqtt_wipe_test_frees++;
      if (az_mqtt_wipe_test_secret != NULL && holds(p, g_blocks[i].size, az_mqtt_wipe_test_secret))
      {
        az_mqtt_wipe_test_leaks++;
      }
      g_blocks[i].p = NULL;
      break;
    }
  }
  free(p);
}
