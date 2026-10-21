// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/** @file race_annotations.h
 * @brief Describes the SDK's own locks to valgrind's helgrind and DRD.
 *
 * Those tools see pthread and Win32 locks, not a lock built on atomics, and
 * report every access it guards as a race. With AZ_IOT_RACE_ANNOTATIONS
 * defined these expand to valgrind client requests (understood by both tools,
 * no-ops outside valgrind); otherwise to nothing. */
#ifndef AZ_IOT_RACE_ANNOTATIONS_INTERNAL_H
#define AZ_IOT_RACE_ANNOTATIONS_INTERNAL_H

#if defined(AZ_IOT_RACE_ANNOTATIONS)
#include <valgrind/helgrind.h>

/** @brief @p lock now exists and is free. */
#define AZ_IOT_RACE_LOCK_CREATE(lock) ANNOTATE_RWLOCK_CREATE(lock)
/** @brief @p lock was acquired exclusively. */
#define AZ_IOT_RACE_LOCK_ACQUIRED(lock) ANNOTATE_RWLOCK_ACQUIRED(lock, 1)
/** @brief @p lock is about to be released. */
#define AZ_IOT_RACE_LOCK_RELEASED(lock) ANNOTATE_RWLOCK_RELEASED(lock, 1)
/** @brief @p lock is no longer used. */
#define AZ_IOT_RACE_LOCK_DESTROY(lock) ANNOTATE_RWLOCK_DESTROY(lock)
#else
#define AZ_IOT_RACE_LOCK_CREATE(lock) ((void)(lock))
#define AZ_IOT_RACE_LOCK_ACQUIRED(lock) ((void)(lock))
#define AZ_IOT_RACE_LOCK_RELEASED(lock) ((void)(lock))
#define AZ_IOT_RACE_LOCK_DESTROY(lock) ((void)(lock))
#endif

#endif /* AZ_IOT_RACE_ANNOTATIONS_INTERNAL_H */
