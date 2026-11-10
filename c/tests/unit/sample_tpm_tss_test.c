// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */

/* The samples' tpm2-tss ESAPI backend against swtpm. Each DPS-format key is
 * built on the same TPM the way DPS builds it: TPM2_MakeCredential of the inner
 * wrapping key for the EK, bound to the SRK Name, and TPM2_Duplicate of an HMAC
 * key of known bytes to the SRK with that inner key. The backend's HMAC must
 * then match OpenSSL's with those bytes. */

#define _POSIX_C_SOURCE 200809L

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>

#include <cmocka.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <tss2/tss2_esys.h>
#include <tss2/tss2_mu.h>
#include <tss2/tss2_tctildr.h>

#include "azure/iot/az_iot_dps_tpm_client.h"
#include "sample_tpm_tss.h"

/** @brief swtpm executable; set by CMake. */
#ifndef SAMPLE_TPM_TSS_TEST_SWTPM
#define SAMPLE_TPM_TSS_TEST_SWTPM "swtpm"
#endif

#define MESSAGE "0ne00000001%2fregistrations%2fdevice\n1700003600"
#define DPS_KEY_MAX 4096
#define HANDLE_FOR_TESTS 0x81000100u
#define HANDLE_NEVER_IMPORTED 0x81000101u

extern char** environ;

/** @brief The swtpm process every test talks to, and its TCTI configuration. */
static struct
{
  pid_t pid;
  char state_dir[64];
  char tcti[64];
} swtpm;

/** @brief Whether something accepts TCP connections on 127.0.0.1:@p port. */
static bool port_accepts(uint16_t port)
{
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
  {
    return false;
  }
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  bool ok = connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0;
  close(fd);
  return ok;
}

static int swtpm_start(void** state)
{
  (void)state;
  /* A port pair per process, so parallel runs do not collide, below Linux's
   * ephemeral range (32768+) so no client socket in TIME_WAIT holds it. */
  uint16_t port = (uint16_t)(12000 + (getpid() % 10000) * 2);
  snprintf(swtpm.state_dir, sizeof(swtpm.state_dir), "/tmp/az_iot_tpm_tss_XXXXXX");
  if (mkdtemp(swtpm.state_dir) == NULL)
  {
    return -1;
  }
  char server[64];
  char ctrl[64];
  char tpmstate[96];
  snprintf(server, sizeof(server), "type=tcp,port=%u,bindaddr=127.0.0.1", (unsigned)port);
  snprintf(ctrl, sizeof(ctrl), "type=tcp,port=%u,bindaddr=127.0.0.1", (unsigned)(port + 1));
  snprintf(tpmstate, sizeof(tpmstate), "dir=%s", swtpm.state_dir);
  snprintf(swtpm.tcti, sizeof(swtpm.tcti), "swtpm:host=127.0.0.1,port=%u", (unsigned)port);
  char* argv[] = { (char*)SAMPLE_TPM_TSS_TEST_SWTPM,
                   (char*)"socket",
                   (char*)"--tpm2",
                   (char*)"--server",
                   server,
                   (char*)"--ctrl",
                   ctrl,
                   (char*)"--tpmstate",
                   tpmstate,
                   (char*)"--flags",
                   (char*)"not-need-init,startup-clear",
                   NULL };
  if (posix_spawnp(&swtpm.pid, argv[0], NULL, NULL, argv, environ) != 0)
  {
    fprintf(stderr, "cannot start %s\n", argv[0]);
    return -1;
  }
  for (int i = 0; i < 100; ++i)
  {
    if (port_accepts(port))
    {
      return 0;
    }
    struct timespec wait = { 0, 50 * 1000 * 1000 };
    nanosleep(&wait, NULL);
  }
  fprintf(stderr, "swtpm did not listen on port %u\n", (unsigned)port);
  return -1;
}

static int swtpm_stop(void** state)
{
  (void)state;
  if (swtpm.pid > 0)
  {
    kill(swtpm.pid, SIGTERM);
    waitpid(swtpm.pid, NULL, 0);
  }
  char command[96];
  snprintf(command, sizeof(command), "rm -rf %s", swtpm.state_dir);
  return system(command) == 0 ? 0 : -1;
}

/**
 * @brief What a test has open on the swtpm. swtpm serves one client at a time
 * and has no resource manager, so connections_release() closes these and
 * flushes leftover transient objects even when an assertion ends a test early.
 */
static struct
{
  sample_tpm_tss* tpm;
  ESYS_CONTEXT* esys;
  TSS2_TCTI_CONTEXT* tcti;
  ESYS_TR session; /**< The test's auth session on esys, if any. */
} opened = { .session = ESYS_TR_NONE };

static sample_tpm_tss* open_tpm(uint32_t hmac_key_handle)
{
  assert_null(opened.tpm);
  opened.tpm = sample_tpm_tss_create(swtpm.tcti, hmac_key_handle);
  assert_non_null(opened.tpm);
  return opened.tpm;
}

