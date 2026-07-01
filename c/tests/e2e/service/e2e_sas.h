// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/* SPDX-License-Identifier: MIT */
/* Internal helper for the e2e service client: Azure connection-string parsing
 * and Shared Access Signature (SAS) token generation.
 *
 * This header is INTERNAL to the az_iot_e2e_service static library. It is never
 * included by test translation units, so the vendored AMQP/OpenSSL details it
 * relies on stay hidden behind the service-client facade.
 */
#ifndef AZ_IOT_E2E_SAS_H
#define AZ_IOT_E2E_SAS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Parsed fields of an Azure connection string (IoT Hub service or Event
 * Hub-compatible endpoint). Unused fields are empty strings. */
typedef struct e2e_conn_info_tag
{
    char host[256];        /* HostName=<..>, or Endpoint=sb://<host>/ (scheme+slash stripped) */
    char key_name[128];    /* SharedAccessKeyName */
    char key[256];         /* SharedAccessKey (base64 text, verbatim)                         */
    char entity_path[128]; /* EntityPath (Event Hub connection strings only; may be empty)     */
} e2e_conn_info_t;

/* Parse an Azure connection string. Recognizes the HostName, Endpoint,
 * SharedAccessKeyName, SharedAccessKey and EntityPath segments (';'-separated,
 * each 'Key=Value' split on the FIRST '='). Returns true when at least a host
 * and a key were found. */
bool e2e_conn_info_parse(const char* connection_string, e2e_conn_info_t* out);

/* Build an Azure Shared Access Signature authorizing @p resource_uri (unescaped)
 * until @p expiry_unix_sec, signed with @p key using HMAC-SHA256.
 *
 * @p key_base64_decode selects the HMAC key derivation, which differs by service:
 *   - IoT Hub (service REST + AMQP): true  -> the HMAC key is base64-decode(key).
 *   - Event Hubs / Service Bus:      false -> the HMAC key is the raw UTF-8 bytes
 *                                             of the key string.
 * (Both conventions are the ones the respective Azure services document.)
 *
 * Writes a NUL-terminated "SharedAccessSignature sr=..&sig=..&se=..&skn=.." token
 * into @p out. Returns true on success. */
bool e2e_sas_token_create(
    const char* resource_uri,
    const char* key_name,
    const char* key,
    bool key_base64_decode,
    int64_t expiry_unix_sec,
    char* out,
    size_t out_size);

/* Percent-encode @p src (RFC 3986 unreserved set preserved) into @p dst.
 * Returns the number of characters written (excluding the NUL). */
size_t e2e_url_encode(const char* src, char* dst, size_t dst_size);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_E2E_SAS_H */
