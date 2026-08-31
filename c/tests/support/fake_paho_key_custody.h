// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Test double for the Paho adapter's non-extractable key custody module (D8).
 *
 * The real module needs a PKCS#11 token to reach a SUCCESSFUL prepare(), and a
 * successful prepare() is the precondition for everything the connect-cleanup
 * suite asserts: only then is there a reference file on disk that a failed
 * connect could leave behind. This double reproduces the one property the
 * adapter depends on -- prepare() creates a file, release() removes it -- so
 * the adapter's cleanup contract can be tested on any machine.
 *
 * It is linked INSTEAD OF az_iot_paho_key_custody.c, so it must define the
 * whole of that translation unit's public surface. az_iot_paho_key_custody.c's
 * own behaviour is covered by paho_key_custody_test.c. */
#ifndef AZ_IOT_TEST_FAKE_PAHO_KEY_CUSTODY_H
#define AZ_IOT_TEST_FAKE_PAHO_KEY_CUSTODY_H

#include <stdbool.h>

#include "azure/iot/az_iot_result.h"

/* Forget the recorded calls and delete any reference file still around. */
void fake_custody_reset(void);

/* What the next prepare() returns. AZ_IOT_OK (the reset default) makes it
 * create a reference file, like the real one does for a reachable token. */
void fake_custody_set_prepare_result(az_iot_result rc);

/* The reference file the last successful prepare() created. Remembered across
 * release() precisely so a test can assert the file is gone. NULL if prepare()
 * has not succeeded yet. */
const char* fake_custody_reference_path(void);

/* Does that file exist right now? False when no prepare() has succeeded. */
bool fake_custody_reference_file_exists(void);

int fake_custody_prepare_calls(void);
int fake_custody_release_calls(void);

#endif /* AZ_IOT_TEST_FAKE_PAHO_KEY_CUSTODY_H */
