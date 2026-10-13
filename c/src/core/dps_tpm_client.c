// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */

/**
 * @file dps_tpm_client.c
 * @brief DPS registration with TPM attestation over HTTPS: request builder and
 * response parser (az_iot_dps_tpm_client.h).
 */

#include "azure/iot/az_iot_dps_tpm_client.h"

#include <string.h>

#include <azure/core/az_json.h>

#include "azure/iot/az_iot_log.h"
#include "azure/iot/az_iot_log_components.h"
#include "azure/iot/az_iot_sas_token.h"
#include "internal/base64.h"
#include "internal/crypto.h"
#include "internal/span_writer.h"

/** @brief What the client sends next. */
enum
{
  DPS_TPM_STATE_FAILED = 0, /**< Nothing; init() again. */
  DPS_TPM_STATE_REGISTER, /**< PUT register without a token: asks for the key. */
  DPS_TPM_STATE_AUTH_REGISTER, /**< PUT register with a TPM-signed token. */
  DPS_TPM_STATE_POLL, /**< GET operation status. */
  DPS_TPM_STATE_ASSIGNED /**< Nothing; done. */
};

/** @brief Flag ORed into the state while a request is outstanding. */
#define DPS_TPM_STATE_SENT 0x100

#define DPS_TPM_DEFAULT_KEY_NAME "registration"
#define DPS_TPM_HTTP_UNAUTHORIZED 401u
#define DPS_TPM_HTTP_TOO_MANY_REQUESTS 429u

/** @brief Between scope and registration ID in the SAS resource URI. */
#define DPS_TPM_URI_INFIX "%2fregistrations%2f"

/** @brief Room for `<scope>%2fregistrations%2f<encoded id>` and its terminator. */
#define DPS_TPM_URI_SIZE                                            \
  ((size_t)AZ_IOT_DPS_TPM_ID_MAX + (sizeof(DPS_TPM_URI_INFIX) - 1u) \
   + 3u * (size_t)AZ_IOT_DPS_TPM_ID_MAX + 1u)

/** @brief Members of one DPS response that the client reads. Spans are slices
 * of the body. */
typedef struct dps_tpm_fields
{
  az_span status; /**< `status`. */
  az_json_token operation_id; /**< `operationId`. */
  az_span authentication_key; /**< `authenticationKey` (401 challenge). */
  az_span key_name; /**< `keyName` (401 challenge). */
  int32_t error_code; /**< `errorCode`, top level or registrationState. */
  az_json_token assigned_hub; /**< `registrationState.assignedHub`. */
  az_json_token device_id; /**< `registrationState.deviceId`. */
  az_span connection_profile; /**< `registrationState.connectionProfile`. */
  bool has_connection_profile; /**< connectionProfile is a string. */
  az_span issued_certificate_chain; /**< `registrationState.issuedCertificateChain`. */
  az_span hub_key; /**< `registrationState.tpm.authenticationKey`. */
} dps_tpm_fields;

static bool is_id_char(char ch, bool allow_colon)
{
  return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')
      || ch == '-' || ch == '.' || ch == '_' || (allow_colon && ch == ':');
}

/** @brief Whether @p s is 1..@p max characters of is_id_char(). */
static bool is_valid_id(const char* s, size_t max, bool allow_colon)
{
  if (s == NULL || s[0] == '\0')
  {
    return false;
  }
  size_t n = 0;
  for (; s[n] != '\0'; ++n)
  {
    if (n >= max || !is_id_char(s[n], allow_colon))
    {
      return false;
    }
  }
  return true;
}

static bool is_base64_char(uint8_t ch)
{
  return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')
      || ch == '+' || ch == '/' || ch == '=';
}

/** @brief Whether @p s is non-empty standard base64 text, so safe in a JSON string. */
static bool is_base64_text(const char* s)
{
  if (s == NULL || s[0] == '\0')
  {
    return false;
  }
  for (; *s != '\0'; ++s)
  {
    if (!is_base64_char((uint8_t)*s))
    {
      return false;
    }
  }
  return true;
}

/** @brief Whether only JSON whitespace follows @p last, a slice of @p doc. */
static bool only_whitespace_after(az_span doc, az_span last)
{
  const uint8_t* begin = az_span_ptr(doc);
  size_t consumed = (size_t)(az_span_ptr(last) - begin) + (size_t)az_span_size(last);
  for (size_t i = consumed; i < (size_t)az_span_size(doc); ++i)
  {
    uint8_t ch = begin[i];
    if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n')
    {
      return false;
    }
  }
  return true;
}