/** @brief Closes what open_tpm() opened. */
static void close_tpm(void)
{
  sample_tpm_tss_destroy(opened.tpm);
  opened.tpm = NULL;
}

/** @brief Opens an ESAPI context on the swtpm, for the test's own commands. */
static ESYS_CONTEXT* esys_open(void)
{
  assert_null(opened.esys);
  assert_int_equal(Tss2_TctiLdr_Initialize(swtpm.tcti, &opened.tcti), TSS2_RC_SUCCESS);
  assert_int_equal(Esys_Initialize(&opened.esys, opened.tcti, NULL), TSS2_RC_SUCCESS);
  return opened.esys;
}

/** @brief Flushes the session duplicate_policy() started. */
static void session_flush(void)
{
  if (opened.session != ESYS_TR_NONE)
  {
    (void)Esys_FlushContext(opened.esys, opened.session);
    opened.session = ESYS_TR_NONE;
  }
}

/** @brief Closes what esys_open() opened. */
static void esys_close(void)
{
  session_flush();
  if (opened.esys != NULL)
  {
    Esys_Finalize(&opened.esys);
  }
  if (opened.tcti != NULL)
  {
    Tss2_TctiLdr_Finalize(&opened.tcti);
  }
}

/** @brief Flushes every transient object the swtpm holds. */
static void flush_transient_objects(ESYS_CONTEXT* esys)
{
  TPMI_YES_NO more = TPM2_NO;
  TPMS_CAPABILITY_DATA* caps = NULL;
  if (Esys_GetCapability(
          esys,
          ESYS_TR_NONE,
          ESYS_TR_NONE,
          ESYS_TR_NONE,
          TPM2_CAP_HANDLES,
          TPM2_TRANSIENT_FIRST,
          TPM2_MAX_CAP_HANDLES,
          &more,
          &caps)
      != TSS2_RC_SUCCESS)
  {
    return;
  }
  for (uint32_t i = 0; i < caps->data.handles.count; ++i)
  {
    ESYS_TR object = ESYS_TR_NONE;
    if (Esys_TR_FromTPMPublic(
            esys, caps->data.handles.handle[i], ESYS_TR_NONE, ESYS_TR_NONE, ESYS_TR_NONE, &object)
        == TSS2_RC_SUCCESS)
    {
      (void)Esys_FlushContext(esys, object);
    }
  }
  Esys_Free(caps);
}

/** @brief Per-test teardown: releases whatever the test left open. */
static int connections_release(void** state)
{
  (void)state;
  close_tpm();
  esys_close();
  if (Tss2_TctiLdr_Initialize(swtpm.tcti, &opened.tcti) == TSS2_RC_SUCCESS
      && Esys_Initialize(&opened.esys, opened.tcti, NULL) == TSS2_RC_SUCCESS)
  {
    flush_transient_objects(opened.esys);
  }
  esys_close();
  return 0;
}

static void expected_hmac(
    const uint8_t* key,
    size_t key_len,
    const char* message,
    uint8_t out[AZ_IOT_SHA256_SIZE])
{
  unsigned int len = 0;
  assert_non_null(
      HMAC(EVP_sha256(), key, (int)key_len, (const uint8_t*)message, strlen(message), out, &len));
  assert_int_equal(len, AZ_IOT_SHA256_SIZE);
}

/**
 * @brief The digest of PolicyCommandCode(TPM2_CC_Duplicate), from a session of
 * @p type. The session is tracked in #opened: release it with session_flush().
 */
static TPM2B_DIGEST duplicate_policy(ESYS_CONTEXT* esys, TPM2_SE type, ESYS_TR* session)
{
  assert_int_equal(opened.session, ESYS_TR_NONE);
  TPMT_SYM_DEF none = { .algorithm = TPM2_ALG_NULL };
  assert_int_equal(
      Esys_StartAuthSession(
          esys,
          ESYS_TR_NONE,
          ESYS_TR_NONE,
          ESYS_TR_NONE,
          ESYS_TR_NONE,
          ESYS_TR_NONE,
          NULL,
          type,
          &none,
          TPM2_ALG_SHA256,
          &opened.session),
      TSS2_RC_SUCCESS);
  *session = opened.session;
  assert_int_equal(
      Esys_PolicyCommandCode(
          esys, *session, ESYS_TR_NONE, ESYS_TR_NONE, ESYS_TR_NONE, TPM2_CC_Duplicate),
      TSS2_RC_SUCCESS);
  TPM2B_DIGEST* digest = NULL;
  assert_int_equal(
      Esys_PolicyGetDigest(esys, *session, ESYS_TR_NONE, ESYS_TR_NONE, ESYS_TR_NONE, &digest),
      TSS2_RC_SUCCESS);
  TPM2B_DIGEST out = *digest;
  Esys_Free(digest);
  return out;
}

