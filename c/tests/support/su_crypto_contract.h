// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/**
 * @file su_crypto_contract.h
 * @brief Backend-independent contract suite for az_iot_su_crypto_hooks.
 *
 * Runs the committed known-answer vectors (su_crypto_vectors.h) through a crypto adapter,
 * both directly and through az_iot_su_parse_update_request(), so every adapter is held to
 * the same accept/reject behaviour.
 */
#ifndef SU_CRYPTO_CONTRACT_H
#define SU_CRYPTO_CONTRACT_H

#include "azure/iot/az_iot_su.h"

/**
 * @brief Run the contract suite against @p hooks.
 *
 * @param group_name cmocka group name, e.g. the adapter name.
 * @param hooks      Adapter under test; must outlive the call.
 * @return Number of failed tests (0 on success).
 */
int su_crypto_contract_run(const char* group_name, const az_iot_su_crypto_hooks* hooks);

#endif /* SU_CRYPTO_CONTRACT_H */
