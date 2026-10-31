// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/**
 * @file az_iot_su_delta.c
 * @brief Reference `microsoft/delta:1` download handler (POSIX).
 */

#define _POSIX_C_SOURCE 200809L

#include "az_iot_su_delta.h"

#include "azure/iot/az_iot_log.h"
#include "azure/iot/az_iot_log_components.h"

#include <azure/core/az_json.h>

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/** Largest hash or algorithm read from a related file's properties, terminator included. */
#define SU_DELTA_MAX_PROPERTY_SIZE 129

/** Error entries of the default delta processor logged after a failure. */
#define SU_DELTA_MAX_LOGGED_ERRORS 8

/** Permissions of the cache directories the handler creates. */
#define SU_DELTA_DIRECTORY_MODE 0750

/** Permissions of the cache files the handler copies. */
#define SU_DELTA_FILE_MODE 0640

/* libadudiffapi legacy apply API (iot-hub-device-update-delta, legacy_adudiffapply.h). */
typedef void* (*su_delta_create_session_fn)(void);
typedef void (*su_delta_close_session_fn)(void* session);
typedef int (*su_delta_apply_fn)(
    void* session,
    const char* source_path,
    const char* delta_path,
    const char* target_path);
typedef uint32_t (*su_delta_get_error_count_fn)(void* session);
typedef const char* (*su_delta_get_error_text_fn)(void* session, uint32_t index);
typedef uint32_t (*su_delta_get_error_code_fn)(void* session, uint32_t index);

/** @brief Write @p name to @p out, characters other than letters, digits and `-` as `_`. */
static bool su_delta_sanitize_directory_name(az_span name, char* out, size_t out_size)
{
  size_t size = (size_t)az_span_size(name);
  if (size == 0 || size >= out_size)
  {
    return false;
  }
  const uint8_t* bytes = az_span_ptr(name);
  for (size_t i = 0; i < size; i++)
  {
    char c = (char)bytes[i];
    bool keep
        = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-';
    out[i] = c;
    if (!keep)
    {
      out[i] = '_';
    }
  }
  out[size] = '\0';
  return true;
}

/** @brief Write the cache file name `<algorithm>-<encoded hash>` of a file. */
static az_iot_result su_delta_get_cache_file_name(
    az_span hash_algorithm,
    az_span hash,
    char* out,
    size_t out_size)
{
  char algorithm[SU_DELTA_MAX_PROPERTY_SIZE];
  if (az_span_size(hash) == 0
      || !su_delta_sanitize_directory_name(hash_algorithm, algorithm, sizeof(algorithm)))
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  size_t length = strlen(algorithm);
  if (length + 1 >= out_size)
  {
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }
  memcpy(out, algorithm, length);
  out[length++] = '-';

  const uint8_t* bytes = az_span_ptr(hash);
  for (int32_t i = 0; i < az_span_size(hash); i++)
  {
    uint8_t c = bytes[i];
    const char* encoded = NULL;
    if (c == '+')
    {
      encoded = "_2B";
    }
    else if (c == '/')
    {
      encoded = "_2F";
    }
    else if (c == '=')
    {
      encoded = "_3D";
    }
    else if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')))
    {
      return AZ_IOT_ERR_INVALID_ARG;
    }
    size_t encoded_length = encoded == NULL ? 1 : 3;
    if (length + encoded_length >= out_size)
    {
      return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
    }
    if (encoded == NULL)
    {
      out[length] = (char)c;
    }
    else
    {
      memcpy(out + length, encoded, encoded_length);
    }
    length += encoded_length;
  }
  out[length] = '\0';
  return AZ_IOT_OK;
}

