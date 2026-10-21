// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* software_update/pc/http_download - Software Update sample with a real download.
 *
 * Portable PC sample. Shows the download side of a deployment, which the
 * application owns: download_fn fetches each file over HTTPS with libcurl, a
 * piece per do_work() call, through the same HTTP proxy the connection uses;
 * read_file_fn streams it back so the SDK checks its SHA-256 against the
 * signed manifest; install_fn works in chunks too. Install and apply are
 * simulated: nothing on the device is changed.
 *
 * The device asks on the regular route (az_iot_su_client_request_update()), so
 * it registers and connects to its IoT Hub, as ../simulated_regular does. It
 * checks at startup, again after each applied update, and every
 * AZ_IOT_SU_POLL_INTERVAL_S.
 *
 * Environment variables (all optional unless noted):
 *   AZ_IOT_DPS_ID_SCOPE, AZ_IOT_DPS_REGISTRATION_ID, AZ_IOT_CLIENT_CERT,
 *   AZ_IOT_CLIENT_KEY, AZ_IOT_TRUSTED_CA (required) and
 *   AZ_IOT_DPS_GLOBAL_ENDPOINT     the device; see the README.
 *   AZ_IOT_SU_MANUFACTURER, AZ_IOT_SU_MODEL   compatibility (Contoso/SU-Sim)
 *   AZ_IOT_SU_INSTALLED_PROVIDER, _NAME, _VERSION   installed now (Contoso/SU-Sim/1.0.0)
 *   AZ_IOT_PROXY_HOST, AZ_IOT_PROXY_PORT, AZ_IOT_PROXY_USERNAME,
 *   AZ_IOT_PROXY_PASSWORD          HTTP proxy for MQTT and the download
 *   SAMPLE_DOWNLOAD_PREFIX         path prefix of downloaded files
 *                                  (default ./su_download_)
 *   SAMPLE_INSTALL_CHUNKS          install_fn calls per step (default 4)
 *   AZ_IOT_SU_POLL_INTERVAL_S      seconds between checks (default 14400)
 *   AZ_IOT_SU_LOG_LEVEL            trace|debug|info|warn|error|off
 */
#include <limits.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>

#include "azure/iot/az_iot.h"
#include "azure/iot/az_iot_su.h"
/* The MQTT adapter this build of the sample uses (see samples/CMakeLists.txt). */
#if defined(AZ_IOT_SAMPLE_ADAPTER_PAHO)
#include "azure/iot/adapters/az_iot_adapter_paho.h"
#elif defined(AZ_IOT_SAMPLE_ADAPTER_AZ_MQTT)
#include "azure/iot/adapters/az_iot_adapter_az_mqtt.h"
#else
#error "Define AZ_IOT_SAMPLE_ADAPTER_PAHO or AZ_IOT_SAMPLE_ADAPTER_AZ_MQTT"
#endif
/* The crypto backend this build of the sample uses (see samples/CMakeLists.txt). */
#if defined(AZ_IOT_SAMPLE_CRYPTO_OPENSSL)
#include "az_iot_crypto_openssl.h"
#elif defined(AZ_IOT_SAMPLE_CRYPTO_MBEDTLS)
#include "az_iot_crypto_mbedtls.h"
#else
#error "Define AZ_IOT_SAMPLE_CRYPTO_OPENSSL or AZ_IOT_SAMPLE_CRYPTO_MBEDTLS"
#endif

#include "sample_utils.h"
#include "su_sim.h"

/** @brief Device-specific failure sub-codes. Kept at 0x100 and above so they
 * never read as an AZ_IOT_SU_RESULT_* value; they end up in the low bits of the
 * step's extendedResultCodes. */
#define SAMPLE_ERR_FILE_OPEN 0x101
#define SAMPLE_ERR_URL_TOO_LONG 0x102
#define SAMPLE_ERR_CURL 0x200 /* + CURLcode */
#define SAMPLE_ERR_HTTP 0x1000 /* + HTTP status */

/** @brief A transfer slower than 1 byte/s for this long fails (CURLE_OPERATION_TIMEDOUT). */
#define SAMPLE_DOWNLOAD_STALL_S 60L

static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int signo)
{
  (void)signo;
  g_stop = 1;
}

