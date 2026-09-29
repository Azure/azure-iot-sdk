// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Software updates OpenSSL crypto adapter: known-answer contract suite. */
#include "az_iot_su_crypto_openssl.h"
#include "support/su_crypto_contract.h"

int main(void)
{
  az_iot_su_crypto_hooks hooks = az_iot_su_crypto_openssl_hooks();
  return su_crypto_contract_run("su_crypto_openssl", &hooks);
}