/** @brief Write `<directory>/<provider directory>[/<file name>]` to @p out. */
static az_iot_result su_delta_join_cache_path(
    const az_iot_su_delta_handler* handler,
    const char* provider_directory,
    const char* file_name,
    char* out,
    size_t out_size)
{
  int written = file_name == NULL ? snprintf(
                                        out,
                                        out_size,
                                        "%s/%s",
                                        handler->_internal.options.source_cache_directory,
                                        provider_directory)
                                  : snprintf(
                                        out,
                                        out_size,
                                        "%s/%s/%s",
                                        handler->_internal.options.source_cache_directory,
                                        provider_directory,
                                        file_name);
  return (written < 0 || (size_t)written >= out_size) ? AZ_IOT_ERR_NOT_ENOUGH_SPACE : AZ_IOT_OK;
}

/** @brief Write the cache path of the file with @p hash under @p provider_directory. */
static az_iot_result su_delta_get_cache_path(
    const az_iot_su_delta_handler* handler,
    const char* provider_directory,
    az_span hash_algorithm,
    az_span hash,
    char* out,
    size_t out_size)
{
  char file_name[AZ_IOT_SU_DELTA_MAX_PATH_SIZE];
  az_iot_result result
      = su_delta_get_cache_file_name(hash_algorithm, hash, file_name, sizeof(file_name));
  if (result != AZ_IOT_OK)
  {
    return result;
  }
  return su_delta_join_cache_path(handler, provider_directory, file_name, out, out_size);
}

/** @brief Read the source hash and algorithm from a related file's `properties`. */
static bool su_delta_read_source_properties(
    az_span properties,
    char* hash,
    char* hash_algorithm,
    size_t size)
{
  hash[0] = '\0';
  hash_algorithm[0] = '\0';
  az_json_reader reader;
  if (az_span_size(properties) == 0 || az_json_reader_init(&reader, properties, NULL) != AZ_OK
      || az_json_reader_next_token(&reader) != AZ_OK
      || reader.token.kind != AZ_JSON_TOKEN_BEGIN_OBJECT)
  {
    return false;
  }
  while (az_json_reader_next_token(&reader) == AZ_OK
         && reader.token.kind == AZ_JSON_TOKEN_PROPERTY_NAME)
  {
    char* target = NULL;
    if (az_json_token_is_text_equal(&reader.token, AZ_SPAN_FROM_STR("microsoft.sourceFileHash")))
    {
      target = hash;
    }
    else if (az_json_token_is_text_equal(
                 &reader.token, AZ_SPAN_FROM_STR("microsoft.sourceFileHashAlgorithm")))
    {
      target = hash_algorithm;
    }
    if (az_json_reader_next_token(&reader) != AZ_OK)
    {
      return false;
    }
    if (target != NULL && reader.token.kind == AZ_JSON_TOKEN_STRING)
    {
      if (az_json_token_get_string(&reader.token, target, (int32_t)size, NULL) != AZ_OK)
      {
        return false;
      }
    }
    else if (az_json_reader_skip_children(&reader) != AZ_OK)
    {
      return false;
    }
  }
  return hash[0] != '\0' && hash_algorithm[0] != '\0';
}

/** @brief Write the cache path of the source of the delta @p related_file. */
static bool su_delta_get_source_path(
    const az_iot_su_delta_handler* handler,
    const az_iot_su_client_update_manifest_file* related_file,
    char* out,
    size_t out_size)
{
  char hash[SU_DELTA_MAX_PROPERTY_SIZE];
  char hash_algorithm[SU_DELTA_MAX_PROPERTY_SIZE];
  if (!su_delta_read_source_properties(
          related_file->properties, hash, hash_algorithm, sizeof(hash)))
  {
    AZ_IOT_LOG_WARNF(
        AZ_IOT_LOG_COMPONENT_SU,
        "delta: related file '%.*s' has no microsoft.sourceFileHash and "
        "microsoft.sourceFileHashAlgorithm",
        (int)az_span_size(related_file->id),
        (const char*)az_span_ptr(related_file->id));
    return false;
  }
  return su_delta_get_cache_path(
             handler,
             handler->_internal.provider_directory,
             az_span_create_from_str(hash_algorithm),
             az_span_create_from_str(hash),
             out,
             out_size)
      == AZ_IOT_OK;
}

