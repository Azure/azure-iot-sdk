// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/**
 * @file su_delta_test.c
 * @brief Tests of the reference microsoft/delta:1 download handler (adapters/su/delta).
 *
 * Built twice. az_iot_tests_su_delta drives the handler's hooks with a fake delta processor.
 * az_iot_tests_su_delta_interop, built with AZ_IOT_TEST_SU_DELTA_LIBRARY, rebuilds the committed
 * target vector with the real delta processor library and checks its SHA-256.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cmocka.h>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "az_iot_su_delta.h"
#include "az_iot_su_delta_internal.h"

#ifdef AZ_IOT_TEST_SU_DELTA_LIBRARY
#include "az_iot_crypto_openssl.h"
#endif

#define WORK_DIRECTORY AZ_IOT_TEST_SU_DELTA_WORK_DIRECTORY
#define CACHE_DIRECTORY WORK_DIRECTORY "/cache"
#define FILES_DIRECTORY WORK_DIRECTORY "/files"
#define PROVIDER "Contoso.Ltd"
#define PROVIDER_DIRECTORY CACHE_DIRECTORY "/Contoso_Ltd"

/* Base64 SHA-256 hashes of source.swu, of no file and of target.swu. */
#define SOURCE_HASH "nuDNVz6kcEmOtefngw1gEGmcGB5L5ZRaBB0qbMbe90E="
#define OTHER_SOURCE_HASH "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8="
#define TARGET_HASH "SgQPjUkH/Z63YrpCFagAH/H3ZSj8tt/JYmd5lSZjdcw="
#define SOURCE_CACHE_FILE \
  PROVIDER_DIRECTORY "/sha256-nuDNVz6kcEmOtefngw1gEGmcGB5L5ZRaBB0qbMbe90E_3D"
#define TARGET_CACHE_FILE \
  PROVIDER_DIRECTORY "/sha256-SgQPjUkH_2FZ63YrpCFagAH_2FH3ZSj8tt_2FJYmd5lSZjdcw_3D"

#define SOURCE_PROPERTIES(hash)                                                \
  "{\"microsoft.sourceFileHashAlgorithm\":\"sha256\",\"other\":{\"a\":[1,2]}," \
  "\"microsoft.sourceFileHash\":\"" hash "\"}"

static void remove_tree(const char* path)
{
  struct stat info;
  if (lstat(path, &info) != 0)
  {
    return;
  }
  if (S_ISDIR(info.st_mode))
  {
    DIR* entries = opendir(path);
    if (entries != NULL)
    {
      for (struct dirent* entry = readdir(entries); entry != NULL; entry = readdir(entries))
      {
        if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0)
        {
          char child[1024];
          (void)snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
          remove_tree(child);
        }
      }
      (void)closedir(entries);
    }
    (void)rmdir(path);
  }
  else
  {
    (void)unlink(path);
  }
}

static void write_file(const char* path, const void* data, size_t size)
{
  FILE* file = fopen(path, "wb");
  assert_non_null(file);
  assert_int_equal(fwrite(data, 1, size, file), size);
  assert_int_equal(fclose(file), 0);
}

/* Reads up to @p capacity bytes of @p path; -1 when it does not exist. */
static long read_file(const char* path, void* buffer, size_t capacity)
{
  FILE* file = fopen(path, "rb");
  if (file == NULL)
  {
    return -1;
  }
  size_t size = fread(buffer, 1, capacity, file);
  (void)fclose(file);
  return (long)size;
}

static bool file_exists(const char* path)
{
  struct stat info;
  return stat(path, &info) == 0;
}

static int setup_work_directory(void** state)
{
  (void)state;
  remove_tree(WORK_DIRECTORY);
  if (mkdir(WORK_DIRECTORY, 0700) != 0 || mkdir(FILES_DIRECTORY, 0700) != 0)
  {
    return -1;
  }
  return 0;
}

static int teardown_work_directory(void** state)
{
  (void)state;
  remove_tree(WORK_DIRECTORY);
  return 0;
}

/* The application's storage: FILES_DIRECTORY/<file index>-<file id>. */
static int32_t get_file_path(
    const az_iot_su_client_update_manifest_file* file,
    uint32_t file_index,
    char* path,
    size_t path_size,
    void* user_ctx)
{
  (void)user_ctx;
  int written = snprintf(
      path,
      path_size,
      FILES_DIRECTORY "/%u-%.*s",
      (unsigned)file_index,
      (int)az_span_size(file->id),
      (const char*)az_span_ptr(file->id));
  return (written > 0 && (size_t)written < path_size) ? AZ_IOT_SU_RESULT_SUCCESS
                                                      : AZ_IOT_SU_RESULT_FAILURE;
}

static az_iot_su_client_update_manifest_file make_file(
    const char* id,
    const char* hash,
    int64_t size,
    const char* properties)
{
  az_iot_su_client_update_manifest_file file;
  memset(&file, 0, sizeof(file));
  file.id = az_span_create_from_str((char*)(uintptr_t)id);
  file.file_name = file.id;
  file.size_in_bytes = size;
  if (hash != NULL)
  {
    file.hashes[0].hash_type = AZ_SPAN_FROM_STR("sha256");
    file.hashes[0].hash_value = az_span_create_from_str((char*)(uintptr_t)hash);
    file.hashes_count = 1;
  }
  if (properties != NULL)
  {
    file.properties = az_span_create_from_str((char*)(uintptr_t)properties);
  }
  return file;
}

/* One step that uses every file of @p manifest, in order, as the manifest parser fills it. */
static void list_files_in_one_step(az_iot_su_client_update_manifest* manifest)
{
  manifest->instructions.steps_count = 1;
  manifest->instructions.steps[0].files_count = manifest->files_count;
  for (uint32_t i = 0; i < manifest->files_count; i++)
  {
    manifest->instructions.steps[0].files[i] = manifest->files[i].id;
  }
}

/* A manifest of one delta-enabled file whose related files are @p related_files. */
static void make_manifest(
    az_iot_su_client_update_manifest* manifest,
    az_iot_su_client_update_manifest_file* related_files,
    uint32_t related_files_count)
{
  memset(manifest, 0, sizeof(*manifest));
  manifest->update_id.provider = AZ_SPAN_FROM_STR(PROVIDER);
  manifest->update_id.name = AZ_SPAN_FROM_STR("firmware");
  manifest->update_id.version = AZ_SPAN_FROM_STR("2.0");
  manifest->files[0] = make_file("target", TARGET_HASH, 6, NULL);
  manifest->files[0].download_handler_id = AZ_SPAN_FROM_STR(AZ_IOT_SU_DELTA_DOWNLOAD_HANDLER_ID);
  manifest->files[0].related_files_count = related_files_count;
  manifest->files_count = 1;
  manifest->related_files = related_files;
  manifest->related_files_count = related_files_count;
  list_files_in_one_step(manifest);
}

static void seed_source(void)
{
  struct stat info;
  if (stat(CACHE_DIRECTORY, &info) != 0)
  {
    assert_int_equal(mkdir(CACHE_DIRECTORY, 0700), 0);
  }
  if (stat(PROVIDER_DIRECTORY, &info) != 0)
  {
    assert_int_equal(mkdir(PROVIDER_DIRECTORY, 0700), 0);
  }
  write_file(SOURCE_CACHE_FILE, "SOURCE", 6);
}

