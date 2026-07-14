// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/* SPDX-License-Identifier: MIT */
#include "e2e_device.h"

#include "azure/iot/adapters/az_iot_adapter_paho.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* DPS provision + MQTT connect can take a while on a cold hub. */
#define E2E_DEVICE_CONNECT_TIMEOUT_S 90

/* Duplicate an environment variable into a heap buffer (portable). Returns NULL
 * when unset/empty. Caller frees. */
static char* env_dup(const char* name)
{
#ifdef _WIN32
    char* value = NULL;
    size_t len = 0;
    if (_dupenv_s(&value, &len, name) != 0 || value == NULL || value[0] == '\0')
    {
        free(value);
        return NULL;
    }
    return value;
#else
    const char* value = getenv(name);
    if (value == NULL || value[0] == '\0')
    {
        return NULL;
    }
    {
        size_t n = strlen(value) + 1;
        char* copy = (char*)malloc(n);
        if (copy != NULL)
        {
            memcpy(copy, value, n);
        }
        return copy;
    }
#endif
}

static int device_config_load(e2e_device* dev)
{
    dev->id_scope        = env_dup("AZ_IOT_DPS_ID_SCOPE");
    dev->reg_id          = env_dup("AZ_IOT_DPS_REGISTRATION_ID");
    dev->cert            = env_dup("AZ_IOT_CLIENT_CERT");
    dev->key             = env_dup("AZ_IOT_CLIENT_KEY");
    dev->ca              = env_dup("AZ_IOT_TRUSTED_CA");
    dev->global_endpoint = env_dup("AZ_IOT_DPS_GLOBAL_ENDPOINT");

    if (dev->id_scope == NULL || dev->reg_id == NULL || dev->cert == NULL
        || dev->key == NULL || dev->ca == NULL)
    {
        fprintf(stderr,
            "[e2e] missing required device env vars: AZ_IOT_DPS_ID_SCOPE/"
            "AZ_IOT_DPS_REGISTRATION_ID/AZ_IOT_CLIENT_CERT/AZ_IOT_CLIENT_KEY/"
            "AZ_IOT_TRUSTED_CA\n");
        return 1;
    }
    dev->device_id = dev->reg_id;
    return 0;
}

static void on_conn_state(az_iot_connection_state s, az_iot_result reason, void* user_ctx)
{
    (void)reason;
    ((e2e_device*)user_ctx)->conn_state = s;
}

void e2e_device_do_work(e2e_device* dev, int ms)
{
    (void)az_iot_connection_client_do_work(&dev->conn, ms);
}

int e2e_device_connect(e2e_device* dev)
{
    if (device_config_load(dev) != 0)
    {
        return 1;
    }

    az_iot_certificate_provider_pem_options pem = az_iot_certificate_provider_pem_options_default();
    pem.trusted_ca_pem_path  = dev->ca;
    pem.client_cert_pem_path = dev->cert;
    pem.client_key_pem_path  = dev->key;
    if (az_iot_certificate_provider_pem_init(&dev->certs, &pem) != AZ_IOT_OK)
    {
        fprintf(stderr, "[e2e] certificate provider init failed\n");
        return 1;
    }
    dev->certs_ok = true;

    az_iot_connection_client_options copts = az_iot_connection_client_options_default();
    copts.dps.id_scope = dev->id_scope;
    copts.dps.registration_id = dev->reg_id;
    copts.certificate_provider = &dev->certs.base;
    if (dev->global_endpoint != NULL)
    {
        copts.dps.global_endpoint = dev->global_endpoint;
    }

    if (az_iot_connection_client_init(&dev->conn, &copts) != AZ_IOT_OK)
    {
        fprintf(stderr, "[e2e] connection client init failed\n");
        return 1;
    }
    dev->conn_ok = true;
    az_iot_connection_client_set_state_callback(&dev->conn, on_conn_state, dev);

    if (az_iot_connection_client_register_mqtt_factory(&dev->conn, az_iot_paho_factory_create_v3_1_1()) != AZ_IOT_OK
        || az_iot_connection_client_register_mqtt_factory(&dev->conn, az_iot_paho_factory_create_v5()) != AZ_IOT_OK)
    {
        fprintf(stderr, "[e2e] MQTT factory registration failed\n");
        return 1;
    }

    if (az_iot_connection_client_open(&dev->conn) != AZ_IOT_OK)
    {
        fprintf(stderr, "[e2e] connection open failed\n");
        return 1;
    }

    time_t start = time(NULL);
    while (dev->conn_state != AZ_IOT_CONN_STATE_CONNECTED
           && (time(NULL) - start) < E2E_DEVICE_CONNECT_TIMEOUT_S)
    {
        e2e_device_do_work(dev, 50);
        if (dev->conn_state == AZ_IOT_CONN_STATE_FAULTED)
        {
            break;
        }
    }

    if (dev->conn_state != AZ_IOT_CONN_STATE_CONNECTED)
    {
        fprintf(stderr, "[e2e] device did not reach CONNECTED (state=%d)\n", (int)dev->conn_state);
        return 1;
    }
    return 0;
}

void e2e_device_disconnect(e2e_device* dev)
{
    if (dev->conn_ok)
    {
        az_iot_connection_client_close(&dev->conn);
        for (int i = 0; i < 100 && dev->conn_state != AZ_IOT_CONN_STATE_IDLE; ++i)
        {
            e2e_device_do_work(dev, 50);
        }
        az_iot_connection_client_destroy(&dev->conn);
        dev->conn_ok = false;
    }
    if (dev->certs_ok)
    {
        az_iot_certificate_provider_pem_destroy(&dev->certs);
        dev->certs_ok = false;
    }
    free(dev->id_scope);
    free(dev->reg_id);
    free(dev->cert);
    free(dev->key);
    free(dev->ca);
    free(dev->global_endpoint);
    dev->id_scope = NULL;
    dev->reg_id = NULL;
    dev->cert = NULL;
    dev->key = NULL;
    dev->ca = NULL;
    dev->global_endpoint = NULL;
    dev->device_id = NULL;
}
