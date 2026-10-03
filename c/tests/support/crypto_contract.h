// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/**
 * @file crypto_contract.h
 * @brief Backend-independent contract suite for az_iot_crypto.
 *
 * Runs the committed known-answer vectors (su_crypto_vectors.h and RFC 4231)
 * through a backend, directly, through the SDK's composed SHA-256 and
 * HMAC-SHA256, and through az_iot_su_parse_update_request(), so every backend
 * is held to the same accept/reject behaviour.
 */
#ifndef CRYPTO_CONTRACT_H
#define CRYPTO_CONTRACT_H

#include "azure/iot/az_iot_crypto.h"

/**
 * @brief Run the contract suite against @p crypto.
 *
 * @param group_name cmocka group name, e.g. the backend name.
 * @param crypto     Backend under test; must outlive the call.
 * @return Number of failed tests (0 on success).
 */
int crypto_contract_run(const char* group_name, const az_iot_crypto* crypto);

#endif /* CRYPTO_CONTRACT_H */