/** @brief What the hooks share; passed to them as user_ctx. */
typedef struct
{
  char* path_prefix; /* where downloaded files go; owned */
  char* proxy_url; /* "http://host:port" for libcurl, or NULL; owned */
  const char* proxy_username;
  const char* proxy_password;
  long install_chunks;

  /* The download in progress, one file at a time. */
  CURLM* multi;
  CURL* easy;
  FILE* file;
  char path[512];
  curl_off_t last_reported;

  /* The file the hash check is reading, kept open across sequential reads. */
  FILE* reader;
  char reader_path[512];
  size_t reader_offset;

  /* Install in progress: chunks done for the current step. */
  long install_done;

  /* What is installed now, and what apply just applied. */
  char installed_provider[SU_SIM_ID_PART_SIZE];
  char installed_name[SU_SIM_ID_PART_SIZE];
  char installed_version[SU_SIM_ID_PART_SIZE];
  int applied;
} download_context;

/** @brief Path of a downloaded file: the prefix and the manifest's file id. */
static void file_path(
    const download_context* d,
    const az_iot_su_client_update_manifest_file* file,
    char* out,
    size_t out_size)
{
  (void)snprintf(
      out,
      out_size,
      "%s%.*s",
      d->path_prefix,
      (int)az_span_size(file->id),
      (const char*)az_span_ptr(file->id));
}

/** @brief Close the hash check's reader, if open. */
static void reader_close(download_context* d)
{
  if (d->reader != NULL)
  {
    (void)fclose(d->reader);
    d->reader = NULL;
  }
}

/** @brief CURLOPT_WRITEFUNCTION: writes with this program's C runtime, not libcurl's. */
static size_t download_write(char* data, size_t size, size_t count, void* user_data)
{
  return fwrite(data, 1, size * count, (FILE*)user_data);
}

/** @brief Stop the transfer in progress and close its file. */
static void download_reset(download_context* d)
{
  if (d->easy != NULL)
  {
    (void)curl_multi_remove_handle(d->multi, d->easy);
    curl_easy_cleanup(d->easy);
    d->easy = NULL;
  }
  if (d->file != NULL)
  {
    (void)fclose(d->file);
    d->file = NULL;
  }
}

/**
 * @brief download_fn: fetch one file over HTTPS, a piece per call.
 *
 * Returns AZ_IOT_SU_RESULT_IN_PROGRESS until the transfer ends, so do_work()
 * never blocks on the network. The SDK then checks the file's SHA-256 through
 * download_read_file().
 */
