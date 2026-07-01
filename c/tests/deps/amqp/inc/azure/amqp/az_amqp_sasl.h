// Copyright (c) Microsoft Corporation. All rights reserved.
// SPDX-License-Identifier: MIT

/**
 * @file
 *
 * @brief SASL security-layer configuration for the AMQP connection (OASIS AMQP 1.0 §5.3).
 *
 * @details Before the AMQP connection is opened, an optional SASL negotiation authenticates the
 * client to the peer over the same transport. The application selects a mechanism and supplies any
 * required credentials through an #az_amqp_sasl_options, which it then attaches to the
 * #az_amqp_connection_options. The connection drives the SASL frame exchange as part of its
 * non-blocking pump; there is no separate object to pump.
 *
 * @note You MUST NOT use any symbols (macros, functions, structures, enums, etc.)
 * prefixed with an underscore ('_') directly in your application code. These symbols
 * are part of the AMQP client's internal implementation; we do not document these symbols
 * and they are subject to change in future versions of the SDK which would break your code.
 */

#ifndef _az_AMQP_SASL_H
#define _az_AMQP_SASL_H

#include <azure/amqp/az_amqp_common.h>

#include <azure/core/az_result.h>
#include <azure/core/az_span.h>

#include <azure/core/_az_cfg_prefix.h>

/**
 * @brief The SASL mechanism the client offers to the peer.
 */
typedef enum
{
  /// No SASL layer; AMQP bytes start immediately after the protocol header. Use this when
  /// authentication is carried out-of-band (for example, via CBS put-token over a session, or via
  /// mutual TLS established by the transport).
  AZ_AMQP_SASL_MECHANISM_NONE = 0,

  /// `ANONYMOUS` (RFC 4505): no credentials are exchanged.
  AZ_AMQP_SASL_MECHANISM_ANONYMOUS = 1,

  /// `PLAIN` (RFC 4616): a username and password are sent (protected by the TLS transport).
  AZ_AMQP_SASL_MECHANISM_PLAIN = 2,

  /// `EXTERNAL` (RFC 4422): authentication is derived from the TLS client certificate presented by
  /// the transport; an optional authorization identity may be supplied.
  AZ_AMQP_SASL_MECHANISM_EXTERNAL = 3,
} az_amqp_sasl_mechanism;

/**
 * @brief SASL configuration attached to an #az_amqp_connection_options.
 *
 * @details Only the fields relevant to the selected #mechanism are read. Credential spans are
 * caller-owned and must remain valid until the connection has finished opening.
 */
typedef struct
{
  az_amqp_sasl_mechanism mechanism; ///< The mechanism to use. Default #AZ_AMQP_SASL_MECHANISM_NONE.

  /// `PLAIN`: the authentication identity (username). `EXTERNAL`: unused.
  az_span username;

  /// `PLAIN`: the password. Sent only over a TLS transport. Ignored for other mechanisms.
  az_span password;

  /// `PLAIN`/`EXTERNAL`: an optional authorization identity to act as. Empty to omit.
  az_span authorization_identity;
} az_amqp_sasl_options;

/**
 * @brief Gets the default SASL options (#AZ_AMQP_SASL_MECHANISM_NONE, all credentials empty).
 *
 * @return An initialized #az_amqp_sasl_options.
 */
AZ_NODISCARD az_amqp_sasl_options az_amqp_sasl_options_default(void);

#include <azure/core/_az_cfg_suffix.h>

#endif // _az_AMQP_SASL_H
