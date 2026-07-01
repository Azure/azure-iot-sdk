// Copyright (c) Microsoft Corporation. All rights reserved.
// SPDX-License-Identifier: MIT

#include <azure/amqp/az_amqp_transport.h>

AZ_NODISCARD az_result az_amqp_transport_init(
    az_amqp_transport* transport,
    az_amqp_transport_vtable const* vtable,
    void* impl)
{
  if (transport == NULL || vtable == NULL)
  {
    return AZ_ERROR_ARG;
  }
  transport->vtable = vtable;
  transport->impl = impl;
  return AZ_OK;
}

AZ_NODISCARD void* az_amqp_transport_get_impl(az_amqp_transport const* transport)
{
  return transport->impl;
}