/** @brief Whether @p payload is exactly one JSON object, optionally padded with whitespace. */
static bool is_json_object(const char* payload)
{
  size_t len = strlen(payload);
  if (len == 0 || len > (size_t)INT32_MAX)
  {
    return false;
  }
  az_span span = az_span_create((uint8_t*)(uintptr_t)payload, (int32_t)len);
  az_json_reader jr;
  if (az_result_failed(az_json_reader_init(&jr, span, NULL))
      || az_result_failed(az_json_reader_next_token(&jr))
      || jr.token.kind != AZ_JSON_TOKEN_BEGIN_OBJECT
      || az_result_failed(az_json_reader_skip_children(&jr)))
  {
    return false;
  }
  return only_whitespace_after(span, jr.token.slice);
}

/** @brief Appends standard padded base64 of @p data. */
static void append_base64(az_iot_span_writer* w, const uint8_t* data, size_t len)
{
  static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  for (size_t i = 0; i < len; i += 3)
  {
    uint32_t v = (uint32_t)data[i] << 16;
    size_t n = len - i;
    if (n > 1)
    {
      v |= (uint32_t)data[i + 1] << 8;
    }
    if (n > 2)
    {
      v |= data[i + 2];
    }
    az_iot_span_writer_append_u8(w, (uint8_t)alphabet[(v >> 18) & 0x3Fu]);
    az_iot_span_writer_append_u8(w, (uint8_t)alphabet[(v >> 12) & 0x3Fu]);
    az_iot_span_writer_append_u8(w, n > 1 ? (uint8_t)alphabet[(v >> 6) & 0x3Fu] : (uint8_t)'=');
    az_iot_span_writer_append_u8(w, n > 2 ? (uint8_t)alphabet[v & 0x3Fu] : (uint8_t)'=');
  }
}

AZ_NODISCARD az_iot_dps_tpm_client_options az_iot_dps_tpm_client_options_default(void)
{
  az_iot_dps_tpm_client_options options;
  memset(&options, 0, sizeof(options));
  return options;
}