#ifndef AZ_IOT_TEST_SU_DELTA_LIBRARY

/* Fake delta processor: target = source + delta, or what the test scripts. */
typedef struct fake_processor
{
  int call_count;
  int32_t results[2];
  bool write_partial_target;
  bool target_existed[2];
  char source_path[AZ_IOT_SU_DELTA_MAX_PATH_SIZE];
  char delta_path[AZ_IOT_SU_DELTA_MAX_PATH_SIZE];
  char target_path[AZ_IOT_SU_DELTA_MAX_PATH_SIZE];
} fake_processor;

static int32_t fake_apply_delta(
    const char* source_path,
    const char* delta_path,
    const char* target_path,
    void* user_ctx)
{
  fake_processor* fake = (fake_processor*)user_ctx;
  int call = fake->call_count++;
  assert_true(call < 2);
  fake->target_existed[call] = file_exists(target_path);
  (void)snprintf(fake->source_path, sizeof(fake->source_path), "%s", source_path);
  (void)snprintf(fake->delta_path, sizeof(fake->delta_path), "%s", delta_path);
  (void)snprintf(fake->target_path, sizeof(fake->target_path), "%s", target_path);
  if (fake->results[call] == AZ_IOT_SU_RESULT_SUCCESS)
  {
    char source[16] = { 0 };
    char delta[16] = { 0 };
    char target[32];
    long source_size = read_file(source_path, source, sizeof(source));
    long delta_size = read_file(delta_path, delta, sizeof(delta));
    assert_true(source_size >= 0 && delta_size >= 0);
    int size = snprintf(target, sizeof(target), "%s%s", source, delta);
    write_file(target_path, target, (size_t)size);
  }
  else if (fake->write_partial_target)
  {
    write_file(target_path, "PART", 4);
  }
  return fake->results[call];
}

static void init_handler(az_iot_su_delta_handler* handler, fake_processor* fake)
{
  az_iot_su_delta_handler_options options = az_iot_su_delta_handler_options_default();
  options.source_cache_directory = CACHE_DIRECTORY;
  options.get_file_path_fn = get_file_path;
  options.apply_delta_fn = fake == NULL ? NULL : fake_apply_delta;
  options.user_ctx = fake;
  assert_int_equal(az_iot_su_delta_handler_init(handler, &options), AZ_IOT_OK);
}

static void test_options_default(void** state)
{
  (void)state;
  az_iot_su_delta_handler_options options = az_iot_su_delta_handler_options_default();
  assert_string_equal(options.source_cache_directory, "/var/lib/adu/sdc");
  assert_string_equal(options.library_path, "libadudiffapi.so");
  assert_null(options.get_file_path_fn);
  assert_null(options.apply_delta_fn);
  assert_null(options.user_ctx);
}

