// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
#ifndef AZ_IOT_TEST_ENV_H
#define AZ_IOT_TEST_ENV_H

#include <stdbool.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/*
 * Reading configuration out of the environment, once.
 *
 * Every e2e and conformance main needs the same two things -- "give me this
 * variable, and treat empty as absent" -- and each had grown its own copy. They
 * were not quite identical, which is the usual reason duplicates are worth
 * removing: the differences are accidental rather than meaningful.
 *
 * Two forms, because both are genuinely needed:
 *
 *   az_iot_test_env()      borrows. No allocation, nothing to free, and the
 *                          pointer is valid for as long as the environment is
 *                          not modified -- which no test does.
 *
 *   az_iot_test_env_dup()  copies. Required where the value outlives the call
 *                          or is stored in a struct the test owns.
 *
 * The borrowing form cannot be used on Windows: getenv is deprecated under
 * MSVC and these targets build with /WX, and the supported replacements
 * (_dupenv_s, getenv_s) both hand back a copy. So the two forms are not simply
 * convenience wrappers around each other, and the split is deliberate.
 */

#ifdef _WIN32

/* Copy, or NULL when unset or empty. Caller frees. */
static inline char* az_iot_test_env_dup(const char* name)
{
  char* value = NULL;
  size_t len = 0;
  if (_dupenv_s(&value, &len, name) != 0 || value == NULL || value[0] == '\0')
  {
    free(value);
    return NULL;
  }
  return value;
}

#else

static inline char* az_iot_test_env_dup(const char* name)
{
  const char* value = getenv(name);
  if (value == NULL || value[0] == '\0')
  {
    return NULL;
  }
  size_t n = strlen(value) + 1;
  char* copy = (char*)malloc(n);
  if (copy != NULL)
  {
    memcpy(copy, value, n);
  }
  return copy;
}

/* Borrow, or NULL when unset or empty. Nothing to free.
 *
 * POSIX only, deliberately: see the note above. A test that must also build on
 * Windows uses az_iot_test_env_dup(). */
static inline const char* az_iot_test_env(const char* name)
{
  const char* value = getenv(name);
  return (value != NULL && value[0] != '\0') ? value : NULL;
}

#endif /* _WIN32 */

/** @brief Copies @p value to @p out; false when it does not fit in @p size bytes. */
static inline bool az_iot_test_copy_string(char* out, size_t size, const char* value)
{
  size_t const len = strlen(value);
  if (len >= size)
  {
    return false;
  }
  memcpy(out, value, len + 1);
  return true;
}

/**
 * @brief Writes the temporary directory to @p out: TMPDIR, TEMP or TMP, the first set, else
 * "/tmp" (POSIX) or "." (Windows).
 *
 * @details Allocates nothing, so suites that count allocations can call it.
 *
 * @return true when it fits in @p size bytes.
 */
static inline bool az_iot_test_temp_dir(char* out, size_t size)
{
  static const char* const names[] = { "TMPDIR", "TEMP", "TMP" };
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
  {
#ifdef _WIN32
    size_t needed = 0; // with the terminator; 0 when unset, 1 when empty
    errno_t const rc = getenv_s(&needed, out, size, names[i]);
    if (needed > 1)
    {
      return rc == 0;
    }
#else
    const char* value = az_iot_test_env(names[i]);
    if (value != NULL)
    {
      return az_iot_test_copy_string(out, size, value);
    }
#endif
  }
#ifdef _WIN32
  return az_iot_test_copy_string(out, size, ".");
#else
  return az_iot_test_copy_string(out, size, "/tmp");
#endif
}

/**
 * @brief Writes "<temporary directory>/<name>" to @p out (see az_iot_test_temp_dir()).
 *
 * @details Allocates nothing.
 *
 * @return true when the whole path fits in @p size bytes.
 */
static inline bool az_iot_test_temp_path(char* out, size_t size, const char* name)
{
  size_t len = 0;
  int written = 0;
  if (!az_iot_test_temp_dir(out, size))
  {
    return false;
  }
  len = strlen(out);
  written = snprintf(out + len, size - len, "/%s", name);
  return written > 0 && (size_t)written < size - len;
}

/**
 * @brief Multiplier for test wait budgets, from AZ_IOT_TEST_TIMEOUT_SCALE (1..100).
 *
 * Runs under a slow instrumentation (helgrind, DRD) set it so waits that are
 * generous natively do not expire. Unset, empty or out of range: 1. It only
 * lengthens waits; it never decides whether a test runs.
 */
static inline unsigned az_iot_test_timeout_scale(void)
{
  char* value = az_iot_test_env_dup("AZ_IOT_TEST_TIMEOUT_SCALE");
  unsigned scale = 1u;
  if (value != NULL)
  {
    char* end = NULL;
    unsigned long v = strtoul(value, &end, 10);
    if (end != value && *end == '\0' && v >= 1ul && v <= 100ul)
    {
      scale = (unsigned)v;
    }
    free(value);
  }
  return scale;
}

#endif /* AZ_IOT_TEST_ENV_H */
