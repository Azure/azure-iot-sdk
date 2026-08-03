#include "sample_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Returns a heap copy of `s` (NUL-terminated), or NULL when `s` is NULL or on
// allocation failure. Caller frees with free().
static char* dup_cstr(const char* s)
{
  if (!s)
    return NULL;
  size_t n = strlen(s) + 1;
  char* out = (char*)malloc(n);
  if (out)
    memcpy(out, s, n);
  return out;
}

// Reads a REQUIRED environment variable, logging to stderr and returning NULL
// when it is unset/empty. On Windows the returned string is CRT-heap-owned
// (_dupenv_s) and freed by sample_config_release; on POSIX it points into the
// process environment and must not be freed.
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

// Reads an OPTIONAL environment variable, returning NULL (no logging) when it
// is unset/empty. Same platform ownership rules as read_env_var().
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

int sample_config_load(sample_config* config)
{
  memset(config, 0, sizeof(*config));

  /* Optional: Hub-Next mock bypass. When set, DPS vars are still loaded
   * but provisioning is skipped internally by the connection client. */
  config->mock_endpoint = read_env_var_optional("AZ_IOT_HUB_NEXT_MOCK_ENDPOINT");
  config->device_id = read_env_var_optional("AZ_IOT_DEVICE_ID");

  config->id_scope = read_env_var("AZ_IOT_DPS_ID_SCOPE");
  config->reg_id = read_env_var("AZ_IOT_DPS_REGISTRATION_ID");
  config->cert = read_env_var("AZ_IOT_CLIENT_CERT");
  config->key = read_env_var("AZ_IOT_CLIENT_KEY");
  config->ca = read_env_var("AZ_IOT_TRUSTED_CA");

  if (!config->id_scope || !config->reg_id || !config->cert || !config->key || !config->ca)
  {
    return 1;
  }

  return 0;
}

void sample_config_release(sample_config* config)
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

char* sample_env_dup(const char* name, const char* fallback)
{
#ifdef _WIN32
  char* value = NULL;
  size_t value_size = 0;
  if (_dupenv_s(&value, &value_size, name) == 0 && value && value[0])
  {
    return value; /* heap-owned by the CRT; caller frees */
  }
  free(value);
  return dup_cstr(fallback);
#else
  const char* v = getenv(name);
  if (v && v[0])
    return dup_cstr(v);
  return dup_cstr(fallback);
#endif
}