static void test_init_rejects_invalid_arguments(void** state)
{
  (void)state;
  az_iot_su_delta_handler handler;
  az_iot_su_delta_handler_options options = az_iot_su_delta_handler_options_default();
  assert_int_equal(az_iot_su_delta_handler_init(&handler, &options), AZ_IOT_ERR_INVALID_ARG);
  options.get_file_path_fn = get_file_path;
  assert_int_equal(az_iot_su_delta_handler_init(NULL, &options), AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(az_iot_su_delta_handler_init(&handler, NULL), AZ_IOT_ERR_INVALID_ARG);

  options.source_cache_directory = "";
  assert_int_equal(az_iot_su_delta_handler_init(&handler, &options), AZ_IOT_ERR_INVALID_ARG);
  static char long_path[AZ_IOT_SU_DELTA_MAX_PATH_SIZE + 1];
  memset(long_path, 'a', sizeof(long_path) - 1);
  options.source_cache_directory = long_path;
  assert_int_equal(az_iot_su_delta_handler_init(&handler, &options), AZ_IOT_ERR_INVALID_ARG);
  options.source_cache_directory = CACHE_DIRECTORY;
  options.library_path = "";
  assert_int_equal(az_iot_su_delta_handler_init(&handler, &options), AZ_IOT_ERR_INVALID_ARG);

  /* NULL strings select the defaults. */
  options.source_cache_directory = NULL;
  options.library_path = NULL;
  assert_int_equal(az_iot_su_delta_handler_init(&handler, &options), AZ_IOT_OK);
  char path[128];
  assert_int_equal(
      az_iot_su_delta_handler_get_source_cache_path(
          &handler,
          AZ_SPAN_FROM_STR("p"),
          AZ_SPAN_FROM_STR("sha256"),
          AZ_SPAN_FROM_STR("AA=="),
          path,
          sizeof(path)),
      AZ_IOT_OK);
  assert_string_equal(path, "/var/lib/adu/sdc/p/sha256-AA_3D_3D");
  az_iot_su_delta_handler_deinit(&handler);
  az_iot_su_delta_handler_deinit(NULL);
}

static void test_get_download_handler(void** state)
{
  (void)state;
  az_iot_su_delta_handler handler;
  init_handler(&handler, NULL);
  az_iot_su_download_handler download_handler
      = az_iot_su_delta_handler_get_download_handler(&handler);
  assert_string_equal(download_handler.id, "microsoft/delta:1");
  assert_non_null(download_handler.choose_related_file_fn);
  assert_non_null(download_handler.build_file_from_related_fn);
  assert_non_null(download_handler.workflow_succeeded_fn);
  assert_ptr_equal(download_handler.user_ctx, &handler);
  assert_true(strlen(download_handler.id) < AZ_IOT_SU_MAX_DOWNLOAD_HANDLER_ID_SIZE);
}

static void test_source_cache_path_encoding(void** state)
{
  (void)state;
  az_iot_su_delta_handler handler;
  init_handler(&handler, NULL);
  char path[AZ_IOT_SU_DELTA_MAX_PATH_SIZE];

  /* The Device Update reference agent's layout. */
  assert_int_equal(
      az_iot_su_delta_handler_get_source_cache_path(
          &handler,
          AZ_SPAN_FROM_STR("Contoso.Ltd/a b-1"),
          AZ_SPAN_FROM_STR("sha256"),
          AZ_SPAN_FROM_STR(TARGET_HASH),
          path,
          sizeof(path)),
      AZ_IOT_OK);
  assert_string_equal(
      path,
      CACHE_DIRECTORY
      "/Contoso_Ltd_a_b-1/sha256-SgQPjUkH_2FZ63YrpCFagAH_2FH3ZSj8tt_2FJYmd5lSZjdcw_3D");
  assert_int_equal(
      az_iot_su_delta_handler_get_source_cache_path(
          &handler,
          AZ_SPAN_FROM_STR(PROVIDER),
          AZ_SPAN_FROM_STR("SHA.256"),
          AZ_SPAN_FROM_STR("+/=="),
          path,
          sizeof(path)),
      AZ_IOT_OK);
  assert_string_equal(path, PROVIDER_DIRECTORY "/SHA_256-_2B_2F_3D_3D");

  /* Exactly fits, then one byte short. */
  size_t size = strlen(path) + 1;
  assert_int_equal(
      az_iot_su_delta_handler_get_source_cache_path(
          &handler,
          AZ_SPAN_FROM_STR(PROVIDER),
          AZ_SPAN_FROM_STR("SHA.256"),
          AZ_SPAN_FROM_STR("+/=="),
          path,
          size),
      AZ_IOT_OK);
  assert_int_equal(
      az_iot_su_delta_handler_get_source_cache_path(
          &handler,
          AZ_SPAN_FROM_STR(PROVIDER),
          AZ_SPAN_FROM_STR("SHA.256"),
          AZ_SPAN_FROM_STR("+/=="),
          path,
          size - 1),
      AZ_IOT_ERR_NOT_ENOUGH_SPACE);
  assert_int_equal(
      az_iot_su_delta_handler_get_source_cache_path(
          &handler,
          AZ_SPAN_FROM_STR(PROVIDER),
          AZ_SPAN_FROM_STR("sha256"),
          AZ_SPAN_FROM_STR("AAAA"),
          path,
          10),
      AZ_IOT_ERR_NOT_ENOUGH_SPACE);

  /* Not base64, empty, too long or NULL. */
  const az_span invalid_hashes[]
      = { AZ_SPAN_FROM_STR("ab.c"), AZ_SPAN_FROM_STR("a/b%2"), AZ_SPAN_EMPTY };
  for (size_t i = 0; i < sizeof(invalid_hashes) / sizeof(invalid_hashes[0]); i++)
  {
    assert_int_equal(
        az_iot_su_delta_handler_get_source_cache_path(
            &handler,
            AZ_SPAN_FROM_STR(PROVIDER),
            AZ_SPAN_FROM_STR("sha256"),
            invalid_hashes[i],
            path,
            sizeof(path)),
        AZ_IOT_ERR_INVALID_ARG);
  }
  char provider[AZ_IOT_SU_DELTA_MAX_PROVIDER_SIZE + 1];
  memset(provider, 'p', sizeof(provider));
  assert_int_equal(
      az_iot_su_delta_handler_get_source_cache_path(
          &handler,
          az_span_create((uint8_t*)provider, AZ_IOT_SU_DELTA_MAX_PROVIDER_SIZE - 1),
          AZ_SPAN_FROM_STR("sha256"),
          AZ_SPAN_FROM_STR("AA=="),
          path,
          sizeof(path)),
      AZ_IOT_OK);
  assert_int_equal(
      az_iot_su_delta_handler_get_source_cache_path(
          &handler,
          az_span_create((uint8_t*)provider, AZ_IOT_SU_DELTA_MAX_PROVIDER_SIZE),
          AZ_SPAN_FROM_STR("sha256"),
          AZ_SPAN_FROM_STR("AA=="),
          path,
          sizeof(path)),
      AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(
      az_iot_su_delta_handler_get_source_cache_path(
          &handler,
          AZ_SPAN_EMPTY,
          AZ_SPAN_FROM_STR("sha256"),
          AZ_SPAN_FROM_STR("AA=="),
          path,
          sizeof(path)),
      AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(
      az_iot_su_delta_handler_get_source_cache_path(
          &handler,
          AZ_SPAN_FROM_STR(PROVIDER),
          AZ_SPAN_EMPTY,
          AZ_SPAN_FROM_STR("AA=="),
          path,
          sizeof(path)),
      AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(
      az_iot_su_delta_handler_get_source_cache_path(
          NULL,
          AZ_SPAN_FROM_STR(PROVIDER),
          AZ_SPAN_FROM_STR("sha256"),
          AZ_SPAN_FROM_STR("AA=="),
          path,
          sizeof(path)),
      AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(
      az_iot_su_delta_handler_get_source_cache_path(
          &handler,
          AZ_SPAN_FROM_STR(PROVIDER),
          AZ_SPAN_FROM_STR("sha256"),
          AZ_SPAN_FROM_STR("AA=="),
          NULL,
          sizeof(path)),
      AZ_IOT_ERR_INVALID_ARG);
}

static void test_choose_picks_first_related_file_with_cached_source(void** state)
{
  (void)state;
  fake_processor fake = { 0 };
  az_iot_su_delta_handler handler;
  init_handler(&handler, &fake);
  az_iot_su_download_handler download_handler
      = az_iot_su_delta_handler_get_download_handler(&handler);
  seed_source();

  az_iot_su_client_update_manifest_file related_files[5];
  related_files[0] = make_file("no-properties", "AA==", 4, NULL);
  related_files[1] = make_file("not-json", "AA==", 4, "{\"microsoft.sourceFileHash\":");
  related_files[2]
      = make_file("no-algorithm", "AA==", 4, "{\"microsoft.sourceFileHash\":\"" SOURCE_HASH "\"}");
  related_files[3] = make_file("not-cached", "AA==", 4, SOURCE_PROPERTIES(OTHER_SOURCE_HASH));
  related_files[4] = make_file("cached", "AA==", 4, SOURCE_PROPERTIES(SOURCE_HASH));
  az_iot_su_client_update_manifest manifest;
  make_manifest(&manifest, related_files, 5);

  uint32_t chosen = 99;
  assert_int_equal(
      download_handler.choose_related_file_fn(
          &manifest, &manifest.files[0], &chosen, download_handler.user_ctx),
      AZ_IOT_SU_RESULT_SUCCESS);
  assert_int_equal(chosen, 4);

  /* Relative to the file's first related file. */
  manifest.files[0].related_files_index = 3;
  manifest.files[0].related_files_count = 2;
  assert_int_equal(
      download_handler.choose_related_file_fn(
          &manifest, &manifest.files[0], &chosen, download_handler.user_ctx),
      AZ_IOT_SU_RESULT_SUCCESS);
  assert_int_equal(chosen, 1);
}

static void test_choose_declines_without_cached_source(void** state)
{
  (void)state;
  fake_processor fake = { 0 };
  az_iot_su_delta_handler handler;
  init_handler(&handler, &fake);
  az_iot_su_download_handler download_handler
      = az_iot_su_delta_handler_get_download_handler(&handler);
  seed_source();

  az_iot_su_client_update_manifest_file related_files[2];
  related_files[0] = make_file("not-cached", "AA==", 4, SOURCE_PROPERTIES(OTHER_SOURCE_HASH));
  related_files[1] = make_file("cached", "AA==", 4, SOURCE_PROPERTIES(SOURCE_HASH));
  az_iot_su_client_update_manifest manifest;
  make_manifest(&manifest, related_files, 1);
  uint32_t chosen = 99;
  assert_int_equal(
      download_handler.choose_related_file_fn(
          &manifest, &manifest.files[0], &chosen, download_handler.user_ctx),
      AZ_IOT_SU_RESULT_FAILURE);

  /* Related files past the manifest's (left out for lack of storage) are not read. */
  manifest.files[0].related_files_count = 2;
  assert_int_equal(
      download_handler.choose_related_file_fn(
          &manifest, &manifest.files[0], &chosen, download_handler.user_ctx),
      AZ_IOT_SU_RESULT_FAILURE);
  manifest.related_files = NULL;
  manifest.related_files_count = 0;
  assert_int_equal(
      download_handler.choose_related_file_fn(
          &manifest, &manifest.files[0], &chosen, download_handler.user_ctx),
      AZ_IOT_SU_RESULT_FAILURE);

  /* A directory is not a source. */
  make_manifest(&manifest, related_files, 2);
  assert_int_equal(unlink(SOURCE_CACHE_FILE), 0);
  assert_int_equal(mkdir(SOURCE_CACHE_FILE, 0700), 0);
  assert_int_equal(
      download_handler.choose_related_file_fn(
          &manifest, &manifest.files[0], &chosen, download_handler.user_ctx),
      AZ_IOT_SU_RESULT_FAILURE);

  /* No provider, or arguments missing. */
  manifest.update_id.provider = AZ_SPAN_EMPTY;
  assert_int_equal(
      download_handler.choose_related_file_fn(
          &manifest, &manifest.files[0], &chosen, download_handler.user_ctx),
      AZ_IOT_SU_RESULT_FAILURE);
  assert_int_equal(
      download_handler.choose_related_file_fn(&manifest, &manifest.files[0], &chosen, NULL),
      AZ_IOT_SU_RESULT_FAILURE);
  assert_int_equal(
      download_handler.choose_related_file_fn(
          &manifest, &manifest.files[0], NULL, download_handler.user_ctx),
      AZ_IOT_SU_RESULT_FAILURE);
  assert_int_equal(fake.call_count, 0);
}

static void test_choose_declines_when_library_cannot_be_loaded(void** state)
{
  (void)state;
  seed_source();
  az_iot_su_client_update_manifest_file related_files[1];
  related_files[0] = make_file("cached", "AA==", 4, SOURCE_PROPERTIES(SOURCE_HASH));
  az_iot_su_client_update_manifest manifest;
  make_manifest(&manifest, related_files, 1);

  /* Missing, then present without the delta apply functions (the C library). */
  const char* const libraries[] = { WORK_DIRECTORY "/missing/libadudiffapi.so", "libc.so.6" };
  for (size_t i = 0; i < sizeof(libraries) / sizeof(libraries[0]); i++)
  {
    az_iot_su_delta_handler_options options = az_iot_su_delta_handler_options_default();
    options.source_cache_directory = CACHE_DIRECTORY;
    options.get_file_path_fn = get_file_path;
    options.library_path = libraries[i];
    az_iot_su_delta_handler handler;
    assert_int_equal(az_iot_su_delta_handler_init(&handler, &options), AZ_IOT_OK);
    az_iot_su_download_handler download_handler
        = az_iot_su_delta_handler_get_download_handler(&handler);
    uint32_t chosen = 99;
    assert_int_equal(
        download_handler.choose_related_file_fn(
            &manifest, &manifest.files[0], &chosen, download_handler.user_ctx),
        AZ_IOT_SU_RESULT_FAILURE);
    assert_int_equal(chosen, 99);
    az_iot_su_delta_handler_deinit(&handler);
  }
}

/* Chooses the cached related file, as the client does before building. */
static void choose(
    const az_iot_su_download_handler* download_handler,
    az_iot_su_client_update_manifest* manifest)
{
  uint32_t chosen = 99;
  assert_int_equal(
      download_handler->choose_related_file_fn(
          manifest, &manifest->files[0], &chosen, download_handler->user_ctx),
      AZ_IOT_SU_RESULT_SUCCESS);
  assert_int_equal(chosen, 0);
}

static void test_build_applies_delta_to_cached_source(void** state)
{
  (void)state;
  fake_processor fake = { 0 };
  az_iot_su_delta_handler handler;
  init_handler(&handler, &fake);
  az_iot_su_download_handler download_handler
      = az_iot_su_delta_handler_get_download_handler(&handler);
  seed_source();
  az_iot_su_client_update_manifest_file related_files[1];
  related_files[0] = make_file("delta", "AA==", 5, SOURCE_PROPERTIES(SOURCE_HASH));
  az_iot_su_client_update_manifest manifest;
  make_manifest(&manifest, related_files, 1);
  choose(&download_handler, &manifest);

  /* The downloaded delta, and a stale target from an interrupted attempt. */
  write_file(FILES_DIRECTORY "/3-delta", "DELTA", 5);
  write_file(FILES_DIRECTORY "/3-target", "STALE", 5);
  assert_int_equal(
      download_handler.build_file_from_related_fn(
          &manifest.files[0], &related_files[0], 3, download_handler.user_ctx),
      AZ_IOT_SU_RESULT_SUCCESS);
  assert_int_equal(fake.call_count, 1);
  assert_false(fake.target_existed[0]);
  assert_string_equal(fake.source_path, SOURCE_CACHE_FILE);
  assert_string_equal(fake.delta_path, FILES_DIRECTORY "/3-delta");
  assert_string_equal(fake.target_path, FILES_DIRECTORY "/3-target");
  char target[32] = { 0 };
  assert_int_equal(read_file(FILES_DIRECTORY "/3-target", target, sizeof(target)), 11);
  assert_string_equal(target, "SOURCEDELTA");
}

static void test_build_continues_in_progress_apply(void** state)
{
  (void)state;
  fake_processor fake = { 0 };
  fake.results[0] = AZ_IOT_SU_RESULT_IN_PROGRESS;
  fake.write_partial_target = true;
  az_iot_su_delta_handler handler;
  init_handler(&handler, &fake);
  az_iot_su_download_handler download_handler
      = az_iot_su_delta_handler_get_download_handler(&handler);
  seed_source();
  az_iot_su_client_update_manifest_file related_files[1];
  related_files[0] = make_file("delta", "AA==", 5, SOURCE_PROPERTIES(SOURCE_HASH));
  az_iot_su_client_update_manifest manifest;
  make_manifest(&manifest, related_files, 1);
  choose(&download_handler, &manifest);
  write_file(FILES_DIRECTORY "/0-delta", "DELTA", 5);

  assert_int_equal(
      download_handler.build_file_from_related_fn(
          &manifest.files[0], &related_files[0], 0, download_handler.user_ctx),
      AZ_IOT_SU_RESULT_IN_PROGRESS);
  /* The second call keeps the target the first one started. */
  assert_int_equal(
      download_handler.build_file_from_related_fn(
          &manifest.files[0], &related_files[0], 0, download_handler.user_ctx),
      AZ_IOT_SU_RESULT_SUCCESS);
  assert_int_equal(fake.call_count, 2);
  assert_false(fake.target_existed[0]);
  assert_true(fake.target_existed[1]);
}

static void test_build_failure_removes_partial_target(void** state)
{
  (void)state;
  fake_processor fake = { 0 };
  fake.results[0] = AZ_IOT_SU_RESULT_IN_PROGRESS;
  fake.results[1] = AZ_IOT_SU_RESULT_FAILURE;
  fake.write_partial_target = true;
  az_iot_su_delta_handler handler;
  init_handler(&handler, &fake);
  az_iot_su_download_handler download_handler
      = az_iot_su_delta_handler_get_download_handler(&handler);
  seed_source();
  az_iot_su_client_update_manifest_file related_files[1];
  related_files[0] = make_file("delta", "AA==", 5, SOURCE_PROPERTIES(SOURCE_HASH));
  az_iot_su_client_update_manifest manifest;
  make_manifest(&manifest, related_files, 1);
  choose(&download_handler, &manifest);
  write_file(FILES_DIRECTORY "/0-delta", "DELTA", 5);

  assert_int_equal(
      download_handler.build_file_from_related_fn(
          &manifest.files[0], &related_files[0], 0, download_handler.user_ctx),
      AZ_IOT_SU_RESULT_IN_PROGRESS);
  assert_int_equal(
      download_handler.build_file_from_related_fn(
          &manifest.files[0], &related_files[0], 0, download_handler.user_ctx),
      AZ_IOT_SU_RESULT_FAILURE);
  assert_false(file_exists(FILES_DIRECTORY "/0-target"));
  assert_true(file_exists(FILES_DIRECTORY "/0-delta"));
  assert_true(file_exists(SOURCE_CACHE_FILE));
}

static void test_build_fails_without_source_properties(void** state)
{
  (void)state;
  fake_processor fake = { 0 };
  az_iot_su_delta_handler handler;
  init_handler(&handler, &fake);
  az_iot_su_download_handler download_handler
      = az_iot_su_delta_handler_get_download_handler(&handler);
  seed_source();
  az_iot_su_client_update_manifest_file related_files[1];
  related_files[0] = make_file("delta", "AA==", 5, SOURCE_PROPERTIES(SOURCE_HASH));
  az_iot_su_client_update_manifest manifest;
  make_manifest(&manifest, related_files, 1);
  choose(&download_handler, &manifest);

  az_iot_su_client_update_manifest_file no_properties = make_file("delta", "AA==", 5, "{}");
  assert_int_equal(
      download_handler.build_file_from_related_fn(
          &manifest.files[0], &no_properties, 0, download_handler.user_ctx),
      AZ_IOT_SU_RESULT_FAILURE);
  assert_int_equal(
      download_handler.build_file_from_related_fn(
          &manifest.files[0], NULL, 0, download_handler.user_ctx),
      AZ_IOT_SU_RESULT_FAILURE);
  assert_int_equal(
      download_handler.build_file_from_related_fn(&manifest.files[0], &related_files[0], 0, NULL),
      AZ_IOT_SU_RESULT_FAILURE);
  assert_int_equal(fake.call_count, 0);
}

static void test_workflow_succeeded_keeps_latest_update_per_provider(void** state)
{
  (void)state;
  fake_processor fake = { 0 };
  az_iot_su_delta_handler handler;
  init_handler(&handler, &fake);
  az_iot_su_download_handler download_handler
      = az_iot_su_delta_handler_get_download_handler(&handler);

  /* An earlier update's source, another provider's, and a leftover temporary file. */
  seed_source();
  write_file(PROVIDER_DIRECTORY "/sha256-old.tmp", "TMP", 3);
  assert_int_equal(mkdir(CACHE_DIRECTORY "/Other", 0700), 0);
  write_file(CACHE_DIRECTORY "/Other/sha256-AA_3D_3D", "OTHER", 5);

  az_iot_su_client_update_manifest manifest;
  make_manifest(&manifest, NULL, 0);
  manifest.files[0] = make_file("target", TARGET_HASH, 6, NULL);
#if AZ_IOT_SU_MAX_FILE_COUNT > 1
  manifest.files[1] = make_file("no-hash", NULL, 2, NULL);
  manifest.files_count = 2;
  write_file(FILES_DIRECTORY "/1-no-hash", "NO", 2);
#endif
  list_files_in_one_step(&manifest);
  write_file(FILES_DIRECTORY "/0-target", "TARGET", 6);

  download_handler.workflow_succeeded_fn(&manifest, download_handler.user_ctx);

  char content[16] = { 0 };
  assert_int_equal(read_file(TARGET_CACHE_FILE, content, sizeof(content)), 6);
  assert_string_equal(content, "TARGET");
  assert_false(file_exists(FILES_DIRECTORY "/0-target"));
  assert_false(file_exists(SOURCE_CACHE_FILE));
  assert_false(file_exists(PROVIDER_DIRECTORY "/sha256-old.tmp"));
  assert_true(file_exists(CACHE_DIRECTORY "/Other/sha256-AA_3D_3D"));
#if AZ_IOT_SU_MAX_FILE_COUNT > 1
  assert_true(file_exists(FILES_DIRECTORY "/1-no-hash"));
#endif

  /* Again, e.g. after a reboot: the cached file stays, the application's copy is left. */
  write_file(FILES_DIRECTORY "/0-target", "TARGET", 6);
  download_handler.workflow_succeeded_fn(&manifest, download_handler.user_ctx);
  assert_true(file_exists(TARGET_CACHE_FILE));
  assert_true(file_exists(FILES_DIRECTORY "/0-target"));
}

static void test_workflow_succeeded_keeps_cache_when_nothing_stored(void** state)
{
  (void)state;
  fake_processor fake = { 0 };
  az_iot_su_delta_handler handler;
  init_handler(&handler, &fake);
  az_iot_su_download_handler download_handler
      = az_iot_su_delta_handler_get_download_handler(&handler);
  seed_source();

  /* The update's file is gone from the application's storage. */
  az_iot_su_client_update_manifest manifest;
  make_manifest(&manifest, NULL, 0);
  download_handler.workflow_succeeded_fn(&manifest, download_handler.user_ctx);
  assert_true(file_exists(SOURCE_CACHE_FILE));
  assert_false(file_exists(TARGET_CACHE_FILE));

  /* No provider, a cache directory that cannot be created, or arguments missing. */
  write_file(FILES_DIRECTORY "/0-target", "TARGET", 6);
  manifest.update_id.provider = AZ_SPAN_EMPTY;
  download_handler.workflow_succeeded_fn(&manifest, download_handler.user_ctx);
  download_handler.workflow_succeeded_fn(NULL, download_handler.user_ctx);
  download_handler.workflow_succeeded_fn(&manifest, NULL);
  manifest.update_id.provider = AZ_SPAN_FROM_STR(PROVIDER);

  az_iot_su_delta_handler_options options = az_iot_su_delta_handler_options_default();
  options.source_cache_directory = FILES_DIRECTORY "/0-target/cache";
  options.get_file_path_fn = get_file_path;
  az_iot_su_delta_handler blocked;
  assert_int_equal(az_iot_su_delta_handler_init(&blocked, &options), AZ_IOT_OK);
  az_iot_su_download_handler blocked_handler
      = az_iot_su_delta_handler_get_download_handler(&blocked);
  blocked_handler.workflow_succeeded_fn(&manifest, blocked_handler.user_ctx);
  assert_true(file_exists(FILES_DIRECTORY "/0-target"));
  assert_true(file_exists(SOURCE_CACHE_FILE));
}

/* The full cycle: the first update seeds the cache, the next one is built from it. */
static void test_succeeded_update_is_source_of_next(void** state)
{
  (void)state;
  fake_processor fake = { 0 };
  az_iot_su_delta_handler handler;
  init_handler(&handler, &fake);
  az_iot_su_download_handler download_handler
      = az_iot_su_delta_handler_get_download_handler(&handler);

  az_iot_su_client_update_manifest first;
  make_manifest(&first, NULL, 0);
  first.files[0] = make_file("image", SOURCE_HASH, 6, NULL);
  list_files_in_one_step(&first);
  write_file(FILES_DIRECTORY "/0-image", "SOURCE", 6);
  download_handler.workflow_succeeded_fn(&first, download_handler.user_ctx);
  assert_true(file_exists(SOURCE_CACHE_FILE));

  az_iot_su_client_update_manifest_file related_files[1];
  related_files[0] = make_file("delta", "AA==", 5, SOURCE_PROPERTIES(SOURCE_HASH));
  az_iot_su_client_update_manifest second;
  make_manifest(&second, related_files, 1);
  choose(&download_handler, &second);
  write_file(FILES_DIRECTORY "/0-delta", "DELTA", 5);
  assert_int_equal(
      download_handler.build_file_from_related_fn(
          &second.files[0], &related_files[0], 0, download_handler.user_ctx),
      AZ_IOT_SU_RESULT_SUCCESS);
  second.files[0].size_in_bytes = 11;
  download_handler.workflow_succeeded_fn(&second, download_handler.user_ctx);
  assert_true(file_exists(TARGET_CACHE_FILE));
  assert_false(file_exists(SOURCE_CACHE_FILE));
}

/* A file listing SHA-512 before SHA-256 is cached, and kept, under the SHA-256 the client
 * verified. */
static void test_workflow_succeeded_caches_under_sha256_in_any_position(void** state)
{
  (void)state;
  fake_processor fake = { 0 };
  az_iot_su_delta_handler handler;
  init_handler(&handler, &fake);
  az_iot_su_download_handler download_handler
      = az_iot_su_delta_handler_get_download_handler(&handler);

  az_iot_su_client_update_manifest manifest;
  make_manifest(&manifest, NULL, 0);
  manifest.files[0].hashes[1] = manifest.files[0].hashes[0];
  manifest.files[0].hashes[0].hash_type = AZ_SPAN_FROM_STR("sha512");
  manifest.files[0].hashes[0].hash_value = AZ_SPAN_FROM_STR("AA==");
  manifest.files[0].hashes_count = 2;
  write_file(FILES_DIRECTORY "/0-target", "TARGET", 6);

  download_handler.workflow_succeeded_fn(&manifest, download_handler.user_ctx);
  assert_true(file_exists(TARGET_CACHE_FILE));
  assert_false(file_exists(PROVIDER_DIRECTORY "/sha512-AA_3D_3D"));

  /* The next success of the same update keeps that entry. */
  download_handler.workflow_succeeded_fn(&manifest, download_handler.user_ctx);
  assert_true(file_exists(TARGET_CACHE_FILE));
}

#if AZ_IOT_SU_MAX_FILE_COUNT > 1 && AZ_IOT_SU_MAX_STEP_COUNT > 1
/* Files are found where download_fn stored them: at their slot in their step, which differs
 * from their manifest index when files are reordered or in several steps. */
static void test_workflow_succeeded_finds_files_by_step_slot(void** state)
{
  (void)state;
  fake_processor fake = { 0 };
  az_iot_su_delta_handler handler;
  init_handler(&handler, &fake);
  az_iot_su_download_handler download_handler
      = az_iot_su_delta_handler_get_download_handler(&handler);

  az_iot_su_client_update_manifest manifest;
  make_manifest(&manifest, NULL, 0);
  manifest.files[1] = manifest.files[0];
  manifest.files[0] = make_file("script", OTHER_SOURCE_HASH, 2, NULL);
  manifest.files_count = 2;
  manifest.instructions.steps_count = 2;
  manifest.instructions.steps[0].files[0] = manifest.files[0].id;
  manifest.instructions.steps[0].files_count = 1;
  manifest.instructions.steps[1].files[0] = manifest.files[1].id;
  manifest.instructions.steps[1].files_count = 1;
  write_file(FILES_DIRECTORY "/0-target", "TARGET", 6);

  download_handler.workflow_succeeded_fn(&manifest, download_handler.user_ctx);
  assert_true(file_exists(TARGET_CACHE_FILE));
  assert_false(file_exists(FILES_DIRECTORY "/0-target"));
}
#endif

/* Copying, used when the cache is on another file system than the application's files. */
static void test_copy_file_keeps_source(void** state)
{
  (void)state;
  static char data[10000];
  for (size_t i = 0; i < sizeof(data); i++)
  {
    data[i] = (char)('a' + (i % 26));
  }
  write_file(FILES_DIRECTORY "/source", data, sizeof(data));

  assert_true(az_iot_su_delta__copy_file(FILES_DIRECTORY "/source", WORK_DIRECTORY "/target"));

  static char copied[sizeof(data) + 1];
  assert_int_equal(read_file(WORK_DIRECTORY "/target", copied, sizeof(copied)), sizeof(data));
  assert_memory_equal(copied, data, sizeof(data));
  assert_true(file_exists(FILES_DIRECTORY "/source"));
  assert_false(file_exists(WORK_DIRECTORY "/target.tmp"));
}

/* A failed copy keeps the source and leaves no temporary file. */
static void test_copy_file_failures_remove_temporary_target(void** state)
{
  (void)state;
  write_file(FILES_DIRECTORY "/source", "SOURCE", 6);

  /* The temporary path does not fit. */
  char long_target[AZ_IOT_SU_DELTA_MAX_PATH_SIZE];
  memset(long_target, 'a', sizeof(long_target));
  memcpy(long_target, WORK_DIRECTORY "/", sizeof(WORK_DIRECTORY));
  long_target[sizeof(long_target) - 4] = '\0';
  assert_false(az_iot_su_delta__copy_file(FILES_DIRECTORY "/source", long_target));

  /* Reading fails after the temporary target is opened: the source is a directory. */
  assert_false(az_iot_su_delta__copy_file(FILES_DIRECTORY, WORK_DIRECTORY "/target"));
  assert_false(file_exists(WORK_DIRECTORY "/target.tmp"));
  assert_false(file_exists(WORK_DIRECTORY "/target"));

  /* Renaming fails after the copy: the target is a directory that is not empty. */
  assert_int_equal(mkdir(WORK_DIRECTORY "/occupied", 0700), 0);
  write_file(WORK_DIRECTORY "/occupied/file", "X", 1);
  assert_false(az_iot_su_delta__copy_file(FILES_DIRECTORY "/source", WORK_DIRECTORY "/occupied"));
  assert_false(file_exists(WORK_DIRECTORY "/occupied.tmp"));
  assert_true(file_exists(WORK_DIRECTORY "/occupied/file"));

  assert_true(file_exists(FILES_DIRECTORY "/source"));
}

/* A file or link planted at the temporary path is replaced, never followed. */
static void test_copy_file_does_not_follow_a_planted_link(void** state)
{
  (void)state;
  write_file(FILES_DIRECTORY "/source", "SOURCE", 6);
  write_file(WORK_DIRECTORY "/outside", "OUTSIDE", 7);
  assert_int_equal(symlink(WORK_DIRECTORY "/outside", WORK_DIRECTORY "/target.tmp"), 0);

  assert_true(az_iot_su_delta__copy_file(FILES_DIRECTORY "/source", WORK_DIRECTORY "/target"));

  char content[16] = { 0 };
  assert_int_equal(read_file(WORK_DIRECTORY "/outside", content, sizeof(content)), 7);
  assert_string_equal(content, "OUTSIDE");
  struct stat info;
  assert_int_equal(lstat(WORK_DIRECTORY "/target", &info), 0);
  assert_true(S_ISREG(info.st_mode));
  memset(content, 0, sizeof(content));
  assert_int_equal(read_file(WORK_DIRECTORY "/target", content, sizeof(content)), 6);
  assert_string_equal(content, "SOURCE");
  assert_int_equal(lstat(WORK_DIRECTORY "/target.tmp", &info), -1);
}

/* A step may name its file with escapes ("t\u0061rget" for "target"): the file is still found
 * at its slot and cached. */
static void test_workflow_succeeded_finds_escaped_step_file_ids(void** state)
{
  (void)state;
  fake_processor fake = { 0 };
  az_iot_su_delta_handler handler;
  init_handler(&handler, &fake);
  az_iot_su_download_handler download_handler
      = az_iot_su_delta_handler_get_download_handler(&handler);

  az_iot_su_client_update_manifest manifest;
  make_manifest(&manifest, NULL, 0);
  manifest.instructions.steps[0].files[0] = AZ_SPAN_FROM_STR("t\\u0061rget");
  write_file(FILES_DIRECTORY "/0-target", "TARGET", 6);

  download_handler.workflow_succeeded_fn(&manifest, download_handler.user_ctx);
  assert_true(file_exists(TARGET_CACHE_FILE));
  assert_false(file_exists(FILES_DIRECTORY "/0-target"));
}

/* Writes the cached source under the provider directory @p directory_name. */
static void seed_source_of_provider(const char* directory_name)
{
  char path[AZ_IOT_SU_DELTA_MAX_PATH_SIZE];
  int written = snprintf(path, sizeof(path), CACHE_DIRECTORY "/%s", directory_name);
  assert_true(written > 0 && (size_t)written < sizeof(path));
  assert_int_equal(mkdir(path, 0700), 0);
  written = snprintf(
      path,
      sizeof(path),
      CACHE_DIRECTORY "/%s/sha256-nuDNVz6kcEmOtefngw1gEGmcGB5L5ZRaBB0qbMbe90E_3D",
      directory_name);
  assert_true(written > 0 && (size_t)written < sizeof(path));
  write_file(path, "SOURCE", 6);
}

/* The provider is decoded before it names the cache directory: an update stored under
 * "Contoso\u002eLtd" is the source of one from "Contoso.Ltd" or another spelling, and the
 * application finds it under the decoded name. */
static void test_provider_spellings_share_a_cache_directory(void** state)
{
  (void)state;
  fake_processor fake = { 0 };
  az_iot_su_delta_handler handler;
  init_handler(&handler, &fake);
  az_iot_su_download_handler download_handler
      = az_iot_su_delta_handler_get_download_handler(&handler);

  az_iot_su_client_update_manifest first;
  make_manifest(&first, NULL, 0);
  first.update_id.provider = AZ_SPAN_FROM_STR("Contoso\\u002eLtd");
  first.files[0] = make_file("image", SOURCE_HASH, 6, NULL);
  list_files_in_one_step(&first);
  write_file(FILES_DIRECTORY "/0-image", "SOURCE", 6);
  download_handler.workflow_succeeded_fn(&first, download_handler.user_ctx);
  assert_true(file_exists(SOURCE_CACHE_FILE));

  char path[AZ_IOT_SU_DELTA_MAX_PATH_SIZE];
  assert_int_equal(
      az_iot_su_delta_handler_get_source_cache_path(
          &handler,
          AZ_SPAN_FROM_STR(PROVIDER),
          AZ_SPAN_FROM_STR("sha256"),
          AZ_SPAN_FROM_STR(SOURCE_HASH),
          path,
          sizeof(path)),
      AZ_IOT_OK);
  assert_string_equal(path, SOURCE_CACHE_FILE);

  az_iot_su_client_update_manifest_file related_files[1];
  related_files[0] = make_file("delta", "AA==", 5, SOURCE_PROPERTIES(SOURCE_HASH));
  az_iot_su_client_update_manifest second;
  make_manifest(&second, related_files, 1);
  choose(&download_handler, &second);
  second.update_id.provider = AZ_SPAN_FROM_STR("Contoso\\u002E\\u004ctd");
  choose(&download_handler, &second);
}

/* The provider length limit applies to the decoded name; a malformed escape names nothing. */
static void test_provider_length_limit_applies_to_decoded_name(void** state)
{
  (void)state;
  fake_processor fake = { 0 };
  az_iot_su_delta_handler handler;
  init_handler(&handler, &fake);
  az_iot_su_download_handler download_handler
      = az_iot_su_delta_handler_get_download_handler(&handler);
  assert_int_equal(mkdir(CACHE_DIRECTORY, 0700), 0);

  /* 64 bytes decoded (84 escaped) fit; 65 do not. */
  const char longest[] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaabbbb";
  const char too_long[] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaabbbbc";
  assert_int_equal(sizeof(longest), AZ_IOT_SU_DELTA_MAX_PROVIDER_SIZE);
  seed_source_of_provider(longest);
  seed_source_of_provider(too_long);

  az_iot_su_client_update_manifest_file related_files[1];
  related_files[0] = make_file("delta", "AA==", 5, SOURCE_PROPERTIES(SOURCE_HASH));
  az_iot_su_client_update_manifest manifest;
  make_manifest(&manifest, related_files, 1);
  manifest.update_id.provider = AZ_SPAN_FROM_STR(
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\\u0062\\u0062\\u0062\\u0062");
  choose(&download_handler, &manifest);

  uint32_t chosen = 99;
  manifest.update_id.provider = AZ_SPAN_FROM_STR(
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\\u0062\\u0062\\u0062\\u0062c");
  assert_int_equal(
      download_handler.choose_related_file_fn(
          &manifest, &manifest.files[0], &chosen, download_handler.user_ctx),
      AZ_IOT_SU_RESULT_FAILURE);

  seed_source_of_provider("Contoso_x");
  manifest.update_id.provider = AZ_SPAN_FROM_STR("Contoso\\x");
  assert_int_equal(
      download_handler.choose_related_file_fn(
          &manifest, &manifest.files[0], &chosen, download_handler.user_ctx),
      AZ_IOT_SU_RESULT_FAILURE);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_options_default),
    cmocka_unit_test(test_init_rejects_invalid_arguments),
    cmocka_unit_test(test_get_download_handler),
    cmocka_unit_test(test_source_cache_path_encoding),
    cmocka_unit_test_setup_teardown(
        test_choose_picks_first_related_file_with_cached_source,
        setup_work_directory,
        teardown_work_directory),
    cmocka_unit_test_setup_teardown(
        test_choose_declines_without_cached_source, setup_work_directory, teardown_work_directory),
    cmocka_unit_test_setup_teardown(
        test_choose_declines_when_library_cannot_be_loaded,
        setup_work_directory,
        teardown_work_directory),
    cmocka_unit_test_setup_teardown(
        test_build_applies_delta_to_cached_source, setup_work_directory, teardown_work_directory),
    cmocka_unit_test_setup_teardown(
        test_build_continues_in_progress_apply, setup_work_directory, teardown_work_directory),
    cmocka_unit_test_setup_teardown(
        test_build_failure_removes_partial_target, setup_work_directory, teardown_work_directory),
    cmocka_unit_test_setup_teardown(
        test_build_fails_without_source_properties, setup_work_directory, teardown_work_directory),
    cmocka_unit_test_setup_teardown(
        test_workflow_succeeded_keeps_latest_update_per_provider,
        setup_work_directory,
        teardown_work_directory),
    cmocka_unit_test_setup_teardown(
        test_workflow_succeeded_keeps_cache_when_nothing_stored,
        setup_work_directory,
        teardown_work_directory),
    cmocka_unit_test_setup_teardown(
        test_succeeded_update_is_source_of_next, setup_work_directory, teardown_work_directory),
    cmocka_unit_test_setup_teardown(
        test_workflow_succeeded_caches_under_sha256_in_any_position,
        setup_work_directory,
        teardown_work_directory),
#if AZ_IOT_SU_MAX_FILE_COUNT > 1 && AZ_IOT_SU_MAX_STEP_COUNT > 1
    cmocka_unit_test_setup_teardown(
        test_workflow_succeeded_finds_files_by_step_slot,
        setup_work_directory,
        teardown_work_directory),
#endif
    cmocka_unit_test_setup_teardown(
        test_copy_file_keeps_source, setup_work_directory, teardown_work_directory),
    cmocka_unit_test_setup_teardown(
        test_copy_file_failures_remove_temporary_target,
        setup_work_directory,
        teardown_work_directory),
    cmocka_unit_test_setup_teardown(
        test_copy_file_does_not_follow_a_planted_link,
        setup_work_directory,
        teardown_work_directory),
    cmocka_unit_test_setup_teardown(
        test_workflow_succeeded_finds_escaped_step_file_ids,
        setup_work_directory,
        teardown_work_directory),
    cmocka_unit_test_setup_teardown(
        test_provider_spellings_share_a_cache_directory,
        setup_work_directory,
        teardown_work_directory),
    cmocka_unit_test_setup_teardown(
        test_provider_length_limit_applies_to_decoded_name,
        setup_work_directory,
        teardown_work_directory),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}

#else /* AZ_IOT_TEST_SU_DELTA_LIBRARY */

#define VECTORS_DIRECTORY AZ_IOT_TEST_SU_DELTA_VECTORS_DIRECTORY

/* SHA-256 of target.swu (tests/unit/data/su_delta/SHA256SUMS). */
static const uint8_t target_sha256[AZ_IOT_SHA256_SIZE]
    = { 0x4a, 0x04, 0x0f, 0x8d, 0x49, 0x07, 0xfd, 0x9e, 0xb7, 0x62, 0xba,
        0x42, 0x15, 0xa8, 0x00, 0x1f, 0xf1, 0xf7, 0x65, 0x28, 0xfc, 0xb6,
        0xdf, 0xc9, 0x62, 0x67, 0x79, 0x95, 0x26, 0x63, 0x75, 0xcc };

/* Copies the first @p max_size bytes of a vector. */
static void copy_vector(const char* name, const char* path, size_t max_size)
{
  static uint8_t data[8192];
  char source[512];
  (void)snprintf(source, sizeof(source), VECTORS_DIRECTORY "/%s", name);
  long size = read_file(source, data, sizeof(data));
  assert_true(size > 0);
  write_file(path, data, (size_t)size < max_size ? (size_t)size : max_size);
}

static void sha256_file(const char* path, uint8_t digest[AZ_IOT_SHA256_SIZE])
{
  static uint8_t data[8192];
  long size = read_file(path, data, sizeof(data));
  assert_true(size > 0);
  const az_iot_crypto* crypto = az_iot_crypto_openssl();
  az_iot_sha256_ctx ctx;
  assert_int_equal(crypto->sha256_init(crypto, &ctx), AZ_IOT_OK);
  assert_int_equal(crypto->sha256_update(crypto, &ctx, data, (size_t)size), AZ_IOT_OK);
  assert_int_equal(crypto->sha256_final(crypto, &ctx, digest), AZ_IOT_OK);
}

static void init_library_handler(az_iot_su_delta_handler* handler)
{
  az_iot_su_delta_handler_options options = az_iot_su_delta_handler_options_default();
  options.source_cache_directory = CACHE_DIRECTORY;
  options.get_file_path_fn = get_file_path;
  options.library_path = AZ_IOT_TEST_SU_DELTA_LIBRARY;
  assert_int_equal(az_iot_su_delta_handler_init(handler, &options), AZ_IOT_OK);
}

static void test_library_rebuilds_target(void** state)
{
  (void)state;
  az_iot_su_delta_handler handler;
  init_library_handler(&handler);
  az_iot_su_download_handler download_handler
      = az_iot_su_delta_handler_get_download_handler(&handler);
  seed_source();
  copy_vector("source.swu", SOURCE_CACHE_FILE, SIZE_MAX);
  copy_vector("delta.bin", FILES_DIRECTORY "/0-delta", SIZE_MAX);

  az_iot_su_client_update_manifest_file related_files[1];
  related_files[0] = make_file("delta", "AA==", 2212, SOURCE_PROPERTIES(SOURCE_HASH));
  az_iot_su_client_update_manifest manifest;
  make_manifest(&manifest, related_files, 1);
  uint32_t chosen = 99;
  /* Fails when the library does not load: the configured library is required. */
  assert_int_equal(
      download_handler.choose_related_file_fn(
          &manifest, &manifest.files[0], &chosen, download_handler.user_ctx),
      AZ_IOT_SU_RESULT_SUCCESS);
  assert_int_equal(chosen, 0);
  assert_int_equal(
      download_handler.build_file_from_related_fn(
          &manifest.files[0], &related_files[0], 0, download_handler.user_ctx),
      AZ_IOT_SU_RESULT_SUCCESS);

  uint8_t digest[AZ_IOT_SHA256_SIZE];
  sha256_file(FILES_DIRECTORY "/0-target", digest);
  assert_memory_equal(digest, target_sha256, sizeof(digest));
  az_iot_su_delta_handler_deinit(&handler);
}

static void test_library_failure_removes_partial_target(void** state)
{
  (void)state;
  az_iot_su_delta_handler handler;
  init_library_handler(&handler);
  az_iot_su_download_handler download_handler
      = az_iot_su_delta_handler_get_download_handler(&handler);
  seed_source();
  /* A truncated delta: the library fails, leaving an empty target behind. */
  copy_vector("source.swu", SOURCE_CACHE_FILE, SIZE_MAX);
  copy_vector("delta.bin", FILES_DIRECTORY "/0-delta", 1000);

  az_iot_su_client_update_manifest_file related_files[1];
  related_files[0] = make_file("delta", "AA==", 2212, SOURCE_PROPERTIES(SOURCE_HASH));
  az_iot_su_client_update_manifest manifest;
  make_manifest(&manifest, related_files, 1);
  uint32_t chosen = 99;
  assert_int_equal(
      download_handler.choose_related_file_fn(
          &manifest, &manifest.files[0], &chosen, download_handler.user_ctx),
      AZ_IOT_SU_RESULT_SUCCESS);
  assert_int_equal(
      download_handler.build_file_from_related_fn(
          &manifest.files[0], &related_files[0], 0, download_handler.user_ctx),
      AZ_IOT_SU_RESULT_FAILURE);
  assert_false(file_exists(FILES_DIRECTORY "/0-target"));

  /* A source removed after choosing. */
  assert_int_equal(unlink(SOURCE_CACHE_FILE), 0);
  assert_int_equal(
      download_handler.build_file_from_related_fn(
          &manifest.files[0], &related_files[0], 0, download_handler.user_ctx),
      AZ_IOT_SU_RESULT_FAILURE);
  assert_false(file_exists(FILES_DIRECTORY "/0-target"));
  az_iot_su_delta_handler_deinit(&handler);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test_setup_teardown(
        test_library_rebuilds_target, setup_work_directory, teardown_work_directory),
    cmocka_unit_test_setup_teardown(
        test_library_failure_removes_partial_target, setup_work_directory, teardown_work_directory),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}

#endif /* AZ_IOT_TEST_SU_DELTA_LIBRARY */
