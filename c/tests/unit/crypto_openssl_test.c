// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* OpenSSL crypto backend: known-answer contract suite. */
#include "az_iot_crypto_openssl.h"
#include "support/crypto_contract.h"

int main(void) { return crypto_contract_run("crypto_openssl", az_iot_crypto_openssl()); }
