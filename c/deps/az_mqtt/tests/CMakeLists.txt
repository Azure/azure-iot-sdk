# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

cmake_minimum_required(VERSION 3.14)

# ── Fetch cmocka via FetchContent ────────────────────────────
include(FetchContent)

FetchContent_Declare(
  cmocka
  GIT_REPOSITORY https://gitlab.com/cmocka/cmocka.git
  GIT_TAG        cmocka-1.1.7
)

# Force cmocka to build as a static library to avoid cyclic dependency issues
set(_saved_BUILD_SHARED_LIBS ${BUILD_SHARED_LIBS})
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(WITH_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WITH_STATIC_LIB ON CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(cmocka)

# Restore BUILD_SHARED_LIBS
set(BUILD_SHARED_LIBS ${_saved_BUILD_SHARED_LIBS} CACHE BOOL "" FORCE)

# ── Test subdirectories ──────────────────────────────────────

add_subdirectory(e2e)

# Tests against an in-process peer (POSIX + OpenSSL 3.0+); no broker needed.
if(UNIX)
  find_package(OpenSSL 3.0 QUIET)
  if(OpenSSL_FOUND)
    add_subdirectory(support)
    add_subdirectory(transport)
    add_subdirectory(client)
  else()
    message(STATUS "Skipping local tests: the in-process peer needs OpenSSL 3.0+")
  endif()
endif()