static bool su_delta_is_readable_file(const char* path)
{
  struct stat info;
  return stat(path, &info) == 0 && S_ISREG(info.st_mode) && access(path, R_OK) == 0;
}

/** @brief Load the default delta processor, unless loaded. */
static bool su_delta_load_library(az_iot_su_delta_handler* handler)
{
  if (handler->_internal.library != NULL)
  {
    return true;
  }
  const char* path = handler->_internal.options.library_path;
  void* library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
  if (library == NULL)
  {
    const char* error = dlerror();
    AZ_IOT_LOG_WARNF(
        AZ_IOT_LOG_COMPONENT_SU,
        "delta: cannot load '%s' (%s); downloading files in full",
        path,
        error == NULL ? "unknown error" : error);
    return false;
  }
  void* create_session = dlsym(library, "adu_diff_apply_create_session");
  void* close_session = dlsym(library, "adu_diff_apply_close_session");
  void* apply = dlsym(library, "adu_diff_apply");
  void* get_error_count = dlsym(library, "adu_diff_apply_get_error_count");
  void* get_error_text = dlsym(library, "adu_diff_apply_get_error_text");
  void* get_error_code = dlsym(library, "adu_diff_apply_get_error_code");
  if (create_session == NULL || close_session == NULL || apply == NULL || get_error_count == NULL
      || get_error_text == NULL || get_error_code == NULL)
  {
    AZ_IOT_LOG_WARNF(
        AZ_IOT_LOG_COMPONENT_SU,
        "delta: '%s' lacks the delta apply functions; downloading files in full",
        path);
    (void)dlclose(library);
    return false;
  }
  handler->_internal.library = library;
  handler->_internal.create_session = create_session;
  handler->_internal.close_session = close_session;
  handler->_internal.apply = apply;
  handler->_internal.get_error_count = get_error_count;
  handler->_internal.get_error_text = get_error_text;
  handler->_internal.get_error_code = get_error_code;
  return true;
}

/** @brief Apply a delta with the default delta processor. */
static int32_t su_delta_apply_with_library(
    const az_iot_su_delta_handler* handler,
    const char* source_path,
    const char* delta_path,
    const char* target_path)
{
  /* memcpy: ISO C has no conversion from void* to a function pointer. */
  su_delta_create_session_fn create_session;
  su_delta_close_session_fn close_session;
  su_delta_apply_fn apply;
  su_delta_get_error_count_fn get_error_count;
  su_delta_get_error_text_fn get_error_text;
  su_delta_get_error_code_fn get_error_code;
  memcpy(
      (void*)&create_session,
      (const void*)&handler->_internal.create_session,
      sizeof(create_session));
  memcpy(
      (void*)&close_session, (const void*)&handler->_internal.close_session, sizeof(close_session));
  memcpy((void*)&apply, (const void*)&handler->_internal.apply, sizeof(apply));
  memcpy(
      (void*)&get_error_count,
      (const void*)&handler->_internal.get_error_count,
      sizeof(get_error_count));
  memcpy(
      (void*)&get_error_text,
      (const void*)&handler->_internal.get_error_text,
      sizeof(get_error_text));
  memcpy(
      (void*)&get_error_code,
      (const void*)&handler->_internal.get_error_code,
      sizeof(get_error_code));

  void* session = create_session();
  if (session == NULL)
  {
    AZ_IOT_LOG_ERROR(AZ_IOT_LOG_COMPONENT_SU, "delta: cannot create a delta apply session");
    return AZ_IOT_SU_RESULT_FAILURE;
  }
  int32_t result = AZ_IOT_SU_RESULT_SUCCESS;
  if (apply(session, source_path, delta_path, target_path) != 0)
  {
    result = AZ_IOT_SU_RESULT_FAILURE;
    uint32_t count = get_error_count(session);
    for (uint32_t i = 0; i < count && i < SU_DELTA_MAX_LOGGED_ERRORS; i++)
    {
      const char* text = get_error_text(session, i);
      AZ_IOT_LOG_ERRORF(
          AZ_IOT_LOG_COMPONENT_SU,
          "delta: apply error %u: %s",
          (unsigned)get_error_code(session, i),
          text == NULL ? "" : text);
    }
  }
  close_session(session);
  return result;
}

