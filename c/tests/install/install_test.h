// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/**
 * @file install_test.h
 * @brief Minimal assertion for the installed-package tests (no test framework
 * is installed with the SDK).
 */
#ifndef AZ_IOT_INSTALL_TEST_H
#define AZ_IOT_INSTALL_TEST_H

#include <stdio.h>
#include <stdlib.h>

/** @brief Exit with failure, naming @p expr and its location, unless @p ok. */
static void install_test_check(int ok, const char* expr, const char* file, int line)
{
  if (!ok)
  {
    fprintf(stderr, "%s:%d: check failed: %s\n", file, line, expr);
    exit(EXIT_FAILURE);
  }
}

#define CHECK(expr) install_test_check((expr) ? 1 : 0, #expr, __FILE__, __LINE__)

#endif /* AZ_IOT_INSTALL_TEST_H */