/** @brief Loads a public area into the null hierarchy. */
static ESYS_TR load_public(ESYS_CONTEXT* esys, const uint8_t* pub, size_t pub_len)
{
  TPM2B_PUBLIC unmarshalled;
  memset(&unmarshalled, 0, sizeof(unmarshalled));
  size_t offset = 0;
  assert_int_equal(
      Tss2_MU_TPM2B_PUBLIC_Unmarshal(pub, pub_len, &offset, &unmarshalled), TSS2_RC_SUCCESS);
  ESYS_TR handle = ESYS_TR_NONE;
  assert_int_equal(
      Esys_LoadExternal(
          esys,
          ESYS_TR_NONE,
          ESYS_TR_NONE,
          ESYS_TR_NONE,
          NULL,
          &unmarshalled,
          ESYS_TR_RH_NULL,
          &handle),
      TSS2_RC_SUCCESS);
  return handle;
}

/**
 * @brief Builds a DPS `authenticationKey` (decoded) holding the HMAC key
 * @p hmac_key (HMAC scheme hashing with @p hmac_hash), for the TPM whose EK and
 * SRK public areas are @p ek and @p srk.
 */
static size_t build_dps_key(
    const uint8_t* ek,
    size_t ek_len,
    const uint8_t* srk,
    size_t srk_len,
    TPMI_ALG_HASH hmac_hash,
    const uint8_t* hmac_key,
    size_t hmac_key_len,
    uint8_t out[DPS_KEY_MAX])
{
  ESYS_CONTEXT* esys = esys_open();

  ESYS_TR ek_handle = load_public(esys, ek, ek_len);
  ESYS_TR srk_handle = load_public(esys, srk, srk_len);
  TPM2B_NAME* srk_name = NULL;
  assert_int_equal(Esys_TR_GetName(esys, srk_handle, &srk_name), TSS2_RC_SUCCESS);

  /* The inner wrapping key, sent to the EK as a credential. */
  TPM2B_DIGEST inner = { .size = 16 };
  for (uint16_t i = 0; i < inner.size; ++i)
  {
    inner.buffer[i] = (uint8_t)(0xA0 + i);
  }
  TPM2B_ID_OBJECT* blob = NULL;
  TPM2B_ENCRYPTED_SECRET* secret = NULL;
  assert_int_equal(
      Esys_MakeCredential(
          esys,
          ek_handle,
          ESYS_TR_NONE,
          ESYS_TR_NONE,
          ESYS_TR_NONE,
          &inner,
          srk_name,
          &blob,
          &secret),
      TSS2_RC_SUCCESS);

  /* swtpm holds three loaded objects: the EK is no longer needed. */
  assert_int_equal(Esys_FlushContext(esys, ek_handle), TSS2_RC_SUCCESS);

  /* An HMAC key of known bytes under a throwaway parent, duplicable by policy. */
  TPM2B_PUBLIC parent_template = { 0 };
  parent_template.publicArea.type = TPM2_ALG_RSA;
  parent_template.publicArea.nameAlg = TPM2_ALG_SHA256;
  parent_template.publicArea.objectAttributes = TPMA_OBJECT_FIXEDTPM | TPMA_OBJECT_FIXEDPARENT
      | TPMA_OBJECT_SENSITIVEDATAORIGIN | TPMA_OBJECT_USERWITHAUTH | TPMA_OBJECT_RESTRICTED
      | TPMA_OBJECT_DECRYPT;
  parent_template.publicArea.parameters.rsaDetail.symmetric.algorithm = TPM2_ALG_AES;
  parent_template.publicArea.parameters.rsaDetail.symmetric.keyBits.aes = 128;
  parent_template.publicArea.parameters.rsaDetail.symmetric.mode.aes = TPM2_ALG_CFB;
  parent_template.publicArea.parameters.rsaDetail.scheme.scheme = TPM2_ALG_NULL;
  parent_template.publicArea.parameters.rsaDetail.keyBits = 2048;
  TPM2B_SENSITIVE_CREATE no_sensitive = { 0 };
  TPM2B_DATA no_outside_info = { 0 };
  TPML_PCR_SELECTION no_pcrs = { 0 };
  ESYS_TR parent = ESYS_TR_NONE;
  assert_int_equal(
      Esys_CreatePrimary(
          esys,
          ESYS_TR_RH_NULL,
          ESYS_TR_PASSWORD,
          ESYS_TR_NONE,
          ESYS_TR_NONE,
          &no_sensitive,
          &parent_template,
          &no_outside_info,
          &no_pcrs,
          &parent,
          NULL,
          NULL,
          NULL,
          NULL),
      TSS2_RC_SUCCESS);

  ESYS_TR trial = ESYS_TR_NONE;
  TPM2B_PUBLIC key_template = { 0 };
  key_template.publicArea.type = TPM2_ALG_KEYEDHASH;
  key_template.publicArea.nameAlg = TPM2_ALG_SHA256;
  key_template.publicArea.objectAttributes = TPMA_OBJECT_USERWITHAUTH | TPMA_OBJECT_SIGN_ENCRYPT;
  key_template.publicArea.authPolicy = duplicate_policy(esys, TPM2_SE_TRIAL, &trial);
  session_flush();
  key_template.publicArea.parameters.keyedHashDetail.scheme.scheme = TPM2_ALG_HMAC;
  key_template.publicArea.parameters.keyedHashDetail.scheme.details.hmac.hashAlg = hmac_hash;
  TPM2B_SENSITIVE_CREATE key_sensitive = { 0 };
  assert_true(hmac_key_len <= sizeof(key_sensitive.sensitive.data.buffer));
  key_sensitive.sensitive.data.size = (UINT16)hmac_key_len;
  memcpy(key_sensitive.sensitive.data.buffer, hmac_key, hmac_key_len);
  TPM2B_PRIVATE* key_private = NULL;
  TPM2B_PUBLIC* key_public = NULL;
  assert_int_equal(
      Esys_Create(
          esys,
          parent,
          ESYS_TR_PASSWORD,
          ESYS_TR_NONE,
          ESYS_TR_NONE,
          &key_sensitive,
          &key_template,
          &no_outside_info,
          &no_pcrs,
          &key_private,
          &key_public,
          NULL,
          NULL,
          NULL),
      TSS2_RC_SUCCESS);
  ESYS_TR key = ESYS_TR_NONE;
  assert_int_equal(
      Esys_Load(
          esys,
          parent,
          ESYS_TR_PASSWORD,
          ESYS_TR_NONE,
          ESYS_TR_NONE,
          key_private,
          key_public,
          &key),
      TSS2_RC_SUCCESS);

  /* Duplicated to the SRK with the inner key, as DPS sends it. */
  ESYS_TR policy = ESYS_TR_NONE;
  (void)duplicate_policy(esys, TPM2_SE_POLICY, &policy);
  TPM2B_DATA inner_key = { .size = inner.size };
  memcpy(inner_key.buffer, inner.buffer, inner.size);
  TPMT_SYM_DEF_OBJECT inner_symmetric
      = { .algorithm = TPM2_ALG_AES, .keyBits.aes = 128, .mode.aes = TPM2_ALG_CFB };
  TPM2B_DATA* key_out = NULL;
  TPM2B_PRIVATE* duplicate = NULL;
  TPM2B_ENCRYPTED_SECRET* seed = NULL;
  assert_int_equal(
      Esys_Duplicate(
          esys,
          key,
          srk_handle,
          policy,
          ESYS_TR_NONE,
          ESYS_TR_NONE,
          &inner_key,
          &inner_symmetric,
          &key_out,
          &duplicate,
          &seed),
      TSS2_RC_SUCCESS);

  size_t len = 0;
  assert_int_equal(Tss2_MU_TPM2B_ID_OBJECT_Marshal(blob, out, DPS_KEY_MAX, &len), TSS2_RC_SUCCESS);
  assert_int_equal(
      Tss2_MU_TPM2B_ENCRYPTED_SECRET_Marshal(secret, out, DPS_KEY_MAX, &len), TSS2_RC_SUCCESS);
  assert_int_equal(
      Tss2_MU_TPM2B_PRIVATE_Marshal(duplicate, out, DPS_KEY_MAX, &len), TSS2_RC_SUCCESS);
  assert_int_equal(
      Tss2_MU_TPM2B_ENCRYPTED_SECRET_Marshal(seed, out, DPS_KEY_MAX, &len), TSS2_RC_SUCCESS);
  assert_int_equal(
      Tss2_MU_TPM2B_PUBLIC_Marshal(key_public, out, DPS_KEY_MAX, &len), TSS2_RC_SUCCESS);

  Esys_Free(srk_name);
  Esys_Free(blob);
  Esys_Free(secret);
  Esys_Free(key_private);
  Esys_Free(key_public);
  Esys_Free(key_out);
  Esys_Free(duplicate);
  Esys_Free(seed);
  session_flush();
  (void)Esys_FlushContext(esys, key);
  (void)Esys_FlushContext(esys, parent);
  (void)Esys_FlushContext(esys, srk_handle);
  esys_close();
  return len;
}