static int32_t su_delta_choose_related_file(
    const az_iot_su_client_update_manifest* manifest,
    const az_iot_su_client_update_manifest_file* file,
    uint32_t* out_related_file_index,
    void* user_ctx)
{
  az_iot_su_delta_handler* handler = (az_iot_su_delta_handler*)user_ctx;
  if (handler == NULL || manifest == NULL || file == NULL || out_related_file_index == NULL)
  {
    return AZ_IOT_SU_RESULT_FAILURE;
  }
  handler->_internal.build_in_progress = false;
  if (!su_delta_sanitize_directory_name(
          manifest->update_id.provider,
          handler->_internal.provider_directory,
          sizeof(handler->_internal.provider_directory)))
  {
    AZ_IOT_LOG_WARN(AZ_IOT_LOG_COMPONENT_SU, "delta: update provider empty or too long");
    return AZ_IOT_SU_RESULT_FAILURE;
  }
  if (handler->_internal.options.apply_delta_fn == NULL && !su_delta_load_library(handler))
  {
    return AZ_IOT_SU_RESULT_FAILURE;
  }

  for (uint32_t i = 0; i < file->related_files_count; i++)
  {
    uint32_t index = file->related_files_index + i;
    if (manifest->related_files == NULL || index < file->related_files_index
        || index >= manifest->related_files_count)
    {
      break;
    }
    char source_path[AZ_IOT_SU_DELTA_MAX_PATH_SIZE];
    if (su_delta_get_source_path(
            handler, &manifest->related_files[index], source_path, sizeof(source_path))
        && su_delta_is_readable_file(source_path))
    {
      AZ_IOT_LOG_INFOF(
          AZ_IOT_LOG_COMPONENT_SU,
          "delta: file '%.*s' from related file '%.*s' and cached source '%s'",
          (int)az_span_size(file->id),
          (const char*)az_span_ptr(file->id),
          (int)az_span_size(manifest->related_files[index].id),
          (const char*)az_span_ptr(manifest->related_files[index].id),
          source_path);
      *out_related_file_index = i;
      return AZ_IOT_SU_RESULT_SUCCESS;
    }
  }
  AZ_IOT_LOG_INFOF(
      AZ_IOT_LOG_COMPONENT_SU,
      "delta: no cached source for file '%.*s'; downloading it in full",
      (int)az_span_size(file->id),
      (const char*)az_span_ptr(file->id));
  return AZ_IOT_SU_RESULT_FAILURE;
}

