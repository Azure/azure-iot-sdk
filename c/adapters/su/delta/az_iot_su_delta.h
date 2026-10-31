// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/**
 * @file az_iot_su_delta.h
 * @brief Reference `microsoft/delta:1` download handler (POSIX): rebuilds an update file from a
 * delta related file and a source file kept from an earlier update.
 *
 * - choose_related_file_fn picks the first related file whose source, named by the
 *   `microsoft.sourceFileHash` and `microsoft.sourceFileHashAlgorithm` properties, is in the
 *   source cache.
 * - build_file_from_related_fn applies the downloaded delta to that source, writing the file.
 * - workflow_succeeded_fn moves the update's files into the source cache and removes the other
 *   entries of the update provider, so the cache keeps only the latest update per provider.
 *
 * Cache entries are `<directory>/<provider>/<algorithm>-<hash>`: in `<provider>` and
 * `<algorithm>`, characters other than letters, digits and `-` become `_`; in the base64 hash,
 * `+`, `/` and `=` become `_2B`, `_2F` and `_3D`. This is the layout of the Device Update
 * reference agent, so both can share a cache.
 *
 * The default delta processor is `libadudiffapi` (github.com/Azure/iot-hub-device-update-delta),
 * loaded with dlopen() on first use; the SDK neither includes nor links it. When it cannot be
 * loaded, choose_related_file_fn declines and the client downloads files in full. The hooks run
 * in az_iot_su_client_do_work(): the default processor blocks it while it applies a delta.
 */
#ifndef AZ_IOT_SU_DELTA_H
#define AZ_IOT_SU_DELTA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "azure/iot/az_iot_result.h"
#include "azure/iot/az_iot_su.h"

