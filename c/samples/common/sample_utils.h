#ifndef SAMPLE_UTILS_H
#define SAMPLE_UTILS_H

#include <stdbool.h>
#include <stdint.h>

#include "azure/iot/az_iot_connection_client.h"

typedef struct sample_config
{
  char* id_scope;
  char* reg_id;
  char* cert;
  char* key;
  char* ca;
  char* device_id; /* AZ_IOT_DEVICE_ID (optional, for mock bypass) */
  char* mock_endpoint; /* AZ_IOT_HUB_NEXT_MOCK_ENDPOINT (optional) */
  /* AZ_IOT_DPS_GLOBAL_ENDPOINT (optional). NULL selects the SDK default,
   * global.azure-devices-provisioning.net. Set it to run a sample against a
   * regional, private-link or sovereign-cloud provisioning endpoint, which
   * otherwise required editing the sample. */
  char* dps_global_endpoint;
} sample_config;

// Reads DPS configuration from environment variables.
// Returns 0 on success, non-zero if any required variable is missing.
int sample_config_load(sample_config* config);

// Applies the DPS options every sample sets the same way: id_scope,
// registration_id, and the optional global endpoint. Call it instead of
// assigning those fields by hand, so a sample picks up a new DPS option
// without being edited.
//
// Deliberately does NOT touch certificate_provider or anything else: a sample
// that needs different auth still says so itself, and this stays the one thing
// every DPS sample shares.
struct az_iot_connection_client_options;
void sample_apply_dps_options(
    struct az_iot_connection_client_options* options,
    const sample_config* config);

// Releases memory allocated by sample_config_load (Windows only; no-op on Linux).
void sample_config_release(sample_config* config);

// Returns a heap copy of environment variable `name`, or a heap copy of
// `fallback` when the variable is unset/empty (fallback may be NULL). Caller
// frees with free(). Avoids getenv()/strdup() to stay clean under MSVC /WX.
char* sample_env_dup(const char* name, const char* fallback);

// Milliseconds from an unspecified origin, never moving backwards. Only
// differences are meaningful. The SDK keeps its clock internal, so a sample
// that has to measure elapsed time brings its own.
uint64_t sample_now_ms(void);

// Blocks the calling thread for `ms` milliseconds; returns immediately for
// ms <= 0. Portable across Windows and POSIX, which every polling sample needs
// for its do_work loop.
void sample_sleep_ms(long ms);

// Human-readable name for a connection state, for logging. Never NULL.
const char* sample_connection_state_name(az_iot_connection_state state);

/** @brief Human-readable name for a connection profile, for logging. Never NULL. */
const char* sample_connection_profile_name(az_iot_connection_profile profile);

/**
 * @brief Reads and prints the hub generation a CONNECTED client reached.
 *
 * @param[in] client Client whose hub lifecycle is CONNECTED.
 * @param[out] out_profile Resolved profile; written only on AZ_IOT_OK.
 * @return The az_iot_connection_client_get_hub_profile() result.
 */
az_iot_result sample_get_hub_profile(
    const az_iot_connection_client* client,
    az_iot_connection_profile* out_profile);

/**
 * @brief Whether @p event reports that DPS assigned the device to the other hub
 * generation than the attached feature clients were built for.
 *
 * The connection is then stopped (AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH): the
 * application destroys those clients, builds the ones for @p out_profile, and
 * calls az_iot_connection_client_close() then az_iot_connection_client_open().
 * This happens on the first connect and whenever a running device is moved to a
 * hub of the other generation.
 *
 * @param[in] event Connection state event.
 * @param[out] out_profile Assigned profile; written only when true is returned.
 * @return true on a profile mismatch, false otherwise.
 */
bool sample_event_is_profile_mismatch(
    const az_iot_connection_state_event* event,
    az_iot_connection_profile* out_profile);

#endif // SAMPLE_UTILS_H
