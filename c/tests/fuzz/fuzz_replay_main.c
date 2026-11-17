// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.
/* SPDX-License-Identifier: MIT */

/**
 * @file fuzz_replay_main.c
 * @brief Runs a fuzz target once per input file, without libFuzzer.
 *
 * Builds without AZ_IOT_BUILD_FUZZERS link this instead of libFuzzer, so CTest replays the seed
 * corpus and every crash input added to it with any compiler and sanitizer.
 *
 * Usage: <target> <file>...  Exits nonzero if a file cannot be read.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

/** @brief Largest input file replayed. */
#define FUZZ_REPLAY_INPUT_MAX (1024 * 1024)

int main(int argc, char** argv)
{
  static uint8_t buffer[FUZZ_REPLAY_INPUT_MAX];
  for (int i = 1; i < argc; ++i)
  {
    FILE* file = fopen(argv[i], "rb");
    if (file == NULL)
    {
      fprintf(stderr, "cannot open %s\n", argv[i]);
      return 1;
    }
    size_t size = fread(buffer, 1, sizeof(buffer), file);
    int failed = ferror(file);
    (void)fclose(file);
    if (failed)
    {
      fprintf(stderr, "cannot read %s\n", argv[i]);
      return 1;
    }
    printf("%s: %zu bytes\n", argv[i], size);
    (void)LLVMFuzzerTestOneInput(buffer, size);
  }
  return 0;
}