static int32_t su_delta_build_file_from_related(
    const az_iot_su_client_update_manifest_file* file,
    const az_iot_su_client_update_manifest_file* related_file,
    uint32_t file_index,
    void* user_ctx)
{
  az_iot_su_delta_handler* handler = (az_iot_su_delta_handler*)user_ctx;
  if (handler == NULL || file == NULL || related_file == NULL)
  {
    return AZ_IOT_SU_RESULT_FAILURE;
  }
  const az_iot_su_delta_handler_options* options = &handler->_internal.options;
  char source_path[AZ_IOT_SU_DELTA_MAX_PATH_SIZE];
  char delta_path[AZ_IOT_SU_DELTA_MAX_PATH_SIZE];
  char target_path[AZ_IOT_SU_DELTA_MAX_PATH_SIZE];
  if (!su_delta_get_source_path(handler, related_file, source_path, sizeof(source_path))
      || options->get_file_path_fn(
             related_file, file_index, delta_path, sizeof(delta_path), options->user_ctx)
          != AZ_IOT_SU_RESULT_SUCCESS
      || options->get_file_path_fn(
             file, file_index, target_path, sizeof(target_path), options->user_ctx)
          != AZ_IOT_SU_RESULT_SUCCESS)
  {
    AZ_IOT_LOG_ERROR(AZ_IOT_LOG_COMPONENT_SU, "delta: cannot get the paths to build a file");
    handler->_internal.build_in_progress = false;
    return AZ_IOT_SU_RESULT_FAILURE;
  }

  if (!handler->_internal.build_in_progress)
  {
    (void)unlink(target_path);
  }
  int32_t result = options->apply_delta_fn != NULL
      ? options->apply_delta_fn(source_path, delta_path, target_path, options->user_ctx)
      : su_delta_apply_with_library(handler, source_path, delta_path, target_path);
  if (result == AZ_IOT_SU_RESULT_IN_PROGRESS)
  {
    handler->_internal.build_in_progress = true;
    return AZ_IOT_SU_RESULT_IN_PROGRESS;
  }
  handler->_internal.build_in_progress = false;
  if (result != AZ_IOT_SU_RESULT_SUCCESS)
  {
    AZ_IOT_LOG_ERRORF(
        AZ_IOT_LOG_COMPONENT_SU,
        "delta: cannot build '%s' from '%s' and '%s'",
        target_path,
        source_path,
        delta_path);
    (void)unlink(target_path);
    return AZ_IOT_SU_RESULT_FAILURE;
  }
  return AZ_IOT_SU_RESULT_SUCCESS;
}

/** @brief Create @p path and its missing parents. */
static bool su_delta_make_directories(const char* path)
{
  char partial[AZ_IOT_SU_DELTA_MAX_PATH_SIZE];
  size_t length = strlen(path);
  if (length == 0 || length >= sizeof(partial))
  {
    return false;
  }
  memcpy(partial, path, length + 1);
  for (size_t i = 1; i <= length; i++)
  {
    if (partial[i] == '/' || partial[i] == '\0')
    {
      char saved = partial[i];
      partial[i] = '\0';
      if (mkdir(partial, SU_DELTA_DIRECTORY_MODE) != 0 && errno != EEXIST)
      {
        return false;
      }
      partial[i] = saved;
    }
  }
  struct stat info;
  return stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

/** @brief Copy @p source_path to @p target_path through a temporary file. */
static bool su_delta_copy_file(const char* source_path, const char* target_path)
{
  char temporary_path[AZ_IOT_SU_DELTA_MAX_PATH_SIZE];
  int written = snprintf(temporary_path, sizeof(temporary_path), "%s.tmp", target_path);
  if (written < 0 || (size_t)written >= sizeof(temporary_path))
  {
    return false;
  }
  int source = open(source_path, O_RDONLY | O_CLOEXEC);
  if (source < 0)
  {
    return false;
  }
  int target
      = open(temporary_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, (mode_t)SU_DELTA_FILE_MODE);
  if (target < 0)
  {
    (void)close(source);
    return false;
  }
  bool ok = true;
  char buffer[4096];
  for (;;)
  {
    ssize_t count = read(source, buffer, sizeof(buffer));
    if (count < 0 && errno == EINTR)
    {
      continue;
    }
    if (count <= 0)
    {
      ok = count == 0;
      break;
    }
    for (ssize_t done = 0; ok && done < count;)
    {
      ssize_t step = write(target, buffer + done, (size_t)(count - done));
      if (step < 0 && errno == EINTR)
      {
        continue;
      }
      ok = step > 0;
      done += step > 0 ? step : 0;
    }
    if (!ok)
    {
      break;
    }
  }
  ok = fsync(target) == 0 && ok;
  ok = close(target) == 0 && ok;
  (void)close(source);
  if (!ok || rename(temporary_path, target_path) != 0)
  {
    (void)unlink(temporary_path);
    return false;
  }
  return true;
}

/** @brief Move @p source_path to @p target_path, copying across file systems. */
static bool su_delta_move_file(const char* source_path, const char* target_path)
{
  if (rename(source_path, target_path) == 0)
  {
    return true;
  }
  if (!su_delta_copy_file(source_path, target_path))
  {
    return false;
  }
  (void)unlink(source_path);
  return true;
}

/** @brief Whether @p name is the cache file name of a file of @p manifest. */
static bool su_delta_is_manifest_file(
    const az_iot_su_client_update_manifest* manifest,
    const char* name)
{
  for (uint32_t i = 0; i < manifest->files_count; i++)
  {
    const az_iot_su_client_update_manifest_file* file = &manifest->files[i];
    char file_name[AZ_IOT_SU_DELTA_MAX_PATH_SIZE];
    if (file->hashes_count > 0
        && su_delta_get_cache_file_name(
               file->hashes[0].hash_type, file->hashes[0].hash_value, file_name, sizeof(file_name))
            == AZ_IOT_OK
        && strcmp(file_name, name) == 0)
    {
      return true;
    }
  }
  return false;
}

/** @brief Remove the entries of @p directory that are not files of @p manifest. */
static void su_delta_remove_other_entries(
    const az_iot_su_client_update_manifest* manifest,
    const char* directory)
{
  DIR* entries = opendir(directory);
  if (entries == NULL)
  {
    return;
  }
  for (struct dirent* entry = readdir(entries); entry != NULL; entry = readdir(entries))
  {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0
        || su_delta_is_manifest_file(manifest, entry->d_name))
    {
      continue;
    }
    char path[AZ_IOT_SU_DELTA_MAX_PATH_SIZE];
    int written = snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
    struct stat info;
    if (written > 0 && (size_t)written < sizeof(path) && lstat(path, &info) == 0
        && !S_ISDIR(info.st_mode))
    {
      AZ_IOT_LOG_INFOF(AZ_IOT_LOG_COMPONENT_SU, "delta: removing cached source '%s'", path);
      (void)unlink(path);
    }
  }
  (void)closedir(entries);
}