static int32_t download_file(
    const az_iot_su_client_update_manifest_file* file,
    az_span url,
    uint32_t file_index,
    uint32_t file_count,
    void* user_ctx)
{
  download_context* d = (download_context*)user_ctx;

  if (d->easy == NULL)
  {
    char url_text[2048];
    if (az_span_size(url) <= 0 || (size_t)az_span_size(url) >= sizeof(url_text))
    {
      return SAMPLE_ERR_URL_TOO_LONG;
    }
    memcpy(url_text, az_span_ptr(url), (size_t)az_span_size(url));
    url_text[az_span_size(url)] = '\0';

    file_path(d, file, d->path, sizeof(d->path));
    reader_close(d);
    d->file = sample_fopen_private(d->path);
    if (d->file == NULL)
    {
      return SAMPLE_ERR_FILE_OPEN;
    }
    d->easy = curl_easy_init();
    if (d->easy == NULL)
    {
      download_reset(d);
      return SAMPLE_ERR_CURL;
    }
    (void)curl_easy_setopt(d->easy, CURLOPT_URL, url_text);
    (void)curl_easy_setopt(d->easy, CURLOPT_WRITEFUNCTION, download_write);
    (void)curl_easy_setopt(d->easy, CURLOPT_WRITEDATA, d->file);
    (void)curl_easy_setopt(d->easy, CURLOPT_FOLLOWLOCATION, 1L);
    (void)curl_easy_setopt(d->easy, CURLOPT_FAILONERROR, 1L);
    /* A stalled transfer fails rather than holding the workflow forever. */
    (void)curl_easy_setopt(d->easy, CURLOPT_LOW_SPEED_LIMIT, 1L);
    (void)curl_easy_setopt(d->easy, CURLOPT_LOW_SPEED_TIME, SAMPLE_DOWNLOAD_STALL_S);
    (void)curl_easy_setopt(d->easy, CURLOPT_CONNECTTIMEOUT, SAMPLE_DOWNLOAD_STALL_S);
    /* The download takes the same route as the MQTT sessions: the proxy set, or
     * none ("" disables it), whatever the proxy environment variables say. */
    (void)curl_easy_setopt(d->easy, CURLOPT_PROXY, d->proxy_url != NULL ? d->proxy_url : "");
    (void)curl_easy_setopt(d->easy, CURLOPT_NOPROXY, "");
    if (d->proxy_url != NULL)
    {
      if (d->proxy_username != NULL)
      {
        (void)curl_easy_setopt(d->easy, CURLOPT_PROXYUSERNAME, d->proxy_username);
        (void)curl_easy_setopt(
            d->easy, CURLOPT_PROXYPASSWORD, d->proxy_password != NULL ? d->proxy_password : "");
      }
    }
    if (curl_multi_add_handle(d->multi, d->easy) != CURLM_OK)
    {
      download_reset(d);
      return SAMPLE_ERR_CURL;
    }
    d->last_reported = 0;
    printf("Downloading file %u of %u to %s\n", file_index + 1, file_count, d->path);
  }

  int running = 0;
  if (curl_multi_perform(d->multi, &running) != CURLM_OK)
  {
    download_reset(d);
    return SAMPLE_ERR_CURL;
  }

  curl_off_t got = 0;
  (void)curl_easy_getinfo(d->easy, CURLINFO_SIZE_DOWNLOAD_T, &got);
  if (got - d->last_reported >= (curl_off_t)1024 * 1024)
  {
    printf("  %lld of %lld bytes\n", (long long)got, (long long)file->size_in_bytes);
    d->last_reported = got;
  }
  if (running > 0)
  {
    return AZ_IOT_SU_RESULT_IN_PROGRESS;
  }

  int32_t rc = AZ_IOT_SU_RESULT_SUCCESS;
  int pending = 0;
  CURLMsg* msg = curl_multi_info_read(d->multi, &pending);
  if (msg == NULL || msg->msg != CURLMSG_DONE || msg->data.result != CURLE_OK)
  {
    long http_status = 0;
    (void)curl_easy_getinfo(d->easy, CURLINFO_RESPONSE_CODE, &http_status);
    CURLcode cr = (msg != NULL) ? msg->data.result : CURLE_FAILED_INIT;
    fprintf(stderr, "Download failed: %s (HTTP %ld)\n", curl_easy_strerror(cr), http_status);
    rc = (http_status >= 400) ? (int32_t)(SAMPLE_ERR_HTTP + http_status)
                              : (int32_t)(SAMPLE_ERR_CURL + (int32_t)cr);
  }
  download_reset(d);
  return rc;
}

/** @brief read_file_fn: stream a downloaded file back for the SHA-256 check. */
static int32_t download_read_file(
    const az_iot_su_client_update_manifest_file* file,
    uint32_t file_index,
    size_t offset,
    uint8_t* buffer,
    size_t buffer_size,
    size_t* out_read,
    void* user_ctx)
{
  (void)file_index;
  download_context* d = (download_context*)user_ctx;
  char path[512];
  file_path(d, file, path, sizeof(path));
  /* The SDK reads sequentially: reopen only for another file or offset. */
  if (d->reader != NULL && (strcmp(path, d->reader_path) != 0 || offset != d->reader_offset))
  {
    reader_close(d);
  }
  if (d->reader == NULL)
  {
    d->reader = fopen(path, "rb");
    if (d->reader == NULL)
    {
      return SAMPLE_ERR_FILE_OPEN;
    }
    (void)snprintf(d->reader_path, sizeof(d->reader_path), "%s", path);
    if (offset > (size_t)LONG_MAX || fseek(d->reader, (long)offset, SEEK_SET) != 0)
    {
      reader_close(d);
      return SAMPLE_ERR_FILE_OPEN;
    }
  }
  *out_read = fread(buffer, 1, buffer_size, d->reader);
  if (*out_read < buffer_size)
  {
    int failed = ferror(d->reader);
    reader_close(d);
    if (failed)
    {
      return SAMPLE_ERR_FILE_OPEN;
    }
    return AZ_IOT_SU_RESULT_SUCCESS;
  }
  d->reader_offset = offset + *out_read;
  return AZ_IOT_SU_RESULT_SUCCESS;
}

