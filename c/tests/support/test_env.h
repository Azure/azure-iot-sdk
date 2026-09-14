// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
#ifndef AZ_IOT_TEST_ENV_H
#define AZ_IOT_TEST_ENV_H

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

#endif /* AZ_IOT_TEST_ENV_H */
