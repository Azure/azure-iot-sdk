#include "sample_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char* read_env_var(const char* name)
{
    char* value;

#ifdef _WIN32
    size_t value_size;
    int read_result = _dupenv_s(&value, &value_size, name);

    if (read_result != 0 || !value || !value[0])
    {
        fprintf(stderr, "Required env var %s not set.\n", name);
        return NULL;
    }
#else
    value = getenv(name);

    if (!value || !value[0])
    {
        fprintf(stderr, "Required env var %s not set.\n", name);
        return NULL;
    }
#endif

    return value;
}

static char* read_env_var_optional(const char* name)
{
    char* value;
#ifdef _WIN32
    size_t value_size;
    int read_result = _dupenv_s(&value, &value_size, name);
    if (read_result != 0 || !value || !value[0])
    {
        free(value);
        return NULL;
    }
#else
    value = getenv(name);
    if (!value || !value[0])
    {
        return NULL;
    }
#endif
    return value;
}

int sample_config_load(sample_config_t* config)
{
    memset(config, 0, sizeof(*config));

    /* Optional: Hub-Next mock bypass. When set, DPS vars are still loaded
     * but provisioning is skipped internally by the connection client. */
    config->mock_endpoint = read_env_var_optional("AZ_IOT_HUB_NEXT_MOCK_ENDPOINT");
    config->device_id     = read_env_var_optional("AZ_IOT_DEVICE_ID");

    config->id_scope = read_env_var("AZ_IOT_DPS_ID_SCOPE");
    config->reg_id   = read_env_var("AZ_IOT_DPS_REGISTRATION_ID");
    config->cert     = read_env_var("AZ_IOT_CLIENT_CERT");
    config->key      = read_env_var("AZ_IOT_CLIENT_KEY");
    config->ca       = read_env_var("AZ_IOT_TRUSTED_CA");

    if (!config->id_scope || !config->reg_id || !config->cert || !config->key || !config->ca)
    {
        return 1;
    }

    return 0;
}

void sample_config_release(sample_config_t* config)
{
#ifdef _WIN32
    free(config->id_scope);
    free(config->reg_id);
    free(config->cert);
    free(config->key);
    free(config->ca);
    free(config->device_id);
    free(config->mock_endpoint);
#endif
    memset(config, 0, sizeof(*config));
}