/** @brief is_installed_fn: the offer is already installed when its id matches. */
static int32_t download_is_installed(
    const az_iot_su_client_update_manifest* manifest,
    void* user_ctx)
{
  download_context* d = (download_context*)user_ctx;
  return (az_span_is_content_equal(
              manifest->update_id.provider, az_span_create_from_str(d->installed_provider))
          && az_span_is_content_equal(
              manifest->update_id.name, az_span_create_from_str(d->installed_name))
          && az_span_is_content_equal(
              manifest->update_id.version, az_span_create_from_str(d->installed_version)))
      ? AZ_IOT_SU_RESULT_ALREADY_INSTALLED
      : AZ_IOT_SU_RESULT_SUCCESS;
}

/**
 * @brief install_fn: install the step in chunks, one per call.
 *
 * A real installer writes a piece of the image per call; returning
 * AZ_IOT_SU_RESULT_IN_PROGRESS gives do_work() back between pieces, so the
 * connection stays serviced. Simulated: nothing is written.
 */
static int32_t download_install(
    const az_iot_su_client_update_manifest* manifest,
    uint32_t step_index,
    void* user_ctx)
{
  (void)manifest;
  download_context* d = (download_context*)user_ctx;
  d->install_done++;
  printf("Installing step %u: chunk %ld of %ld\n", step_index, d->install_done, d->install_chunks);
  if (d->install_done < d->install_chunks)
  {
    return AZ_IOT_SU_RESULT_IN_PROGRESS;
  }
  d->install_done = 0;
  return AZ_IOT_SU_RESULT_SUCCESS;
}

/** @brief apply_fn: simulated; the last step records the update as installed. */
static int32_t download_apply(
    const az_iot_su_client_update_manifest* manifest,
    uint32_t step_index,
    void* user_ctx)
{
  download_context* d = (download_context*)user_ctx;
  printf("Applying step %u (simulated)\n", step_index);
  /* Later steps are downloaded and applied separately, and may still fail. */
  if (step_index + 1u < manifest->instructions.steps_count)
  {
    return AZ_IOT_SU_RESULT_SUCCESS;
  }
  (void)snprintf(
      d->installed_provider,
      sizeof(d->installed_provider),
      "%.*s",
      (int)az_span_size(manifest->update_id.provider),
      (const char*)az_span_ptr(manifest->update_id.provider));
  (void)snprintf(
      d->installed_name,
      sizeof(d->installed_name),
      "%.*s",
      (int)az_span_size(manifest->update_id.name),
      (const char*)az_span_ptr(manifest->update_id.name));
  (void)snprintf(
      d->installed_version,
      sizeof(d->installed_version),
      "%.*s",
      (int)az_span_size(manifest->update_id.version),
      (const char*)az_span_ptr(manifest->update_id.version));
  d->applied = 1;
  return AZ_IOT_SU_RESULT_SUCCESS;
}

/* ------------------------------------------------------------------------- */
/* observers                                                                 */
/* ------------------------------------------------------------------------- */

typedef struct
{
  int workflow_active;
  int workflow_succeeded;
  int connection_faulted;
} sample_flags;

static void on_connection_state(const az_iot_connection_state_event* event, void* user_ctx)
{
  sample_flags* flags = (sample_flags*)user_ctx;
  printf(
      "%s: %s (%s)\n",
      event->scope == AZ_IOT_CONN_SCOPE_DPS ? "Provisioning" : "Hub",
      sample_connection_state_name(event->state),
      az_iot_result_to_string(event->reason));
  if (event->state == AZ_IOT_CONN_STATE_FAULTED)
  {
    flags->connection_faulted = 1;
  }
}

