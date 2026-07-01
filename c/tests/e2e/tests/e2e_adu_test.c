// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/* SPDX-License-Identifier: MIT */
/* Device Update (ADU) end-to-end placeholder.
 *
 * ADU is intentionally a SEPARATE, slow-lane pipeline: provisioning the Azure
 * Device Update account/instance takes ~25 minutes, so it is excluded from the
 * fast PR job and runs nightly, on manual dispatch, or when a PR touches ADU
 * code paths (see .github/workflows/ci-c-e2e-adu.yml).
 *
 * The real device-side ADU update scenario is not implemented yet. This
 * placeholder keeps the ADU pipeline meaningful by gating on the provisioned
 * environment (the config the ADU setup job emits must be present) and then
 * reporting a CTest SKIP (exit code 77) to signal "infrastructure only".
 * Replace the skip below with a real ADU update round-trip (the device applies
 * an update; the service side drives/verifies it via 'az iot du') when it lands.
 *
 * This is deliberately standalone (no cmocka, no device/service link): the fast
 * suite (az_iot_tests_e2e) already covers the SDK data path. This target exists
 * only to carry the ADU pipeline until the scenario is written.
 */
#include <stdio.h>
#include <stdlib.h>

/* CTest treats this exit code as "skipped" (see SKIP_RETURN_CODE in CMake). */
#define E2E_ADU_SKIP 77

/* Portable, warning-clean check that an environment variable is set/non-empty. */
static int env_is_set(const char* name)
{
#ifdef _WIN32
    char* value = NULL;
    size_t len = 0;
    int set = (_dupenv_s(&value, &len, name) == 0 && value != NULL && value[0] != '\0');
    free(value);
    return set;
#else
    const char* value = getenv(name);
    return (value != NULL && value[0] != '\0');
#endif
}

int main(void)
{
    /* Gate: the ADU setup job must have provisioned a usable IoT Hub and emitted
     * its connection string. If it is missing the pipeline is misconfigured, so
     * fail loudly rather than silently pass. */
    if (!env_is_set("IOTHUB_CONNECTION_STRING"))
    {
        fprintf(stderr,
            "[e2e-adu] IOTHUB_CONNECTION_STRING is not set; the ADU e2e "
            "environment was not provisioned.\n");
        return EXIT_FAILURE;
    }

    fprintf(stderr,
        "[e2e-adu] ADU update scenario not implemented yet (infrastructure "
        "only); reporting CTest skip.\n");
    return E2E_ADU_SKIP;
}