/**
 * @brief A DPS key for the swtpm's EK and SRK, whose HMAC scheme hashes with
 * @p hmac_hash. swtpm serves one client at a time, so the backend is closed
 * while the key is built.
 */
static size_t dps_key_for_tpm_with_hash(
    TPMI_ALG_HASH hmac_hash,
    const uint8_t* hmac_key,
    size_t hmac_key_len,
    uint8_t out[DPS_KEY_MAX])
{
  sample_tpm_tss* tpm = open_tpm(HANDLE_FOR_TESTS);
  static uint8_t ek[TPM2_MAX_COMMAND_SIZE];
  static uint8_t srk[TPM2_MAX_COMMAND_SIZE];
  const uint8_t* pub = NULL;
  size_t ek_len = 0;
  size_t srk_len = 0;
  sample_tpm_tss_ek_public(tpm, &pub, &ek_len);
  memcpy(ek, pub, ek_len);
  sample_tpm_tss_srk_public(tpm, &pub, &srk_len);
  memcpy(srk, pub, srk_len);
  close_tpm();
  return build_dps_key(ek, ek_len, srk, srk_len, hmac_hash, hmac_key, hmac_key_len, out);
}

/** @brief dps_key_for_tpm_with_hash() for an HMAC-SHA256 key, as DPS sends. */
static size_t dps_key_for_tpm(
    const uint8_t* hmac_key,
    size_t hmac_key_len,
    uint8_t out[DPS_KEY_MAX])
{
  return dps_key_for_tpm_with_hash(TPM2_ALG_SHA256, hmac_key, hmac_key_len, out);
}

