// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
#include "internal/dispatch.h"

#include <string.h>

void az_iot_dispatch_init(az_iot_dispatch_table* tbl)
{
  if (tbl)
    memset(tbl, 0, sizeof(*tbl));
}

az_iot_result az_iot_dispatch_register_prefix(
    az_iot_dispatch_table* tbl,
    const char* topic_prefix,
    az_iot_inbound_handler_callback cb,
    void* user_ctx)
{
  if (!tbl || !topic_prefix || !cb)
    return AZ_IOT_ERR_INVALID_ARG;

  size_t n = strlen(topic_prefix);
  if (n + 1 > AZ_IOT_DISPATCH_PREFIX_MAX)
    return AZ_IOT_ERR_NOT_SUPPORTED;

  /* Find an empty slot, and reject a prefix that is already claimed.
   *
   * Routing is longest-prefix-wins with no tie-break, so a second registration
   * for the identical prefix would shadow the first and the loser would
   * silently receive nothing for the life of the connection. Two feature
   * clients of the same type on one connection is a programming error -- there
   * is one C2D stream, one twin and one method channel per identity -- and it
   * should look like one.
   *
   * The check is deliberately EXACT-match only. Connection multiplexing puts
   * several identities on one connection, and their topics differ by device id
   * ("devices/{device-id}/..."), so distinct identities never collide here and
   * stay free to register side by side. Only a genuine duplicate is refused. */
  az_iot_dispatch_entry* slot = NULL;
  for (size_t i = 0; i < AZ_IOT_MAX_INBOUND_HANDLERS; ++i)
  {
    az_iot_dispatch_entry* e = &tbl->entries[i];
    if (!e->in_use)
    {
      if (!slot)
      {
        slot = e;
      }
      continue;
    }
    if (e->prefix_len == n && memcmp(e->prefix, topic_prefix, n) == 0)
    {
      return AZ_IOT_ERR_ALREADY_INITIALIZED;
    }
  }
  if (!slot)
  {
    return AZ_IOT_ERR_NOT_SUPPORTED;
  }

  memcpy(slot->prefix, topic_prefix, n + 1);
  slot->prefix_len = n;
  slot->cb = cb;
  slot->user_ctx = user_ctx;
  slot->in_use = true;
  tbl->count++;
  return AZ_IOT_OK;
}

size_t az_iot_dispatch_unregister_by_ctx(az_iot_dispatch_table* tbl, void* user_ctx)
{
  if (!tbl)
    return 0;
  size_t removed = 0;
  for (size_t i = 0; i < AZ_IOT_MAX_INBOUND_HANDLERS; ++i)
  {
    az_iot_dispatch_entry* e = &tbl->entries[i];
    if (e->in_use && e->user_ctx == user_ctx)
    {
      memset(e, 0, sizeof(*e));
      tbl->count--;
      removed++;
    }
  }
  return removed;
}

bool az_iot_dispatch_route(const az_iot_dispatch_table* tbl, const az_iot_mqtt_message* msg)
{
  if (!tbl || !msg || !msg->topic)
    return false;
  size_t topic_len = strlen(msg->topic);

  /* Longest-prefix match: walk all entries, track the best one. */
  const az_iot_dispatch_entry* best = NULL;
  for (size_t i = 0; i < AZ_IOT_MAX_INBOUND_HANDLERS; ++i)
  {
    const az_iot_dispatch_entry* e = &tbl->entries[i];
    if (!e->in_use)
      continue;
    if (e->prefix_len > topic_len)
      continue;
    if (memcmp(msg->topic, e->prefix, e->prefix_len) != 0)
      continue;
    if (!best || e->prefix_len > best->prefix_len)
      best = e;
  }
  if (!best)
    return false;
  best->cb(best->user_ctx, msg);
  return true;
}

size_t az_iot_dispatch_count(const az_iot_dispatch_table* tbl) { return tbl ? tbl->count : 0; }