static void su_delta_workflow_succeeded(
    const az_iot_su_client_update_manifest* manifest,
    void* user_ctx)
{
  az_iot_su_delta_handler* handler = (az_iot_su_delta_handler*)user_ctx;
  if (handler == NULL || manifest == NULL)
  {
    return;
  }
  const az_iot_su_delta_handler_options* options = &handler->_internal.options;
  char provider_directory[AZ_IOT_SU_DELTA_MAX_PROVIDER_SIZE];
  char directory[AZ_IOT_SU_DELTA_MAX_PATH_SIZE];
  if (!su_delta_sanitize_directory_name(
          manifest->update_id.provider, provider_directory, sizeof(provider_directory))
      || su_delta_join_cache_path(handler, provider_directory, NULL, directory, sizeof(directory))
          != AZ_IOT_OK
      || !su_delta_make_directories(directory))
  {
    AZ_IOT_LOG_ERROR(AZ_IOT_LOG_COMPONENT_SU, "delta: cannot create the source cache directory");
    return;
  }

  uint32_t cached_count = 0;
  for (uint32_t i = 0; i < manifest->files_count; i++)
  {
    const az_iot_su_client_update_manifest_file* file = &manifest->files[i];
    char cache_path[AZ_IOT_SU_DELTA_MAX_PATH_SIZE];
    char file_path[AZ_IOT_SU_DELTA_MAX_PATH_SIZE];
    if (file->hashes_count == 0
        || su_delta_get_cache_path(
               handler,
               provider_directory,
               file->hashes[0].hash_type,
               file->hashes[0].hash_value,
               cache_path,
               sizeof(cache_path))
            != AZ_IOT_OK)
    {
      continue;
    }
    struct stat info;
    if (stat(cache_path, &info) == 0 && S_ISREG(info.st_mode)
        && (int64_t)info.st_size == file->size_in_bytes)
    {
      cached_count++;
      continue;
    }
    if (options->get_file_path_fn(file, i, file_path, sizeof(file_path), options->user_ctx)
            == AZ_IOT_SU_RESULT_SUCCESS
        && su_delta_move_file(file_path, cache_path))
    {
      AZ_IOT_LOG_INFOF(AZ_IOT_LOG_COMPONENT_SU, "delta: cached source '%s'", cache_path);
      cached_count++;
    }
    else
    {
      AZ_IOT_LOG_WARNF(
          AZ_IOT_LOG_COMPONENT_SU,
          "delta: cannot cache file '%.*s'",
          (int)az_span_size(file->id),
          (const char*)az_span_ptr(file->id));
    }
  }
  if (cached_count > 0)
  {
    su_delta_remove_other_entries(manifest, directory);
  }
}

