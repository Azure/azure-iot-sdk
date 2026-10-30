// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/**
 * @file direct_method_delivery.h
 * @brief Delivery of the MQTTv5 direct method replies (probe-acks, results, probe rejections and
 *        abandons) through the client's delivery tracker: sent at once or held, published again
 *        after a reconnect, and given up at their deadline.
 */
#ifndef AZ_IOT_MQTTV5_DIRECT_METHOD_DELIVERY_H
#define AZ_IOT_MQTTV5_DIRECT_METHOD_DELIVERY_H

#include <stddef.h>
#include <stdint.h>

#include "azure/iot/az_iot_connection_client.h"
#include "azure/iot/mqttv5/az_iot_direct_method_client.h"

#include "internal/direct_method_codec.h"

#ifdef __cplusplus
extern "C"
{
#endif

/**
 * @brief Version of every phase's `type` ("probe:1", "result:1", ...). Part of the dispatch key: a
 *        later "exec:2" may carry another payload.
 */
#define AZ_IOT_MQTTV5_DM_TYPE_VERSION "1"

/** @brief A pool element's tracker entry is stored plus one, so 0 means none. */
#define AZ_IOT_MQTTV5_DM_NO_TRACKER_ENTRY 0u

  /** @brief Set up the client's delivery tracker and reserve its PUBACK slots on @p conn. */
  az_iot_result az_iot_mqttv5_direct_method_client__start_reply_delivery(
      az_iot_mqttv5_direct_method_client* client,
      az_iot_connection_client* conn);

  /** @brief Drop every reply without completing it, and give the PUBACK slots back. */
  void az_iot_mqttv5_direct_method_client__stop_reply_delivery(
      az_iot_mqttv5_direct_method_client* client);

  /**
   * @brief Send the ProbeAck{ready} of ready token @p slot by @p deadline_ms.
   * @return AZ_IOT_OK when sent or held; its completion may already have ended the token.
   */
  az_iot_result az_iot_mqttv5_direct_method_client__deliver_probe_ack(
      az_iot_mqttv5_direct_method_client* client,
      az_iot_mqttv5_direct_method_ready_slot* slot,
      uint64_t deadline_ms);

  /**
   * @brief Send the encoded result of req_pool[@p index] by its result deadline; its slot is freed
   *        when the result ends.
   */
  az_iot_result az_iot_mqttv5_direct_method_client__deliver_result(
      az_iot_mqttv5_direct_method_client* client,
      size_t index);

  /**
   * @brief Send a ProbeAck{rejected} @p frame, kept until its PUBACK; best effort when every notice
   *        is in use.
   * @param budget_seconds Message Expiry counted from @p start_ms; 0 sends none.
   */
  void az_iot_mqttv5_direct_method_client__send_probe_rejection(
      az_iot_mqttv5_direct_method_client* client,
      const uint8_t* frame,
      size_t frame_len,
      const uint8_t* request_id,
      uint32_t budget_seconds,
      uint64_t start_ms);

  /**
   * @brief Send the advisory Abandon of ready token @p slot, which ended before execution began.
   * @param budget_seconds Budget the caller still waits within, from @p start_ms; 0 uses the
   *        response timeout from now.
   */
  void az_iot_mqttv5_direct_method_client__send_abandon(
      az_iot_mqttv5_direct_method_client* client,
      const az_iot_mqttv5_direct_method_ready_slot* slot,
      az_iot_dm_proto_abandon_reason reason,
      uint32_t budget_seconds,
      uint64_t start_ms);

  /** @brief Stop sending the reply @p tracker_entry names, if any; its late PUBACK is ignored. */
  void az_iot_mqttv5_direct_method_client__cancel_reply(
      az_iot_mqttv5_direct_method_client* client,
      size_t* tracker_entry);

  /** @brief End now every reply past its deadline, freeing what it held. */
  void az_iot_mqttv5_direct_method_client__expire_overdue_replies(
      az_iot_mqttv5_direct_method_client* client);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_MQTTV5_DIRECT_METHOD_DELIVERY_H */
