// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */

/**
 * @file az_iot_connection_client_config.h
 * @brief Default az_iot_connection_client_limits, used for a limit left at 0 and by
 * AZ_IOT_CONNECTION_CLIENT_DEFAULT_WORKSPACE_SIZE. Each value can be defined at compile time.
 *
 * Give the SDK and the application the same values. A mismatch cannot corrupt memory:
 * az_iot_connection_client_init() checks the workspace size.
 */

#ifndef AZ_IOT_CONNECTION_CLIENT_CONFIG_H
#define AZ_IOT_CONNECTION_CLIENT_CONFIG_H

/**
 * @def AZ_IOT_CONNECTION_CLIENT_CONFIG_FILE
 * @brief Optional header, as written in a quoted #include, read first: it may define any
 * value below. Example: -DAZ_IOT_CONNECTION_CLIENT_CONFIG_FILE='"my_connection_config.h"'.
 */
#ifdef AZ_IOT_CONNECTION_CLIENT_CONFIG_FILE
#include AZ_IOT_CONNECTION_CLIENT_CONFIG_FILE
#endif

/** @brief Default az_iot_connection_client_limits::max_pending_publishes, 1-65535. */
#ifndef AZ_IOT_CONNECTION_CLIENT_DEFAULT_MAX_PENDING_PUBLISHES
#define AZ_IOT_CONNECTION_CLIENT_DEFAULT_MAX_PENDING_PUBLISHES 16
#endif
/** @brief Default az_iot_connection_client_limits::max_publish_reservations, 1-254. */
#ifndef AZ_IOT_CONNECTION_CLIENT_DEFAULT_MAX_PUBLISH_RESERVATIONS
#define AZ_IOT_CONNECTION_CLIENT_DEFAULT_MAX_PUBLISH_RESERVATIONS 2
#endif
/** @brief Default az_iot_connection_client_limits::max_persistent_subscriptions, 1-255. */
#ifndef AZ_IOT_CONNECTION_CLIENT_DEFAULT_MAX_PERSISTENT_SUBSCRIPTIONS
#define AZ_IOT_CONNECTION_CLIENT_DEFAULT_MAX_PERSISTENT_SUBSCRIPTIONS 8
#endif
/** @brief Default az_iot_connection_client_limits::max_feature_observers, 1-65535. */
#ifndef AZ_IOT_CONNECTION_CLIENT_DEFAULT_MAX_FEATURE_OBSERVERS
#define AZ_IOT_CONNECTION_CLIENT_DEFAULT_MAX_FEATURE_OBSERVERS 10
#endif
/** @brief Default az_iot_connection_client_limits::max_application_state_observers, 1-65535. */
#ifndef AZ_IOT_CONNECTION_CLIENT_DEFAULT_MAX_APPLICATION_STATE_OBSERVERS
#define AZ_IOT_CONNECTION_CLIENT_DEFAULT_MAX_APPLICATION_STATE_OBSERVERS 4
#endif
/** @brief Default az_iot_connection_client_limits::max_feature_clients, 1-65535. */
#ifndef AZ_IOT_CONNECTION_CLIENT_DEFAULT_MAX_FEATURE_CLIENTS
#define AZ_IOT_CONNECTION_CLIENT_DEFAULT_MAX_FEATURE_CLIENTS 8
#endif
/** @brief Default az_iot_connection_client_limits::max_inbound_handlers, 1-65535. */
#ifndef AZ_IOT_CONNECTION_CLIENT_DEFAULT_MAX_INBOUND_HANDLERS
#define AZ_IOT_CONNECTION_CLIENT_DEFAULT_MAX_INBOUND_HANDLERS 8
#endif

#if defined(AZ_IOT_CONNECTION_CLIENT_DEFAULT_MAX_SESSION_HANDLERS) \
    || defined(AZ_IOT_CONNECTION_CLIENT_DEFAULT_MAX_FEATURE_STATE_OBSERVERS)
#error "Use AZ_IOT_CONNECTION_CLIENT_DEFAULT_MAX_FEATURE_OBSERVERS: one table for both."
#endif

#if AZ_IOT_CONNECTION_CLIENT_DEFAULT_MAX_PENDING_PUBLISHES < 1 \
    || AZ_IOT_CONNECTION_CLIENT_DEFAULT_MAX_PENDING_PUBLISHES > 65535
#error "AZ_IOT_CONNECTION_CLIENT_DEFAULT_MAX_PENDING_PUBLISHES must be 1-65535"
#endif
#if AZ_IOT_CONNECTION_CLIENT_DEFAULT_MAX_PUBLISH_RESERVATIONS < 1 \
    || AZ_IOT_CONNECTION_CLIENT_DEFAULT_MAX_PUBLISH_RESERVATIONS > 254
#error "AZ_IOT_CONNECTION_CLIENT_DEFAULT_MAX_PUBLISH_RESERVATIONS must be 1-254"
#endif
#if AZ_IOT_CONNECTION_CLIENT_DEFAULT_MAX_PERSISTENT_SUBSCRIPTIONS < 1 \
    || AZ_IOT_CONNECTION_CLIENT_DEFAULT_MAX_PERSISTENT_SUBSCRIPTIONS > 255
#error "AZ_IOT_CONNECTION_CLIENT_DEFAULT_MAX_PERSISTENT_SUBSCRIPTIONS must be 1-255"
#endif
#if AZ_IOT_CONNECTION_CLIENT_DEFAULT_MAX_FEATURE_OBSERVERS < 1                  \
    || AZ_IOT_CONNECTION_CLIENT_DEFAULT_MAX_FEATURE_OBSERVERS > 65535           \
    || AZ_IOT_CONNECTION_CLIENT_DEFAULT_MAX_APPLICATION_STATE_OBSERVERS < 1     \
    || AZ_IOT_CONNECTION_CLIENT_DEFAULT_MAX_APPLICATION_STATE_OBSERVERS > 65535 \
    || AZ_IOT_CONNECTION_CLIENT_DEFAULT_MAX_FEATURE_CLIENTS < 1                 \
    || AZ_IOT_CONNECTION_CLIENT_DEFAULT_MAX_FEATURE_CLIENTS > 65535             \
    || AZ_IOT_CONNECTION_CLIENT_DEFAULT_MAX_INBOUND_HANDLERS < 1                \
    || AZ_IOT_CONNECTION_CLIENT_DEFAULT_MAX_INBOUND_HANDLERS > 65535
#error "AZ_IOT_CONNECTION_CLIENT_DEFAULT_MAX_* must be 1-65535"
#endif

#endif /* AZ_IOT_CONNECTION_CLIENT_CONFIG_H */
