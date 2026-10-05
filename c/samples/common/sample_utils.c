/* Must precede every system header: glibc gates clock_gettime and
 * CLOCK_MONOTONIC on it, and the first header included fixes the choice. */
#ifndef _WIN32
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif
#endif

#include "sample_utils.h"

#include "azure/iot/az_iot.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#endif

// Returns a heap copy of `s` (NUL-terminated), or NULL when `s` is NULL or on
// allocation failure. Caller frees with free().
static char* dup_cstr(const char* s)
{
  if (!s)
  {
    return NULL;
  }
  size_t n = strlen(s) + 1;
  char* out = (char*)malloc(n);
  if (out)
  {
    memcpy(out, s, n);
  }
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

  /* Optional: MQTTv5 mock bypass. When set, DPS vars are still loaded
   * but provisioning is skipped internally by the connection client. */
  config->mock_endpoint = read_env_var_optional("AZ_IOT_HUB_MQTT_V5_MOCK_ENDPOINT");
  config->device_id = read_env_var_optional("AZ_IOT_DEVICE_ID");

  config->id_scope = read_env_var("AZ_IOT_DPS_ID_SCOPE");
  config->reg_id = read_env_var("AZ_IOT_DPS_REGISTRATION_ID");
  config->cert = read_env_var("AZ_IOT_CLIENT_CERT");
  config->key = read_env_var("AZ_IOT_CLIENT_KEY");
  config->ca = read_env_var("AZ_IOT_TRUSTED_CA");

  /* Optional: a provisioning endpoint other than the global one. */
  config->dps_global_endpoint = read_env_var_optional("AZ_IOT_DPS_GLOBAL_ENDPOINT");

  if (!config->id_scope || !config->reg_id || !config->cert || !config->key || !config->ca)
  {
    /* Release what did load. Without this the strings already read leak on
     * Windows, where read_env_var() allocates -- the callers all return
     * immediately on failure rather than releasing a config they were told was
     * unusable. */
    sample_config_release(config);
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
  free(config->dps_global_endpoint);
#endif
  memset(config, 0, sizeof(*config));
}

void sample_apply_dps_options(
    struct az_iot_connection_client_options* options,
    const sample_config* config)
{
  if (options == NULL || config == NULL)
  {
    return;
  }
  options->dps.id_scope = config->id_scope;
  options->dps.registration_id = config->reg_id;
  /* Left NULL when unset, which is what selects the SDK's global endpoint. */
  if (config->dps_global_endpoint != NULL)
  {
    options->dps.global_endpoint = config->dps_global_endpoint;
  }
}

az_iot_log_sink sample_log_sink(az_iot_log_level min_level)
{
  /* Static: the sink writes through this state for the life of the process. */
  static az_iot_log_file_sink s_file_sink;
  az_iot_log_sink sink;
  char* enabled = sample_env_dup("AZ_IOT_SAMPLE_LOG_TO_FILE", NULL);
  if (enabled == NULL)
  {
    return az_iot_log_stderr_sink(min_level);
  }
  free(enabled);
  az_iot_result r = az_iot_log_file_sink_open(
      &s_file_sink, AZ_IOT_LOG_FILE_DEFAULT_NAME, NULL, min_level, &sink);
  if (r != AZ_IOT_OK)
  {
    fprintf(
        stderr,
        "AZ_IOT_SAMPLE_LOG_TO_FILE: cannot open '%s' (%s); logging to stderr\n",
        AZ_IOT_LOG_FILE_DEFAULT_NAME,
        az_iot_result_to_string(r));
    sink = az_iot_log_stderr_sink(min_level);
  }
  return sink;
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
  {
    return dup_cstr(v);
  }
  return dup_cstr(fallback);
#endif
}

bool sample_copy_str(char* dst, size_t dst_size, const char* src)
{
  size_t n = strlen(src);
  if (n >= dst_size)
  {
    return false;
  }
  memcpy(dst, src, n + 1);
  return true;
}

bool sample_env_to_buffer(const char* name, const char* fallback, char* dst, size_t dst_size)
{
  char* v = sample_env_dup(name, fallback);
  bool ok = (v != NULL) && sample_copy_str(dst, dst_size, v);
  if (v != NULL && !ok)
  {
    fprintf(stderr, "%s is too long (max %zu characters).\n", name, dst_size - 1);
  }
  free(v);
  return ok;
}

FILE* sample_fopen_private(const char* path)
{
  FILE* f = NULL;
  int fd;
  if (!path)
  {
    return NULL;
  }
#ifdef _WIN32
  if (_sopen_s(
          &fd, path, _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY, _SH_DENYWR, _S_IREAD | _S_IWRITE)
      != 0)
  {
    return NULL;
  }
  f = _fdopen(fd, "wb");
  if (!f)
  {
    (void)_close(fd);
  }
#else
  fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
  if (fd < 0)
  {
    return NULL;
  }
  /* open() keeps the mode of a file that already exists. */
  if (fchmod(fd, S_IRUSR | S_IWUSR) == 0)
  {
    f = fdopen(fd, "wb");
  }
  if (!f)
  {
    (void)close(fd);
  }
#endif
  return f;
}

uint64_t sample_now_ms(void)
{
#ifdef _WIN32
  return (uint64_t)GetTickCount64();
#else
  struct timespec ts;
  /* CLOCK_MONOTONIC rather than time(): a step from NTP must not shorten or
   * extend a deadline measured against it. */
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
  {
    return 0;
  }
  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000L);
#endif
}

