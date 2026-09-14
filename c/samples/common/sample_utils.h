#ifndef SAMPLE_UTILS_H
#define SAMPLE_UTILS_H

#include <stdint.h>

typedef struct sample_config
{
  char* id_scope;
  char* reg_id;
  char* cert;
  char* key;
  char* ca;
  char* device_id; /* AZ_IOT_DEVICE_ID (optional, for mock bypass) */
  char* mock_endpoint; /* AZ_IOT_HUB_NEXT_MOCK_ENDPOINT (optional) */
} sample_config;

// Reads DPS configuration from environment variables.
// Returns 0 on success, non-zero if any required variable is missing.
int sample_config_load(sample_config* config);

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

#endif // SAMPLE_UTILS_H