static void public_areas_are_rsa2048_ek_and_srk(void** state)
{
  (void)state;
  sample_tpm_tss* tpm = open_tpm(HANDLE_FOR_TESTS);
  const uint8_t* pub = NULL;
  size_t len = 0;
  sample_tpm_tss_ek_public(tpm, &pub, &len);
  TPM2B_PUBLIC ek;
  memset(&ek, 0, sizeof(ek));
  size_t offset = 0;
  assert_int_equal(Tss2_MU_TPM2B_PUBLIC_Unmarshal(pub, len, &offset, &ek), TSS2_RC_SUCCESS);
  assert_int_equal(offset, len);
  assert_int_equal(ek.publicArea.type, TPM2_ALG_RSA);
  assert_int_equal(ek.publicArea.objectAttributes, 0x000300B2u); /* template L-1 */
  assert_int_equal(ek.publicArea.authPolicy.size, AZ_IOT_SHA256_SIZE);
  assert_int_equal(ek.publicArea.unique.rsa.size, 256);

  sample_tpm_tss_srk_public(tpm, &pub, &len);
  TPM2B_PUBLIC srk;
  memset(&srk, 0, sizeof(srk));
  offset = 0;
  assert_int_equal(Tss2_MU_TPM2B_PUBLIC_Unmarshal(pub, len, &offset, &srk), TSS2_RC_SUCCESS);
  assert_int_equal(srk.publicArea.type, TPM2_ALG_RSA);
  assert_int_equal(srk.publicArea.objectAttributes, 0x00030472u);
  assert_int_equal(srk.publicArea.unique.rsa.size, 256);
  close_tpm();

  /* Primaries are deterministic: the same EK, so the same registration ID, next time. */
  tpm = open_tpm(HANDLE_FOR_TESTS);
  const uint8_t* again = NULL;
  size_t again_len = 0;
  sample_tpm_tss_ek_public(tpm, &again, &again_len);
  TPM2B_PUBLIC ek_again;
  memset(&ek_again, 0, sizeof(ek_again));
  offset = 0;
  assert_int_equal(
      Tss2_MU_TPM2B_PUBLIC_Unmarshal(again, again_len, &offset, &ek_again), TSS2_RC_SUCCESS);
  assert_memory_equal(ek_again.publicArea.unique.rsa.buffer, ek.publicArea.unique.rsa.buffer, 256);
  close_tpm();
}

static void hmac_fails_before_a_key_is_imported(void** state)
{
  (void)state;
  sample_tpm_tss* tpm = open_tpm(HANDLE_NEVER_IMPORTED);
  uint8_t mac[AZ_IOT_SHA256_SIZE];
  assert_false(sample_tpm_tss_hmac(tpm, (const uint8_t*)MESSAGE, strlen(MESSAGE), mac));
  az_iot_dps_tpm_ops ops = sample_tpm_tss_ops(tpm);
  assert_int_equal(
      ops.hmac_sha256(ops.ctx, (const uint8_t*)MESSAGE, strlen(MESSAGE), mac), AZ_IOT_ERR_INTERNAL);
  close_tpm();
}

