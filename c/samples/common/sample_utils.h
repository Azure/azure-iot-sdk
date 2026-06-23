#ifndef SAMPLE_UTILS_H
#define SAMPLE_UTILS_H

typedef struct sample_config
{
    char* id_scope;
    char* reg_id;
    char* cert;
    char* key;
    char* ca;
    char* device_id;         /* AZ_IOT_DEVICE_ID (optional, for mock bypass) */
    char* mock_endpoint;     /* AZ_IOT_HUB_NEXT_MOCK_ENDPOINT (optional) */
} sample_config_t;

// Reads DPS configuration from environment variables.
// Returns 0 on success, non-zero if any required variable is missing.
int sample_config_load(sample_config_t* config);

// Releases memory allocated by sample_config_load (Windows only; no-op on Linux).
void sample_config_release(sample_config_t* config);

#endif // SAMPLE_UTILS_H