az_iot_su_delta_handler_options az_iot_su_delta_handler_options_default(void)
{
  az_iot_su_delta_handler_options options;
  memset(&options, 0, sizeof(options));
  options.source_cache_directory = AZ_IOT_SU_DELTA_DEFAULT_SOURCE_CACHE_DIRECTORY;
  options.library_path = AZ_IOT_SU_DELTA_DEFAULT_LIBRARY_PATH;
  return options;
}

static bool su_delta_is_valid_path(const char* path)
{
  return path != NULL && path[0] != '\0'
      && strnlen(path, AZ_IOT_SU_DELTA_MAX_PATH_SIZE) < AZ_IOT_SU_DELTA_MAX_PATH_SIZE;
}

AZ_NODISCARD az_iot_result az_iot_su_delta_handler_init(
    az_iot_su_delta_handler* handler,
    const az_iot_su_delta_handler_options* options)
{
  if (handler == NULL || options == NULL || options->get_file_path_fn == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  az_iot_su_delta_handler_options resolved = *options;
  if (resolved.source_cache_directory == NULL)
  {
    resolved.source_cache_directory = AZ_IOT_SU_DELTA_DEFAULT_SOURCE_CACHE_DIRECTORY;
  }
  if (resolved.library_path == NULL)
  {
    resolved.library_path = AZ_IOT_SU_DELTA_DEFAULT_LIBRARY_PATH;
  }
  if (!su_delta_is_valid_path(resolved.source_cache_directory)
      || !su_delta_is_valid_path(resolved.library_path))
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  memset(handler, 0, sizeof(*handler));
  handler->_internal.options = resolved;
  return AZ_IOT_OK;
}

az_iot_su_download_handler az_iot_su_delta_handler_get_download_handler(
    az_iot_su_delta_handler* handler)
{
  az_iot_su_download_handler download_handler;
  memset(&download_handler, 0, sizeof(download_handler));
  download_handler.id = AZ_IOT_SU_DELTA_DOWNLOAD_HANDLER_ID;
  download_handler.choose_related_file_fn = su_delta_choose_related_file;
  download_handler.build_file_from_related_fn = su_delta_build_file_from_related;
  download_handler.workflow_succeeded_fn = su_delta_workflow_succeeded;
  download_handler.user_ctx = handler;
  return download_handler;
}

AZ_NODISCARD az_iot_result az_iot_su_delta_handler_get_source_cache_path(
    const az_iot_su_delta_handler* handler,
    az_span provider,
    az_span hash_algorithm,
    az_span hash,
    char* path,
    size_t path_size)
{
  char provider_directory[AZ_IOT_SU_DELTA_MAX_PROVIDER_SIZE];
  if (handler == NULL || path == NULL || path_size == 0
      || !su_delta_sanitize_directory_name(
          provider, provider_directory, sizeof(provider_directory)))
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  return su_delta_get_cache_path(
      handler, provider_directory, hash_algorithm, hash, path, path_size);
}

void az_iot_su_delta_handler_deinit(az_iot_su_delta_handler* handler)
{
  if (handler == NULL)
  {
    return;
  }
  if (handler->_internal.library != NULL)
  {
    (void)dlclose(handler->_internal.library);
  }
  memset(&handler->_internal, 0, sizeof(handler->_internal));
}
