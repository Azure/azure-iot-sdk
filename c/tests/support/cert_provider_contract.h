// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/**
 * @file cert_provider_contract.h
 * @brief Provider-independent contract suite for az_iot_certificate_provider.
 *
 * Holds a provider to what az_iot_certificate_provider.h documents: the vtable
 * shape, load() index/NOT_FOUND behaviour, and, when get_csr() is set, CSR
 * validity, key stability, store_issued_certificate() for both wire forms, and
 * all-or-nothing refusal. A custom provider can reuse it with its own fixture.
 * Requires OpenSSL 3.0+ (it issues test certificates).
 */

#ifndef CERT_PROVIDER_CONTRACT_H
#define CERT_PROVIDER_CONTRACT_H

#include <stdbool.h>

#include "azure/iot/az_iot_certificate_provider.h"

/** @brief How the suite creates and resets the provider under test. */
typedef struct cert_provider_contract_fixture
{
  /** cmocka group name, e.g. the provider name. */
  const char* name;
  /** Passed to the hooks. */
  void* ctx;
  /** Delete everything the provider persists, so the next create() starts fresh. */
  void (*reset)(void* ctx);
  /** Initialize the provider over what it persisted; NULL on failure. The suite
   * destroys it through vtable->deinit. */
  az_iot_certificate_provider* (*create)(void* ctx);
  /** A stored chain and the operational key survive deinit and a new create(). */
  bool persists;
} cert_provider_contract_fixture;

/**
 * @brief Run the contract suite against the provider @p fixture creates.
 *
 * @param fixture Must outlive the call.
 * @return Number of failed tests (0 on success).
 */
int cert_provider_contract_run(const cert_provider_contract_fixture* fixture);

#endif /* CERT_PROVIDER_CONTRACT_H */