static void imports_the_dps_key_and_persists_it(void** state)
{
  (void)state;
  static const uint8_t first[32]
      = { 1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16,
          17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32 };
  static const uint8_t second[32] = { 0xF0, 0xE1, 0xD2, 0xC3, 0xB4, 0xA5, 0x96, 0x87 };
  static uint8_t key[DPS_KEY_MAX];
  uint8_t expected[AZ_IOT_SHA256_SIZE];
  uint8_t mac[AZ_IOT_SHA256_SIZE];

  size_t key_len = dps_key_for_tpm(first, sizeof(first), key);
  sample_tpm_tss* tpm = open_tpm(HANDLE_FOR_TESTS);
  az_iot_dps_tpm_ops ops = sample_tpm_tss_ops(tpm);
  assert_int_equal(ops.import_key(ops.ctx, key, key_len), AZ_IOT_OK);
  assert_int_equal(
      ops.hmac_sha256(ops.ctx, (const uint8_t*)MESSAGE, strlen(MESSAGE), mac), AZ_IOT_OK);
  expected_hmac(first, sizeof(first), MESSAGE, expected);
  assert_memory_equal(mac, expected, sizeof(mac));
  close_tpm();

  /* Persisted: a new session signs with it without importing. */
  tpm = open_tpm(HANDLE_FOR_TESTS);
  assert_true(sample_tpm_tss_hmac(tpm, (const uint8_t*)MESSAGE, strlen(MESSAGE), mac));
  assert_memory_equal(mac, expected, sizeof(mac));
  close_tpm();

  /* A later key (re-registration, or the hub key) replaces it. */
  key_len = dps_key_for_tpm(second, sizeof(second), key);
  tpm = open_tpm(HANDLE_FOR_TESTS);
  ops = sample_tpm_tss_ops(tpm);
  assert_int_equal(ops.import_key(ops.ctx, key, key_len), AZ_IOT_OK);
  assert_true(sample_tpm_tss_hmac(tpm, (const uint8_t*)MESSAGE, strlen(MESSAGE), mac));
  expected_hmac(second, sizeof(second), MESSAGE, expected);
  assert_memory_equal(mac, expected, sizeof(mac));
  close_tpm();
}

static void a_tampered_or_malformed_key_is_refused(void** state)
{
  (void)state;
  static const uint8_t bytes[32] = { 0x55 };
  static uint8_t key[DPS_KEY_MAX];
  size_t key_len = dps_key_for_tpm(bytes, sizeof(bytes), key);
  sample_tpm_tss* tpm = open_tpm(HANDLE_FOR_TESTS);
  az_iot_dps_tpm_ops ops = sample_tpm_tss_ops(tpm);

  key[2 + 2 + 4] ^= 0x01; /* inside the credential blob's integrity HMAC */
  assert_int_equal(ops.import_key(ops.ctx, key, key_len), AZ_IOT_ERR_AUTH);
  key[2 + 2 + 4] ^= 0x01;
  assert_int_equal(ops.import_key(ops.ctx, key, key_len - 1), AZ_IOT_ERR_PROTOCOL);
  assert_int_equal(ops.import_key(ops.ctx, key, 3), AZ_IOT_ERR_PROTOCOL);
  assert_int_equal(ops.import_key(NULL, key, key_len), AZ_IOT_ERR_PROTOCOL);
  close_tpm();
}

/**
 * @brief Provisions a primary at persistent @p handle, as a TPM provisioning
 * tool would: CreatePrimary from the template of @p pub, then EvictControl.
 * @p unique_marker seeds `unique`, so the key differs from the backend's
 * zero-unique fallback; its public area goes to @p out_pub / @p out_len.
 */
static void persist_primary(
    ESYS_CONTEXT* esys,
    ESYS_TR hierarchy,
    const uint8_t* pub,
    size_t pub_len,
    uint8_t unique_marker,
    uint32_t handle,
    uint8_t out_pub[TPM2_MAX_COMMAND_SIZE],
    size_t* out_len)
{
  TPM2B_PUBLIC key_template;
  memset(&key_template, 0, sizeof(key_template));
  size_t offset = 0;
  assert_int_equal(
      Tss2_MU_TPM2B_PUBLIC_Unmarshal(pub, pub_len, &offset, &key_template), TSS2_RC_SUCCESS);
  memset(key_template.publicArea.unique.rsa.buffer, 0, key_template.publicArea.unique.rsa.size);
  key_template.publicArea.unique.rsa.buffer[0] = unique_marker;
  key_template.size = 0;
  TPM2B_SENSITIVE_CREATE no_sensitive = { 0 };
  TPM2B_DATA no_outside_info = { 0 };
  TPML_PCR_SELECTION no_pcrs = { 0 };
  ESYS_TR primary = ESYS_TR_NONE;
  TPM2B_PUBLIC* created = NULL;
  assert_int_equal(
      Esys_CreatePrimary(
          esys,
          hierarchy,
          ESYS_TR_PASSWORD,
          ESYS_TR_NONE,
          ESYS_TR_NONE,
          &no_sensitive,
          &key_template,
          &no_outside_info,
          &no_pcrs,
          &primary,
          &created,
          NULL,
          NULL,
          NULL),
      TSS2_RC_SUCCESS);
  size_t offset_out = 0;
  assert_int_equal(
      Tss2_MU_TPM2B_PUBLIC_Marshal(created, out_pub, TPM2_MAX_COMMAND_SIZE, &offset_out),
      TSS2_RC_SUCCESS);
  *out_len = offset_out;
  Esys_Free(created);
  ESYS_TR persistent = ESYS_TR_NONE;
  assert_int_equal(
      Esys_EvictControl(
          esys,
          ESYS_TR_RH_OWNER,
          primary,
          ESYS_TR_PASSWORD,
          ESYS_TR_NONE,
          ESYS_TR_NONE,
          handle,
          &persistent),
      TSS2_RC_SUCCESS);
  assert_int_equal(Esys_FlushContext(esys, primary), TSS2_RC_SUCCESS);
  assert_int_equal(Esys_TR_Close(esys, &persistent), TSS2_RC_SUCCESS);
}

