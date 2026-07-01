// Copyright (c) Microsoft Corporation. All rights reserved.
// SPDX-License-Identifier: MIT

/**
 * @file
 *
 * @brief The single public header for the AMQP 1.0 client library.
 *
 * @details Include this to gain the full client surface: the pluggable transport, the AMQP type
 * system, messages, and the connection/session/sender/receiver objects, plus the SASL and CBS
 * authentication layers. The library is built on the Azure SDK for C core (`az_span`, `az_result`,
 * `az_context`), performs no dynamic memory allocation, and is driven by a single non-blocking
 * pump (#az_amqp_connection_process) so it runs unchanged on Linux, Windows, and embedded targets
 * such as the ESP32.
 *
 * @note You MUST NOT use any symbols (macros, functions, structures, enums, etc.)
 * prefixed with an underscore ('_') directly in your application code. These symbols
 * are part of the AMQP client's internal implementation; we do not document these symbols
 * and they are subject to change in future versions of the SDK which would break your code.
 */

#ifndef _az_AMQP_H
#define _az_AMQP_H

#include <azure/amqp/az_amqp_cbs.h>
#include <azure/amqp/az_amqp_common.h>
#include <azure/amqp/az_amqp_connection.h>
#include <azure/amqp/az_amqp_link.h>
#include <azure/amqp/az_amqp_message.h>
#include <azure/amqp/az_amqp_sasl.h>
#include <azure/amqp/az_amqp_session.h>
#include <azure/amqp/az_amqp_transport.h>
#include <azure/amqp/az_amqp_value.h>
#include <azure/amqp/az_amqp_version.h>

#endif // _az_AMQP_H
