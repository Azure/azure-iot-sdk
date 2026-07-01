// Copyright (c) Microsoft Corporation. All rights reserved.
// SPDX-License-Identifier: MIT

#include <azure/amqp/az_amqp_sasl.h>

AZ_NODISCARD az_amqp_sasl_options az_amqp_sasl_options_default(void)
{
  az_amqp_sasl_options options = { 0 };
  options.mechanism = AZ_AMQP_SASL_MECHANISM_NONE;
  options.username = AZ_SPAN_EMPTY;
  options.password = AZ_SPAN_EMPTY;
  options.authorization_identity = AZ_SPAN_EMPTY;
  return options;
}