AZ_NODISCARD az_iot_result az_iot_dps_tpm_client_init(
    az_iot_dps_tpm_client* client,
    const az_iot_dps_tpm_client_options* options)
{
  if (client == NULL || options == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  memset(client, 0, sizeof(*client));
  if (!is_valid_id(options->id_scope, AZ_IOT_DPS_TPM_ID_MAX, false)
      || !is_valid_id(options->registration_id, AZ_IOT_DPS_TPM_ID_MAX, true)
      || options->endorsement_key == NULL || options->endorsement_key_len == 0
      || options->endorsement_key_len > UINT16_MAX + 2u || options->storage_root_key == NULL
      || options->storage_root_key_len == 0 || options->storage_root_key_len > UINT16_MAX + 2u
      || (options->payload != NULL && !is_json_object(options->payload))
      || (options->csr != NULL && !is_base64_text(options->csr))
      || (options->api_version != NULL
          && !is_valid_id(options->api_version, AZ_IOT_DPS_TPM_ID_MAX, false))
      || options->tpm.import_key == NULL || options->tpm.hmac_sha256 == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  client->_internal.opts = *options;
  if (client->_internal.opts.api_version == NULL)
  {
    client->_internal.opts.api_version = AZ_IOT_DPS_TPM_DEFAULT_API_VERSION;
  }
  if (client->_internal.opts.sas_token_lifetime_seconds == 0)
  {
    client->_internal.opts.sas_token_lifetime_seconds
        = AZ_IOT_DPS_TPM_DEFAULT_SAS_TOKEN_LIFETIME_SECONDS;
  }
  client->_internal.state = DPS_TPM_STATE_REGISTER;
  return AZ_IOT_OK;
}

/** @brief Appends `/<scope>/registrations/<id>`. */
static void append_registration_path(az_iot_span_writer* w, const az_iot_dps_tpm_client* client)
{
  az_iot_span_writer_append_str(w, "/");
  az_iot_span_writer_append_str(w, client->_internal.opts.id_scope);
  az_iot_span_writer_append_str(w, "/registrations/");
  az_iot_span_writer_append_url_encoded(w, client->_internal.opts.registration_id);
}

/** @brief Appends the register body. */
static void append_register_body(az_iot_span_writer* w, const az_iot_dps_tpm_client_options* o)
{
  az_iot_span_writer_append_str(w, "{\"registrationId\":\"");
  az_iot_span_writer_append_str(w, o->registration_id);
  az_iot_span_writer_append_str(w, "\",\"tpm\":{\"endorsementKey\":\"");
  append_base64(w, o->endorsement_key, o->endorsement_key_len);
  az_iot_span_writer_append_str(w, "\",\"storageRootKey\":\"");
  append_base64(w, o->storage_root_key, o->storage_root_key_len);
  az_iot_span_writer_append_str(w, "\"}");
  if (o->csr != NULL)
  {
    az_iot_span_writer_append_str(w, ",\"csr\":\"");
    az_iot_span_writer_append_str(w, o->csr);
    az_iot_span_writer_append_str(w, "\"");
  }
  if (o->payload != NULL)
  {
    az_iot_span_writer_append_str(w, ",\"payload\":");
    az_iot_span_writer_append_str(w, o->payload);
  }
  az_iot_span_writer_append_str(w, "}");
}

/**
 * @brief Writes a SAS token signed by the TPM into @p out.
 *
 * @return AZ_IOT_OK; AZ_IOT_ERR_INVALID_ARG for a 0 time;
 * AZ_IOT_ERR_NOT_ENOUGH_SPACE; the hmac_sha256 result.
 */
static az_iot_result build_token(
    const az_iot_dps_tpm_client* client,
    uint64_t unix_seconds,
    char* out,
    size_t out_size)
{
  const az_iot_dps_tpm_client_options* o = &client->_internal.opts;
  if (unix_seconds == 0 || unix_seconds > UINT64_MAX - o->sas_token_lifetime_seconds)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  uint64_t expiry = unix_seconds + o->sas_token_lifetime_seconds;

  char uri[DPS_TPM_URI_SIZE];
  az_iot_span_writer w;
  az_iot_span_writer_init(&w, AZ_SPAN_FROM_BUFFER(uri));
  az_iot_span_writer_append_str(&w, o->id_scope);
  az_iot_span_writer_append_str(&w, DPS_TPM_URI_INFIX);
  az_iot_span_writer_append_url_encoded(&w, o->registration_id);
  size_t uri_len = 0;
  az_iot_result r = az_iot_span_writer_end_str(&w, &uri_len);
  if (r != AZ_IOT_OK)
  {
    return r;
  }

  char to_sign[AZ_IOT_SAS_STRING_TO_SIGN_SIZE(DPS_TPM_URI_SIZE)];
  size_t to_sign_len = 0;
  r = az_iot_sas_token_string_to_sign(uri, expiry, to_sign, sizeof(to_sign), &to_sign_len);
  if (r != AZ_IOT_OK)
  {
    return r;
  }

  uint8_t mac[AZ_IOT_SHA256_SIZE];
  r = o->tpm.hmac_sha256(o->tpm.ctx, (const uint8_t*)to_sign, to_sign_len, mac);
  if (r == AZ_IOT_OK)
  {
    size_t token_len = 0;
    const char* key_name = client->_internal.key_name[0] != '\0' ? client->_internal.key_name
                                                                 : DPS_TPM_DEFAULT_KEY_NAME;
    r = az_iot_sas_token_from_signature(uri, key_name, expiry, mac, out, out_size, &token_len);
  }
  az_iot_crypto__wipe(mac, sizeof(mac));
  return r;
}

AZ_NODISCARD az_iot_result az_iot_dps_tpm_client_get_request(
    az_iot_dps_tpm_client* client,
    uint64_t unix_seconds,
    char* buffer,
    size_t buffer_size,
    az_iot_dps_tpm_request* out)
{
  if (client == NULL || buffer == NULL || buffer_size == 0 || out == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  memset(out, 0, sizeof(*out));
  buffer[0] = '\0';
  int state = client->_internal.state & ~DPS_TPM_STATE_SENT;
  if (state != DPS_TPM_STATE_REGISTER && state != DPS_TPM_STATE_AUTH_REGISTER
      && state != DPS_TPM_STATE_POLL)
  {
    return AZ_IOT_ERR_NOT_INITIALIZED;
  }
  int32_t capacity = buffer_size > (size_t)INT32_MAX ? INT32_MAX : (int32_t)buffer_size;
  az_span all = az_span_create((uint8_t*)buffer, capacity);
  const az_iot_dps_tpm_client_options* o = &client->_internal.opts;

  /* Path. */
  az_iot_span_writer w;
  az_iot_span_writer_init(&w, all);
  append_registration_path(&w, client);
  if (state == DPS_TPM_STATE_POLL)
  {
    az_iot_span_writer_append_str(&w, "/operations/");
    az_iot_span_writer_append_url_encoded(&w, client->_internal.operation_id);
  }
  else
  {
    az_iot_span_writer_append_str(&w, "/register");
  }
  az_iot_span_writer_append_str(&w, "?api-version=");
  az_iot_span_writer_append_str(&w, o->api_version);
  size_t path_len = 0;
  az_iot_result r = az_iot_span_writer_end_str(&w, &path_len);
  size_t used = path_len + 1;

  /* Authorization, once DPS has sent the key. */
  const char* authorization = NULL;
  if (r == AZ_IOT_OK && state != DPS_TPM_STATE_REGISTER)
  {
    authorization = buffer + used;
    r = build_token(client, unix_seconds, buffer + used, buffer_size - used);
    if (r == AZ_IOT_OK)
    {
      used += strlen(authorization) + 1;
    }
  }

  /* Body, for register. */
  const char* body = NULL;
  size_t body_len = 0;
  if (r == AZ_IOT_OK && state != DPS_TPM_STATE_POLL)
  {
    body = buffer + used;
    az_iot_span_writer_init(&w, az_span_slice_to_end(all, (int32_t)used));
    append_register_body(&w, o);
    r = az_iot_span_writer_end_str(&w, &body_len);
  }

  if (r != AZ_IOT_OK)
  {
    az_iot_crypto__wipe(buffer, buffer_size);
    return r;
  }
  out->method = state == DPS_TPM_STATE_POLL ? "GET" : "PUT";
  out->path = buffer;
  out->authorization = authorization;
  out->body = body;
  out->body_len = body_len;
  client->_internal.state = state | DPS_TPM_STATE_SENT;
  return AZ_IOT_OK;
}

/** @brief Copies JSON string @p token into @p out (@p out_size bytes, terminated). */
static bool copy_json_string(const az_json_token* token, char* out, size_t out_size)
{
  /* The slice may hold escapes, so only the unescaped length is checked (by az_core). */
  if (token->kind != AZ_JSON_TOKEN_STRING || az_span_size(token->slice) <= 0
      || out_size > (size_t)INT32_MAX)
  {
    return false;
  }
  int32_t written = 0;
  if (az_result_failed(az_json_token_get_string(token, out, (int32_t)out_size, &written))
      || written <= 0)
  {
    return false;
  }
  return true;
}

/** @brief Records the string token at @p jr into @p out; anything else leaves it empty. */
static void take_string(const az_json_reader* jr, az_span* out)
{
  if (jr->token.kind == AZ_JSON_TOKEN_STRING)
  {
    *out = jr->token.slice;
  }
}

/** @brief Records the token at @p jr when it is a string; JSON escapes stay pending. */
static void take_string_token(const az_json_reader* jr, az_json_token* out)
{
  if (jr->token.kind == AZ_JSON_TOKEN_STRING)
  {
    *out = jr->token;
  }
}

/** @brief Records the integer token at @p jr into @p out when it fits. */
static void take_int32(const az_json_reader* jr, int32_t* out)
{
  int32_t v = 0;
  if (jr->token.kind == AZ_JSON_TOKEN_NUMBER
      && az_result_succeeded(az_json_token_get_int32(&jr->token, &v)))
  {
    *out = v;
  }
}

/** @brief Skips the value at @p jr when it is an object or array. */
static bool skip_value(az_json_reader* jr)
{
  if (jr->token.kind == AZ_JSON_TOKEN_BEGIN_OBJECT || jr->token.kind == AZ_JSON_TOKEN_BEGIN_ARRAY)
  {
    return az_result_succeeded(az_json_reader_skip_children(jr));
  }
  return true;
}

/** @brief Reads `tpm.authenticationKey` of the object @p jr is at. */
static bool read_tpm_object(az_json_reader* jr, dps_tpm_fields* f)
{
  while (az_result_succeeded(az_json_reader_next_token(jr)))
  {
    if (jr->token.kind == AZ_JSON_TOKEN_END_OBJECT)
    {
      return true;
    }
    bool is_key = az_json_token_is_text_equal(&jr->token, AZ_SPAN_FROM_STR("authenticationKey"));
    if (az_result_failed(az_json_reader_next_token(jr)))
    {
      return false;
    }
    if (is_key)
    {
      take_string(jr, &f->hub_key);
    }
    else if (!skip_value(jr))
    {
      return false;
    }
  }
  return false;
}

/** @brief Reads the members of `registrationState`, the object @p jr is at. */
static bool read_registration_state(az_json_reader* jr, dps_tpm_fields* f)
{
  while (az_result_succeeded(az_json_reader_next_token(jr)))
  {
    if (jr->token.kind == AZ_JSON_TOKEN_END_OBJECT)
    {
      return true;
    }
    az_json_token name = jr->token;
    if (az_result_failed(az_json_reader_next_token(jr)))
    {
      return false;
    }
    if (az_json_token_is_text_equal(&name, AZ_SPAN_FROM_STR("assignedHub")))
    {
      take_string_token(jr, &f->assigned_hub);
    }
    else if (az_json_token_is_text_equal(&name, AZ_SPAN_FROM_STR("deviceId")))
    {
      take_string_token(jr, &f->device_id);
    }
    else if (az_json_token_is_text_equal(&name, AZ_SPAN_FROM_STR("errorCode")))
    {
      take_int32(jr, &f->error_code);
    }
    else if (az_json_token_is_text_equal(&name, AZ_SPAN_FROM_STR("connectionProfile")))
    {
      if (jr->token.kind == AZ_JSON_TOKEN_STRING)
      {
        f->connection_profile = jr->token.slice;
        f->has_connection_profile = true;
      }
    }
    else if (
        az_json_token_is_text_equal(&name, AZ_SPAN_FROM_STR("issuedCertificateChain"))
        && jr->token.kind == AZ_JSON_TOKEN_BEGIN_ARRAY)
    {
      uint8_t* begin = az_span_ptr(jr->token.slice);
      if (az_result_failed(az_json_reader_skip_children(jr)))
      {
        return false;
      }
      uint8_t* end = az_span_ptr(jr->token.slice) + az_span_size(jr->token.slice);
      f->issued_certificate_chain = az_span_create(begin, (int32_t)(end - begin));
    }
    else if (
        az_json_token_is_text_equal(&name, AZ_SPAN_FROM_STR("tpm"))
        && jr->token.kind == AZ_JSON_TOKEN_BEGIN_OBJECT)
    {
      if (!read_tpm_object(jr, f))
      {
        return false;
      }
    }
    else if (!skip_value(jr))
    {
      return false;
    }
  }
  return false;
}

/** @brief Reads @p body, a DPS response object, into @p f. */
static bool read_fields(az_span body, dps_tpm_fields* f)
{
  memset(f, 0, sizeof(*f));
  if (az_span_ptr(body) == NULL || az_span_size(body) <= 0)
  {
    return false;
  }
  az_json_reader jr;
  if (az_result_failed(az_json_reader_init(&jr, body, NULL))
      || az_result_failed(az_json_reader_next_token(&jr))
      || jr.token.kind != AZ_JSON_TOKEN_BEGIN_OBJECT)
  {
    return false;
  }
  while (az_result_succeeded(az_json_reader_next_token(&jr)))
  {
    if (jr.token.kind == AZ_JSON_TOKEN_END_OBJECT)
    {
      return only_whitespace_after(body, jr.token.slice);
    }
    az_json_token name = jr.token;
    if (az_result_failed(az_json_reader_next_token(&jr)))
    {
      return false;
    }
    if (az_json_token_is_text_equal(&name, AZ_SPAN_FROM_STR("status")))
    {
      take_string(&jr, &f->status);
    }
    else if (az_json_token_is_text_equal(&name, AZ_SPAN_FROM_STR("operationId")))
    {
      take_string_token(&jr, &f->operation_id);
    }
    else if (az_json_token_is_text_equal(&name, AZ_SPAN_FROM_STR("authenticationKey")))
    {
      take_string(&jr, &f->authentication_key);
    }
    else if (az_json_token_is_text_equal(&name, AZ_SPAN_FROM_STR("keyName")))
    {
      take_string(&jr, &f->key_name);
    }
    else if (az_json_token_is_text_equal(&name, AZ_SPAN_FROM_STR("errorCode")))
    {
      take_int32(&jr, &f->error_code);
    }
    else if (
        az_json_token_is_text_equal(&name, AZ_SPAN_FROM_STR("registrationState"))
        && jr.token.kind == AZ_JSON_TOKEN_BEGIN_OBJECT)
    {
      if (!read_registration_state(&jr, f))
      {
        return false;
      }
    }
    else if (!skip_value(&jr))
    {
      return false;
    }
  }
  return false;
}

/**
 * @brief Decodes the base64 JSON string @p text in place and imports it.
 *
 * The only escape base64 can carry in JSON is `\/`; it is removed first.
 */
static az_iot_result import_key(const az_iot_dps_tpm_client* client, az_span text)
{
  uint8_t* p = az_span_ptr(text);
  int32_t n = az_span_size(text);
  int32_t out = 0;
  for (int32_t i = 0; i < n; ++i)
  {
    if (p[i] == '\\')
    {
      if (i + 1 >= n || p[i + 1] != '/')
      {
        return AZ_IOT_ERR_PROTOCOL;
      }
      ++i;
    }
    p[out++] = p[i];
  }
  if (out == 0)
  {
    return AZ_IOT_ERR_PROTOCOL;
  }
  az_span decoded = AZ_SPAN_EMPTY;
  az_span unescaped = az_span_create(p, out);
  if (az_iot_base64_decode(unescaped, true, unescaped, &decoded) != AZ_IOT_OK)
  {
    return AZ_IOT_ERR_PROTOCOL;
  }
  const az_iot_dps_tpm_ops* tpm = &client->_internal.opts.tpm;
  az_iot_result r = tpm->import_key(tpm->ctx, az_span_ptr(decoded), (size_t)az_span_size(decoded));
  az_iot_crypto__wipe(p, (size_t)n);
  return r;
}

/** @brief Ends the registration with @p result. */
static az_iot_result fail(az_iot_dps_tpm_client* client, az_iot_result result)
{
  client->_internal.state = DPS_TPM_STATE_FAILED;
  return result;
}

/** @brief Handles a 401: the key challenge, or a refused token. */
static az_iot_result on_unauthorized(
    az_iot_dps_tpm_client* client,
    int state,
    const dps_tpm_fields* f,
    az_iot_dps_tpm_next* out)
{
  if (state != DPS_TPM_STATE_REGISTER || az_span_size(f->authentication_key) <= 0)
  {
    AZ_IOT_LOG_ERRORF(
        AZ_IOT_LOG_COMPONENT_DPS,
        "TPM registration refused: HTTP 401, errorCode %ld",
        (long)f->error_code);
    return fail(client, AZ_IOT_ERR_AUTH);
  }
  client->_internal.key_name[0] = '\0';
  if (az_span_size(f->key_name) > 0)
  {
    int32_t len = az_span_size(f->key_name);
    const uint8_t* name = az_span_ptr(f->key_name);
    if ((size_t)len > AZ_IOT_DPS_TPM_TOKEN_MAX)
    {
      return fail(client, AZ_IOT_ERR_PROTOCOL);
    }
    for (int32_t i = 0; i < len; ++i)
    {
      if (!is_id_char((char)name[i], false))
      {
        return fail(client, AZ_IOT_ERR_PROTOCOL);
      }
    }
    memcpy(client->_internal.key_name, name, (size_t)len);
    client->_internal.key_name[len] = '\0';
  }
  az_iot_result r = import_key(client, f->authentication_key);
  if (r != AZ_IOT_OK)
  {
    AZ_IOT_LOG_ERROR(AZ_IOT_LOG_COMPONENT_DPS, "TPM registration: importing the DPS key failed");
    return fail(client, r);
  }
  client->_internal.state = DPS_TPM_STATE_AUTH_REGISTER;
  out->retry_after_seconds = 0;
  return AZ_IOT_OK;
}

/** @brief Handles an assignment: copies the result and imports the hub key. */
static az_iot_result on_assigned(az_iot_dps_tpm_client* client, const dps_tpm_fields* f)
{
  if (!copy_json_string(
          &f->assigned_hub, client->_internal.assigned_hub, sizeof(client->_internal.assigned_hub))
      || !copy_json_string(
          &f->device_id, client->_internal.device_id, sizeof(client->_internal.device_id)))
  {
    return fail(client, AZ_IOT_ERR_PROTOCOL);
  }

  az_iot_dps_tpm_assignment* a = &client->_internal.assignment;
  a->assigned_hub = client->_internal.assigned_hub;
  a->device_id = client->_internal.device_id;
  a->connection_profile = AZ_IOT_CONNECTION_PROFILE_MQTT_V3;
  if (f->has_connection_profile
      && !az_span_is_content_equal(f->connection_profile, AZ_SPAN_FROM_STR("classic")))
  {
    a->connection_profile
        = az_span_is_content_equal(f->connection_profile, AZ_SPAN_FROM_STR("mqttV5"))
        ? AZ_IOT_CONNECTION_PROFILE_MQTT_V5
        : AZ_IOT_CONNECTION_PROFILE_UNKNOWN;
  }
  a->issued_certificate_chain = f->issued_certificate_chain;
  a->hub_key_imported = false;
  if (az_span_size(f->hub_key) > 0)
  {
    az_iot_result r = import_key(client, f->hub_key);
    if (r != AZ_IOT_OK)
    {
      AZ_IOT_LOG_ERROR(AZ_IOT_LOG_COMPONENT_DPS, "TPM registration: importing the hub key failed");
      return fail(client, r);
    }
    a->hub_key_imported = true;
  }
  client->_internal.state = DPS_TPM_STATE_ASSIGNED;
  return AZ_IOT_OK;
}

AZ_NODISCARD az_iot_result az_iot_dps_tpm_client_handle_response(
    az_iot_dps_tpm_client* client,
    uint16_t http_status,
    az_span body,
    uint32_t retry_after_seconds,
    az_iot_dps_tpm_next* out)
{
  if (client == NULL || out == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  memset(out, 0, sizeof(*out));
  if ((client->_internal.state & DPS_TPM_STATE_SENT) == 0)
  {
    return AZ_IOT_ERR_NOT_INITIALIZED;
  }
  int state = client->_internal.state & ~DPS_TPM_STATE_SENT;
  client->_internal.state = state;
  client->_internal.http_status = http_status;
  client->_internal.error_code = 0;
  uint32_t wait
      = retry_after_seconds != 0 ? retry_after_seconds : AZ_IOT_DPS_TPM_DEFAULT_RETRY_AFTER_SECONDS;

  if (http_status == DPS_TPM_HTTP_TOO_MANY_REQUESTS || http_status >= 500u)
  {
    out->retry_after_seconds = wait;
    return AZ_IOT_OK;
  }

  dps_tpm_fields f;
  bool parsed = read_fields(body, &f);
  client->_internal.error_code = f.error_code;

  if (http_status == DPS_TPM_HTTP_UNAUTHORIZED)
  {
    if (!parsed && state == DPS_TPM_STATE_REGISTER)
    {
      return fail(client, AZ_IOT_ERR_PROTOCOL);
    }
    return on_unauthorized(client, state, &f, out);
  }
  if (http_status < 200u || http_status >= 300u)
  {
    AZ_IOT_LOG_ERRORF(
        AZ_IOT_LOG_COMPONENT_DPS,
        "TPM registration failed: HTTP %u, errorCode %ld",
        (unsigned)http_status,
        (long)f.error_code);
    return fail(client, AZ_IOT_ERR_DPS);
  }
  if (!parsed || state == DPS_TPM_STATE_REGISTER)
  {
    /* A 2xx before the key exchange is not part of the TPM flow. */
    return fail(client, AZ_IOT_ERR_PROTOCOL);
  }

  if (az_span_is_content_equal(f.status, AZ_SPAN_FROM_STR("assigned")))
  {
    az_iot_result r = on_assigned(client, &f);
    out->assigned = r == AZ_IOT_OK;
    return r;
  }
  if (az_span_is_content_equal(f.status, AZ_SPAN_FROM_STR("assigning"))
      || az_span_is_content_equal(f.status, AZ_SPAN_FROM_STR("unassigned")))
  {
    if (!copy_json_string(
            &f.operation_id,
            client->_internal.operation_id,
            sizeof(client->_internal.operation_id)))
    {
      return fail(client, AZ_IOT_ERR_PROTOCOL);
    }
    client->_internal.state = DPS_TPM_STATE_POLL;
    out->retry_after_seconds = wait;
    return AZ_IOT_OK;
  }
  AZ_IOT_LOG_ERRORF(
      AZ_IOT_LOG_COMPONENT_DPS,
      "TPM registration failed: status \"%.*s\", errorCode %ld",
      (int)az_span_size(f.status),
      (const char*)az_span_ptr(f.status),
      (long)f.error_code);
  return fail(client, AZ_IOT_ERR_DPS);
}

const az_iot_dps_tpm_assignment* az_iot_dps_tpm_client_get_assignment(
    const az_iot_dps_tpm_client* client)
{
  if (client == NULL || client->_internal.state != DPS_TPM_STATE_ASSIGNED)
  {
    return NULL;
  }
  return &client->_internal.assignment;
}

void az_iot_dps_tpm_client_get_error(
    const az_iot_dps_tpm_client* client,
    uint16_t* http_status,
    int32_t* error_code)
{
  if (http_status != NULL)
  {
    *http_status = client != NULL ? client->_internal.http_status : 0;
  }
  if (error_code != NULL)
  {
    *error_code = client != NULL ? client->_internal.error_code : 0;
  }
}

/** @brief Takes one UINT16-sized part off @p key at @p *pos into @p out. */
static bool take_sized(const uint8_t* key, size_t key_len, size_t* pos, az_span* out)
{
  if (key_len - *pos < 2u)
  {
    return false;
  }
  size_t size = ((size_t)key[*pos] << 8) | key[*pos + 1];
  if (key_len - *pos - 2u < size)
  {
    return false;
  }
  *out = az_span_create((uint8_t*)(uintptr_t)(key + *pos), (int32_t)(size + 2u));
  *pos += size + 2u;
  return true;
}

AZ_NODISCARD az_iot_result
az_iot_dps_tpm_parse_key(const uint8_t* key, size_t key_len, az_iot_dps_tpm_key_parts* out)
{
  if (out == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  memset(out, 0, sizeof(*out));
  if (key == NULL || key_len == 0 || key_len > (size_t)INT32_MAX)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  size_t pos = 0;
  if (!take_sized(key, key_len, &pos, &out->credential_blob)
      || !take_sized(key, key_len, &pos, &out->secret)
      || !take_sized(key, key_len, &pos, &out->duplicate)
      || !take_sized(key, key_len, &pos, &out->in_sym_seed)
      || !take_sized(key, key_len, &pos, &out->object_public)
      || (pos < key_len && !take_sized(key, key_len, &pos, &out->encrypted_data)))
  {
    memset(out, 0, sizeof(*out));
    return AZ_IOT_ERR_PROTOCOL;
  }
  return AZ_IOT_OK;
}

AZ_NODISCARD az_iot_result az_iot_dps_tpm_registration_id_from_ek(
    const az_iot_crypto* crypto,
    const uint8_t* endorsement_key,
    size_t endorsement_key_len,
    char out[AZ_IOT_DPS_TPM_REGISTRATION_ID_SIZE])
{
  if (out == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  out[0] = '\0';
  if (endorsement_key == NULL || endorsement_key_len == 0
      || az_iot_crypto__validate(crypto) != AZ_IOT_OK)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  uint8_t digest[AZ_IOT_SHA256_SIZE];
  az_iot_result r = az_iot_crypto__sha256(crypto, endorsement_key, endorsement_key_len, digest);
  if (r != AZ_IOT_OK)
  {
    return r;
  }
  static const char alphabet[] = "abcdefghijklmnopqrstuvwxyz234567";
  size_t n = 0;
  uint32_t bits = 0;
  int pending = 0;
  for (size_t i = 0; i < sizeof(digest); ++i)
  {
    bits = (bits << 8) | digest[i];
    pending += 8;
    while (pending >= 5)
    {
      if (n >= AZ_IOT_DPS_TPM_REGISTRATION_ID_SIZE - 2)
      {
        return AZ_IOT_ERR_INTERNAL;
      }
      pending -= 5;
      out[n++] = alphabet[(bits >> pending) & 0x1Fu];
    }
  }
  if (pending > 0)
  {
    out[n++] = alphabet[(bits << (5 - pending)) & 0x1Fu];
  }
  out[n] = '\0';
  return AZ_IOT_OK;
}