void sample_sleep_ms(long ms)
{
  if (ms <= 0)
  {
    return;
  }
#if defined(_WIN32)
  Sleep((DWORD)ms);
#else
  struct timespec ts;
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (ms % 1000) * 1000000L;
  (void)nanosleep(&ts, NULL);
#endif
}

const char* sample_connection_state_name(az_iot_connection_state state)
{
  switch (state)
  {
    case AZ_IOT_CONN_STATE_IDLE:
      return "Idle";
    case AZ_IOT_CONN_STATE_CONNECTING:
      return "Connecting";
    case AZ_IOT_CONN_STATE_CONNECTED:
      return "Connected";
    case AZ_IOT_CONN_STATE_RETRY_PENDING:
      return "Retry pending";
    case AZ_IOT_CONN_STATE_DISCONNECTING:
      return "Disconnecting";
    case AZ_IOT_CONN_STATE_FAULTED:
      return "Faulted";
    case AZ_IOT_CONN_STATE_SETTING_UP:
      return "Setting up";
    default:
      return "?";
  }
}

const char* sample_connection_profile_name(az_iot_connection_profile profile)
{
  switch (profile)
  {
    case AZ_IOT_CONNECTION_PROFILE_MQTT_V3:
      return "MQTTv3 (MQTT v3.1.1)";
    case AZ_IOT_CONNECTION_PROFILE_MQTT_V5:
      return "MQTTv5 (MQTT v5)";
    case AZ_IOT_CONNECTION_PROFILE_UNKNOWN:
    default:
      return "not known to this SDK";
  }
}

az_iot_result sample_get_hub_profile(
    const az_iot_connection_client* client,
    az_iot_connection_profile* out_profile)
{
  az_iot_hub_profile profile;

  az_iot_result result = az_iot_connection_client_get_hub_profile(client, &profile);
  if (result != AZ_IOT_OK)
  {
    printf("hub profile not available: %s\n", az_iot_result_to_string(result));
    return result;
  }

  printf(
      "hub profile: %s, service sent \"%s\"%s\n",
      sample_connection_profile_name(profile.connection_profile),
      profile.connection_profile_raw ? profile.connection_profile_raw : "(none)",
      profile.connection_profile_raw_truncated ? " (truncated)" : "");

  *out_profile = profile.connection_profile;
  return AZ_IOT_OK;
}

bool sample_event_is_profile_mismatch(
    const az_iot_connection_state_event* event,
    az_iot_connection_profile* out_profile)
{
  if (event == NULL || event->state != AZ_IOT_CONN_STATE_FAULTED
      || event->reason != AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH || event->profile == NULL)
  {
    return false;
  }
  *out_profile = event->profile->connection_profile;
  return true;
}

az_iot_connection_profile sample_initial_profile(const sample_config* config)
{
  /* The mock bypass skips DPS, so no mismatch would ever correct a guess. */
  return (config != NULL && config->mock_endpoint != NULL) ? AZ_IOT_CONNECTION_PROFILE_MQTT_V5
                                                           : AZ_IOT_CONNECTION_PROFILE_MQTT_V3;
}

void sample_report_unsupported_profile(const az_iot_connection_state_event* event)
{
  if (event == NULL || event->reason != AZ_IOT_ERR_CONNECTION_PROFILE_UNSUPPORTED
      || event->profile == NULL)
  {
    return;
  }
  printf(
      "Unsupported hub generation \"%s\"%s. Upgrade the SDK.\n",
      event->profile->connection_profile_raw ? event->profile->connection_profile_raw : "(none)",
      event->profile->connection_profile_raw_truncated ? " (truncated)" : "");
}