#ifdef __cplusplus
extern "C"
{
#endif

/** @brief Download handler id the handler serves. */
#define AZ_IOT_SU_DELTA_DOWNLOAD_HANDLER_ID "microsoft/delta:1"

/** @brief Default source cache directory, that of the Device Update reference agent. */
#define AZ_IOT_SU_DELTA_DEFAULT_SOURCE_CACHE_DIRECTORY "/var/lib/adu/sdc"

/** @brief Default delta processor library, found through the dynamic loader's search path. */
#define AZ_IOT_SU_DELTA_DEFAULT_LIBRARY_PATH "libadudiffapi.so"

/** @brief Largest path, terminator included, the handler builds or accepts. */
#define AZ_IOT_SU_DELTA_MAX_PATH_SIZE 1024

/** @brief Largest update provider, terminator included, the handler accepts. */
#define AZ_IOT_SU_DELTA_MAX_PROVIDER_SIZE 65

  /**
   * @brief Write the path where the application's download_fn stores @p file, the way
   * download_fn gets it, as a NUL-terminated string.
   *
   * Called for update files and related files: @p file_index is the update file's position in
   * its step, as download_fn gets it, in both cases. read_file_fn must read the files at these
   * paths, including the files the handler builds there.
   *
   * @param[in] file An update file or a related file.
   * @param[in] file_index Position of the update file in its step (the last step that uses it,
   * after the workflow succeeded).
   * @param[out] path Buffer for the path.
   * @param[in] path_size Capacity of @p path, terminator included.
   * @param[in] user_ctx az_iot_su_delta_handler_options::user_ctx.
   * @return AZ_IOT_SU_RESULT_SUCCESS, or AZ_IOT_SU_RESULT_FAILURE.
   */
  typedef int32_t (*az_iot_su_delta_get_file_path_fn)(
      const az_iot_su_client_update_manifest_file* file,
      uint32_t file_index,
      char* path,
      size_t path_size,
      void* user_ctx);

  /**
   * @brief Write @p target_path from @p source_path and the delta at @p delta_path.
   *
   * @param[in] source_path Source file, in the source cache.
   * @param[in] delta_path Downloaded delta file.
   * @param[in] target_path File to write: a private file in the source cache directory, which the
   * handler then moves to the path #az_iot_su_delta_get_file_path_fn gives.
   * @param[in] user_ctx az_iot_su_delta_handler_options::user_ctx.
   * @return AZ_IOT_SU_RESULT_SUCCESS; AZ_IOT_SU_RESULT_IN_PROGRESS to be called again with the
   * same paths on the next do_work(); anything else is a failure, after which the handler removes
   * @p target_path.
   */
  typedef int32_t (*az_iot_su_delta_apply_fn)(
      const char* source_path,
      const char* delta_path,
      const char* target_path,
      void* user_ctx);

  /** @brief Options of az_iot_su_delta_handler_init(). */
  typedef struct az_iot_su_delta_handler_options
  {
    /** Source cache directory, created when needed; must outlive the handler.
     * NULL: #AZ_IOT_SU_DELTA_DEFAULT_SOURCE_CACHE_DIRECTORY. */
    const char* source_cache_directory;
    /** Required: where the application's download_fn stores files. */
    az_iot_su_delta_get_file_path_fn get_file_path_fn;
    /** Delta processor. Default (NULL): `libadudiffapi`, loaded from #library_path. */
    az_iot_su_delta_apply_fn apply_delta_fn;
    /** Library of the default delta processor, for dlopen(); must outlive the handler.
     * NULL: #AZ_IOT_SU_DELTA_DEFAULT_LIBRARY_PATH. Unused with #apply_delta_fn. */
    const char* library_path;
    /** Passed to #get_file_path_fn and #apply_delta_fn. */
    void* user_ctx;
  } az_iot_su_delta_handler_options;

  /**
   * @brief The reference `microsoft/delta:1` download handler. Initialize with
   * az_iot_su_delta_handler_init(); keep it alive as long as the software updates client.
   */
  typedef struct az_iot_su_delta_handler
  {
    struct
    {
      az_iot_su_delta_handler_options options;
      /** Cache directory name of the provider of the update being built. */
      char provider_directory[AZ_IOT_SU_DELTA_MAX_PROVIDER_SIZE];
      /** The delta processor returned IN_PROGRESS for the file being built. */
      bool build_in_progress;
      /** The default processor's library (dlopen()) and functions, or NULL. */
      void* library;
      void* create_session;
      void* close_session;
      void* apply;
      void* get_error_count;
      void* get_error_text;
      void* get_error_code;
    } _internal;
  } az_iot_su_delta_handler;

  /**
   * @brief Default options: the default cache directory and delta processor, no
   * get_file_path_fn.
   *
   * @return The options.
   */
  az_iot_su_delta_handler_options az_iot_su_delta_handler_options_default(void);

  /**
   * @brief Initialize @p handler.
   *
   * @param[out] handler The handler.
   * @param[in] options Options; copied.
   * @return AZ_IOT_OK; AZ_IOT_ERR_INVALID_ARG when an argument or get_file_path_fn is NULL, or the
   * directory or library path is empty or does not fit #AZ_IOT_SU_DELTA_MAX_PATH_SIZE.
   */
  AZ_NODISCARD az_iot_result az_iot_su_delta_handler_init(
      az_iot_su_delta_handler* handler,
      const az_iot_su_delta_handler_options* options);

  /**
   * @brief Get the download handler to register in
   * az_iot_su_client_config_options::download_handlers.
   *
   * @param[in] handler An initialized handler; the hooks' user_ctx.
   * @return The download handler, with id #AZ_IOT_SU_DELTA_DOWNLOAD_HANDLER_ID.
   */
  az_iot_su_download_handler az_iot_su_delta_handler_get_download_handler(
      az_iot_su_delta_handler* handler);

  /**
   * @brief Write the source cache path of a file, e.g. to seed the cache with the factory
   * image.
   *
   * @param[in] handler An initialized handler.
   * @param[in] provider Update provider, as decoded text (`Contoso.Ltd`, not JSON-escaped).
   * @param[in] hash_algorithm Hash algorithm, e.g. `sha256`.
   * @param[in] hash Base64 hash of the file.
   * @param[out] path Buffer for the NUL-terminated path.
   * @param[in] path_size Capacity of @p path.
   * @return AZ_IOT_OK; AZ_IOT_ERR_INVALID_ARG when an argument is NULL or empty, the hash is not
   * base64 or the provider is too long; AZ_IOT_ERR_NOT_ENOUGH_SPACE when @p path is too small.
   */
  AZ_NODISCARD az_iot_result az_iot_su_delta_handler_get_source_cache_path(
      const az_iot_su_delta_handler* handler,
      az_span provider,
      az_span hash_algorithm,
      az_span hash,
      char* path,
      size_t path_size);

  /**
   * @brief Unload the default delta processor, if loaded. Call after the software updates client
   * is deinitialized.
   *
   * @param[in,out] handler The handler.
   */
  void az_iot_su_delta_handler_deinit(az_iot_su_delta_handler* handler);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_SU_DELTA_H */