static void on_su_event(const az_iot_su_event* event, void* user_ctx)
{
  sample_flags* flags = (sample_flags*)user_ctx;
  switch (event->kind)
  {
    case AZ_IOT_SU_EVENT_WORKFLOW_STATE_CHANGED:
      printf(
          "Update workflow: %s -> %s\n",
          su_sample_state_name(event->previous_state),
          su_sample_state_name(event->state));
      if (event->state == AZ_IOT_SU_STATE_FAILED)
      {
        fprintf(
            stderr,
            "Update failed: %s, extended result 0x%08x\n",
            az_iot_result_to_string(event->reason),
            (unsigned)event->extended_result_code);
      }
      flags->workflow_active = (event->state != AZ_IOT_SU_STATE_IDLE);
      if (event->state == AZ_IOT_SU_STATE_IDLE
          && event->previous_state == AZ_IOT_SU_STATE_APPLY_STARTED)
      {
        flags->workflow_succeeded = 1;
      }
      break;
    case AZ_IOT_SU_EVENT_OPERATION_ABANDONED:
      fprintf(
          stderr,
          "%s abandoned: %s (service code %d)\n",
          su_sample_operation_name(event->operation),
          az_iot_result_to_string(event->reason),
          (int)event->service_error.code);
      break;
    case AZ_IOT_SU_EVENT_NO_UPDATE:
      printf("No update available.\n");
      break;
    case AZ_IOT_SU_EVENT_REPORT_ACCEPTED:
      printf("Status report accepted%s.\n", event->report_terminal ? " (final)" : "");
      break;
    case AZ_IOT_SU_EVENT_PERSIST_FAILED:
    case AZ_IOT_SU_EVENT_PERSIST_RECOVERED:
    case AZ_IOT_SU_EVENT_UPDATE_REFUSED:
    case AZ_IOT_SU_EVENT_UPDATE_DEFERRED:
    case AZ_IOT_SU_EVENT_UPDATE_DUPLICATE:
      printf(
          "Software updates event %d: %s\n",
          (int)event->kind,
          az_iot_result_to_string(event->reason));
      break;
  }
}

/* The software updates client's memory: update copy, persisted-state record,
 * scratch, message body and device-property strings. Sized for the limits
 * main() sets; static, as it is too large for a stack frame. */
static uint8_t g_su_workspace[AZ_IOT_SU_WORKSPACE_DEFAULT_SIZE];