/** @brief Removes the persistent object at @p handle. */
static void evict_persistent(ESYS_CONTEXT* esys, uint32_t handle)
{
  ESYS_TR persistent = ESYS_TR_NONE;
  assert_int_equal(
      Esys_TR_FromTPMPublic(esys, handle, ESYS_TR_NONE, ESYS_TR_NONE, ESYS_TR_NONE, &persistent),
      TSS2_RC_SUCCESS);
  ESYS_TR none = ESYS_TR_NONE;
  assert_int_equal(
      Esys_EvictControl(
          esys,
          ESYS_TR_RH_OWNER,
          persistent,
          ESYS_TR_PASSWORD,
          ESYS_TR_NONE,
          ESYS_TR_NONE,
          handle,
          &none),
      TSS2_RC_SUCCESS);
}

static void a_key_that_cannot_sign_sha256_is_refused_and_the_old_key_kept(void** state)
{
  (void)state;
  static const uint8_t current[32] = { 0x11, 0x22, 0x33 };
  static const uint8_t sha512_key[32] = { 0x44, 0x55, 0x66 };
  static uint8_t key[DPS_KEY_MAX];
  uint8_t expected[AZ_IOT_SHA256_SIZE];
  uint8_t mac[AZ_IOT_SHA256_SIZE];

  size_t key_len = dps_key_for_tpm(current, sizeof(current), key);
  sample_tpm_tss* tpm = open_tpm(HANDLE_FOR_TESTS);
  az_iot_dps_tpm_ops ops = sample_tpm_tss_ops(tpm);
  assert_int_equal(ops.import_key(ops.ctx, key, key_len), AZ_IOT_OK);
  close_tpm();

  key_len = dps_key_for_tpm_with_hash(TPM2_ALG_SHA512, sha512_key, sizeof(sha512_key), key);
  tpm = open_tpm(HANDLE_FOR_TESTS);
  ops = sample_tpm_tss_ops(tpm);
  assert_int_equal(ops.import_key(ops.ctx, key, key_len), AZ_IOT_ERR_PROTOCOL);
  assert_true(sample_tpm_tss_hmac(tpm, (const uint8_t*)MESSAGE, strlen(MESSAGE), mac));
  expected_hmac(current, sizeof(current), MESSAGE, expected);
  assert_memory_equal(mac, expected, sizeof(mac));
}

/** @brief Whether the swtpm holds an object at persistent @p handle. */
static bool persistent_exists(ESYS_CONTEXT* esys, uint32_t handle)
{
  TPMI_YES_NO more = TPM2_NO;
  TPMS_CAPABILITY_DATA* caps = NULL;
  assert_int_equal(
      Esys_GetCapability(
          esys,
          ESYS_TR_NONE,
          ESYS_TR_NONE,
          ESYS_TR_NONE,
          TPM2_CAP_HANDLES,
          handle,
          1,
          &more,
          &caps),
      TSS2_RC_SUCCESS);
  bool found = caps->data.handles.count == 1 && caps->data.handles.handle[0] == handle;
  Esys_Free(caps);
  return found;
}

/** @brief Asserts that @p tpm reports the public areas @p ek and @p srk. */
static void assert_publics(
    const sample_tpm_tss* tpm,
    const uint8_t* ek,
    size_t ek_len,
    const uint8_t* srk,
    size_t srk_len)
{
  const uint8_t* pub = NULL;
  size_t len = 0;
  sample_tpm_tss_ek_public(tpm, &pub, &len);
  assert_int_equal(len, ek_len);
  assert_memory_equal(pub, ek, ek_len);
  sample_tpm_tss_srk_public(tpm, &pub, &len);
  assert_int_equal(len, srk_len);
  assert_memory_equal(pub, srk, srk_len);
}

