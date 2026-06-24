# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

# ---------------------------------------------------------------------------
# az_iot_e2e_agent
#
# Device-side driver for the end-to-end test suite. A small CLI built on the
# public SDK + Paho adapter that performs ONE scriptable IoT scenario against a
# real Azure IoT Hub / DPS instance and exits 0 on success. The service-side
# half of every scenario (and all assertions) lives in the dotnet harness under
# e2e/driver, which launches this executable.
#
# This is intentionally NOT registered with add_test(): it is not a standalone
# CTest case. The dotnet harness owns orchestration so it can reuse the
# Microsoft.Azure.Devices service client for verification.
# ---------------------------------------------------------------------------
add_executable(az_iot_e2e_agent agent/e2e_agent.c)
target_link_libraries(az_iot_e2e_agent PRIVATE az_iot_core az_iot_adapter_paho)
az_iot_apply_warnings(az_iot_e2e_agent)

# Pin the output name/location so the harness can locate it deterministically
# across single- and multi-config generators.
set_target_properties(az_iot_e2e_agent PROPERTIES OUTPUT_NAME az_iot_e2e_agent)