int main(void)
{
  az_iot_log_sink log = sample_log_sink(su_sample_log_level_from_env());
  az_iot_log_set_global_sink(&log);
  (void)signal(SIGINT, on_sigint);

  if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
  {
    return 1;
  }
  /* Without an asynchronous resolver, curl_multi_perform() blocks on DNS. */
  const curl_version_info_data* curl_info = curl_version_info(CURLVERSION_NOW);
  if (curl_info == NULL || (curl_info->features & CURL_VERSION_ASYNCHDNS) == 0)
  {
    fprintf(stderr, "libcurl without asynchronous DNS: the download would block do_work().\n");
    curl_global_cleanup();
    return 1;
  }

  int rc = 1;
  sample_config config = { 0 };
  sample_flags flags = { 0 };
  download_context dl = { 0 };
  az_iot_certificate_provider_pem certs = { 0 };
  az_iot_connection_client connection;
  az_iot_su_client su;
  az_iot_mqtt_factory* factory_v3 = NULL;
  az_iot_mqtt_factory* factory_v5 = NULL;
  int connection_ready = 0;
  int su_ready = 0;
  char* manufacturer = sample_env_dup("AZ_IOT_SU_MANUFACTURER", "Contoso");
  char* model = sample_env_dup("AZ_IOT_SU_MODEL", "SU-Sim");
  char* proxy_host = sample_env_dup("AZ_IOT_PROXY_HOST", NULL);
  char* proxy_port = sample_env_dup("AZ_IOT_PROXY_PORT", NULL);
  char* proxy_username = sample_env_dup("AZ_IOT_PROXY_USERNAME", NULL);
  char* proxy_password = sample_env_dup("AZ_IOT_PROXY_PASSWORD", NULL);

  dl.path_prefix = sample_env_dup("SAMPLE_DOWNLOAD_PREFIX", "./su_download_");
  dl.install_chunks = su_sample_env_long("SAMPLE_INSTALL_CHUNKS", 4);
  if (dl.install_chunks < 1)
  {
    dl.install_chunks = 1;
  }
  dl.multi = curl_multi_init();
  long poll_interval_s = su_sample_env_long("AZ_IOT_SU_POLL_INTERVAL_S", 14400);
  if (manufacturer == NULL || model == NULL || dl.path_prefix == NULL || dl.multi == NULL
      || poll_interval_s < 0
      || !sample_env_to_buffer(
          "AZ_IOT_SU_INSTALLED_PROVIDER",
          "Contoso",
          dl.installed_provider,
          sizeof(dl.installed_provider))
      || !sample_env_to_buffer(
          "AZ_IOT_SU_INSTALLED_NAME", "SU-Sim", dl.installed_name, sizeof(dl.installed_name))
      || !sample_env_to_buffer(
          "AZ_IOT_SU_INSTALLED_VERSION",
          "1.0.0",
          dl.installed_version,
          sizeof(dl.installed_version))
      || sample_config_load(&config) != 0)
  {
    goto cleanup;
  }

  /* One proxy for both the MQTT sessions and the download. */
  if (proxy_host != NULL)
  {
    size_t n = strlen(proxy_host) + 32;
    dl.proxy_url = (char*)malloc(n);
    if (dl.proxy_url == NULL)
    {
      goto cleanup;
    }
    (void)snprintf(
        dl.proxy_url, n, "http://%s:%s", proxy_host, proxy_port != NULL ? proxy_port : "8080");
    dl.proxy_username = proxy_username;
    dl.proxy_password = proxy_password;
  }

  /* Certificate provider and connection client. */
  az_iot_certificate_provider_pem_options pem = az_iot_certificate_provider_pem_options_default();
  pem.trusted_ca_pem_path = config.ca;
  pem.client_cert_pem_path = config.cert;
  pem.client_key_pem_path = config.key;
  if (az_iot_certificate_provider_pem_init(&certs, &pem) != AZ_IOT_OK)
  {
    goto cleanup;
  }
  az_iot_connection_client_options copts = az_iot_connection_client_options_default();
  copts.dps.id_scope = config.id_scope;
  copts.dps.registration_id = config.reg_id;
  copts.dps.global_endpoint = config.dps_global_endpoint; /* NULL: the global default */
  copts.certificate_provider = &certs.base;
#if defined(AZ_IOT_SAMPLE_CRYPTO_OPENSSL)
  copts.crypto = az_iot_crypto_openssl(); /* verifies manifests and file hashes */
#elif defined(AZ_IOT_SAMPLE_CRYPTO_MBEDTLS)
  copts.crypto = az_iot_crypto_mbedtls(); /* verifies manifests and file hashes */
#endif
  copts.proxy.host = proxy_host; /* NULL: no proxy */
  copts.proxy.port = (proxy_port != NULL) ? (uint16_t)strtoul(proxy_port, NULL, 10) : (uint16_t)0;
  copts.proxy.username = proxy_username;
  copts.proxy.password = proxy_password;
  if (az_iot_connection_client_init(&connection, &copts) != AZ_IOT_OK)
  {
    goto cleanup;
  }
  connection_ready = 1;
#if defined(AZ_IOT_SAMPLE_ADAPTER_PAHO)
  factory_v3 = az_iot_paho_factory_create_v3_1_1();
  factory_v5 = az_iot_paho_factory_create_v5();
#elif defined(AZ_IOT_SAMPLE_ADAPTER_AZ_MQTT)
  factory_v3 = az_iot_az_mqtt_factory_create_v3_1_1();
  factory_v5 = az_iot_az_mqtt_factory_create_v5();
#endif
  if (az_iot_connection_client_add_state_observer(&connection, on_connection_state, &flags)
          != AZ_IOT_OK
      || az_iot_connection_client_register_mqtt_factory(&connection, factory_v3) != AZ_IOT_OK
      || az_iot_connection_client_register_mqtt_factory(&connection, factory_v5) != AZ_IOT_OK)
  {
    goto cleanup;
  }

  /* Software updates client: the hooks this sample is about. */
  az_iot_su_platform_hooks hooks = { 0 };
  hooks.download_fn = download_file;
  hooks.read_file_fn = download_read_file;
  hooks.is_installed_fn = download_is_installed;
  hooks.install_fn = download_install;
  hooks.apply_fn = download_apply;
  hooks.user_ctx = &dl;

  az_iot_su_device_properties dp = { 0 };
  dp.manufacturer = manufacturer;
  dp.model = model;
  dp.installed_update_id.provider = dl.installed_provider;
  dp.installed_update_id.name = dl.installed_name;
  dp.installed_update_id.version = dl.installed_version;

  size_t root_key_count = 0;
  const az_iot_su_root_key* root_keys = az_iot_su_microsoft_root_keys(&root_key_count);
  az_iot_su_client_config_options su_opts = az_iot_su_client_config_options_default();
  su_opts.hooks = &hooks;
  su_opts.root_keys = root_keys;
  su_opts.root_key_count = root_key_count;
  su_opts.device_properties = &dp;
  /* Limits, each the SDK default; the workspace is sized for them. */
  su_opts.limits.max_update_size = AZ_IOT_SU_DEFAULT_MAX_UPDATE_SIZE;
  su_opts.limits.scratch_size = AZ_IOT_SU_VERIFY_SCRATCH_SIZE;
  su_opts.limits.max_message_size = AZ_IOT_SU_DEFAULT_MAX_MESSAGE_SIZE;
  su_opts.limits.device_properties_size = AZ_IOT_SU_DEVICE_PROPERTIES_BUFFER_SIZE;
  su_opts.workspace = g_su_workspace;
  su_opts.workspace_size = sizeof(g_su_workspace);
  if (az_iot_su_client_init(&su, &connection, &su_opts) != AZ_IOT_OK)
  {
    fprintf(stderr, "az_iot_su_client_init failed\n");
    goto cleanup;
  }
  su_ready = 1;
  if (az_iot_su_client_add_observer(&su, on_su_event, &flags) != AZ_IOT_OK
      || az_iot_su_client_request_update(&su, AZ_IOT_SU_REQUEST_DEFAULT_TIMEOUT_MS) != AZ_IOT_OK
      || az_iot_connection_client_open(&connection) != AZ_IOT_OK)
  {
    goto cleanup;
  }

  printf("Running (Ctrl-C to exit). Checking every %ld s.\n", poll_interval_s);
  uint64_t next_check_ms = sample_now_ms() + (uint64_t)poll_interval_s * 1000u;
  rc = 0;
  while (!g_stop)
  {
    (void)az_iot_connection_client_do_work(&connection, 20);
    (void)az_iot_su_client_do_work(&su);

    /* After an applied update, report it as installed and ask again at once. */
    int check_now = 0;
    if (flags.workflow_succeeded)
    {
      flags.workflow_succeeded = 0;
      if (dl.applied && az_iot_su_client_update_device_properties(&su, &dp) == AZ_IOT_OK)
      {
        printf(
            "Installed: %s/%s/%s\n",
            dl.installed_provider,
            dl.installed_name,
            dl.installed_version);
      }
      dl.applied = 0;
      check_now = 1;
    }
    if (flags.connection_faulted && !flags.workflow_active)
    {
      fprintf(stderr, "The connection faulted and will not recover on its own.\n");
      rc = 1;
      break;
    }
    if (!flags.workflow_active
        && (check_now || (poll_interval_s > 0 && sample_now_ms() >= next_check_ms)))
    {
      next_check_ms = sample_now_ms() + (uint64_t)poll_interval_s * 1000u;
      if (az_iot_su_client_request_update(&su, AZ_IOT_SU_REQUEST_DEFAULT_TIMEOUT_MS) != AZ_IOT_OK)
      {
        fprintf(stderr, "Could not request an update check.\n");
      }
    }
  }

  az_iot_connection_client_close(&connection);
  for (int i = 0; i < 100; ++i)
  {
    (void)az_iot_connection_client_do_work(&connection, 50);
  }

cleanup:
  if (su_ready)
  {
    az_iot_su_client_deinit(&su);
  }
  if (connection_ready)
  {
    az_iot_connection_client_deinit(&connection);
  }
#if defined(AZ_IOT_SAMPLE_ADAPTER_PAHO)
  az_iot_paho_factory_destroy(factory_v3);
  az_iot_paho_factory_destroy(factory_v5);
#elif defined(AZ_IOT_SAMPLE_ADAPTER_AZ_MQTT)
  az_iot_az_mqtt_factory_destroy(factory_v3);
  az_iot_az_mqtt_factory_destroy(factory_v5);
#endif
  az_iot_certificate_provider_pem_deinit(&certs);
  sample_config_release(&config);
  download_reset(&dl);
  reader_close(&dl);
  if (dl.multi != NULL)
  {
    (void)curl_multi_cleanup(dl.multi);
  }
  curl_global_cleanup();
  free(dl.proxy_url);
  free(dl.path_prefix);
  free(manufacturer);
  free(model);
  free(proxy_host);
  free(proxy_port);
  free(proxy_username);
  free(proxy_password);
  return rc;
}