static void provisioned_ek_and_srk_are_used_and_kept(void** state)
{
  (void)state;
  static uint8_t fallback_ek[TPM2_MAX_COMMAND_SIZE];
  static uint8_t fallback_srk[TPM2_MAX_COMMAND_SIZE];
  static uint8_t ek[TPM2_MAX_COMMAND_SIZE];
  static uint8_t srk[TPM2_MAX_COMMAND_SIZE];
  static const uint8_t bytes[32] = { 0x3C, 0x5A, 0x96, 0x69 };
  static uint8_t key[DPS_KEY_MAX];
  uint8_t expected[AZ_IOT_SHA256_SIZE];
  uint8_t mac[AZ_IOT_SHA256_SIZE];
  const uint8_t* pub = NULL;
  size_t fallback_ek_len = 0;
  size_t fallback_srk_len = 0;
  size_t ek_len = 0;
  size_t srk_len = 0;

  sample_tpm_tss* tpm = open_tpm(HANDLE_FOR_TESTS);
  sample_tpm_tss_ek_public(tpm, &pub, &fallback_ek_len);
  memcpy(fallback_ek, pub, fallback_ek_len);
  sample_tpm_tss_srk_public(tpm, &pub, &fallback_srk_len);
  memcpy(fallback_srk, pub, fallback_srk_len);
  close_tpm();

  /* Provisioned primaries differ from the fallback ones (non-zero unique), so
   * a backend that re-creates them instead of using the handles fails below. */
  ESYS_CONTEXT* esys = esys_open();
  persist_primary(
      esys,
      ESYS_TR_RH_ENDORSEMENT,
      fallback_ek,
      fallback_ek_len,
      0xE5,
      SAMPLE_TPM_TSS_EK_HANDLE,
      ek,
      &ek_len);
  persist_primary(
      esys,
      ESYS_TR_RH_OWNER,
      fallback_srk,
      fallback_srk_len,
      0x55,
      SAMPLE_TPM_TSS_SRK_HANDLE,
      srk,
      &srk_len);
  esys_close();
  assert_true(ek_len != fallback_ek_len || memcmp(ek, fallback_ek, ek_len) != 0);
  assert_true(srk_len != fallback_srk_len || memcmp(srk, fallback_srk, srk_len) != 0);

  size_t key_len
      = build_dps_key(ek, ek_len, srk, srk_len, TPM2_ALG_SHA256, bytes, sizeof(bytes), key);
  tpm = open_tpm(HANDLE_FOR_TESTS);
  assert_publics(tpm, ek, ek_len, srk, srk_len);
  az_iot_dps_tpm_ops ops = sample_tpm_tss_ops(tpm);
  assert_int_equal(ops.import_key(ops.ctx, key, key_len), AZ_IOT_OK);
  assert_true(sample_tpm_tss_hmac(tpm, (const uint8_t*)MESSAGE, strlen(MESSAGE), mac));
  expected_hmac(bytes, sizeof(bytes), MESSAGE, expected);
  assert_memory_equal(mac, expected, sizeof(mac));
  close_tpm();

  /* Destroy releases the persistent primaries without evicting them. */
  esys = esys_open();
  assert_true(persistent_exists(esys, SAMPLE_TPM_TSS_EK_HANDLE));
  assert_true(persistent_exists(esys, SAMPLE_TPM_TSS_SRK_HANDLE));
  esys_close();
  tpm = open_tpm(HANDLE_FOR_TESTS);
  assert_publics(tpm, ek, ek_len, srk, srk_len);
  assert_true(sample_tpm_tss_hmac(tpm, (const uint8_t*)MESSAGE, strlen(MESSAGE), mac));
  assert_memory_equal(mac, expected, sizeof(mac));
  close_tpm();

  esys = esys_open();
  evict_persistent(esys, SAMPLE_TPM_TSS_EK_HANDLE);
  evict_persistent(esys, SAMPLE_TPM_TSS_SRK_HANDLE);
  esys_close();
}

static void create_rejects_a_non_persistent_or_reserved_handle(void** state)
{
  (void)state;
  assert_null(sample_tpm_tss_create(swtpm.tcti, 0x80000000u));
  assert_null(sample_tpm_tss_create(swtpm.tcti, 0x81800000u));
  assert_null(sample_tpm_tss_create(swtpm.tcti, SAMPLE_TPM_TSS_EK_HANDLE));
  assert_null(sample_tpm_tss_create(swtpm.tcti, SAMPLE_TPM_TSS_SRK_HANDLE));
  sample_tpm_tss_destroy(NULL);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test_teardown(
        create_rejects_a_non_persistent_or_reserved_handle, connections_release),
    cmocka_unit_test_teardown(public_areas_are_rsa2048_ek_and_srk, connections_release),
    cmocka_unit_test_teardown(hmac_fails_before_a_key_is_imported, connections_release),
    cmocka_unit_test_teardown(imports_the_dps_key_and_persists_it, connections_release),
    cmocka_unit_test_teardown(a_tampered_or_malformed_key_is_refused, connections_release),
    cmocka_unit_test_teardown(
        a_key_that_cannot_sign_sha256_is_refused_and_the_old_key_kept, connections_release),
    cmocka_unit_test_teardown(provisioned_ek_and_srk_are_used_and_kept, connections_release),
  };
  return cmocka_run_group_tests(tests, swtpm_start, swtpm_stop);
}
