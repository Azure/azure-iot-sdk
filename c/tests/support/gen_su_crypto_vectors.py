#!/usr/bin/env python3
# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

"""Generate tests/support/su_crypto_vectors.h, the software updates crypto test vectors.

The output is committed; tests never run this. Keys are generated fresh on each run, so
regenerating rewrites every RSA vector. Requires the `cryptography` package.

    python3 c/tests/support/gen_su_crypto_vectors.py > c/tests/support/su_crypto_vectors.h
"""

import base64
import hashlib
import json
import sys

from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric import padding, rsa

# SHA-256 known answers from FIPS 180-4 / NIST CAVP. (name, message, repeat, digest hex)
SHA256_KAT = [
    ("empty", b"", 1, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"),
    ("abc", b"abc", 1, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"),
    (
        "448_bit",
        b"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
        1,
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
    ),
    (
        "896_bit",
        b"abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopq"
        b"klmnopqrlmnopqrsmnopqrstnopqrstu",
        1,
        "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1",
    ),
    ("million_a", b"a", 1000000, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"),
]

SHA256_DIGEST_INFO = bytes.fromhex("3031300d060960864801650304020105000420")

MESSAGE = (
    b"eyJhbGciOiJSUzI1NiIsInNqd2siOiJ0ZXN0In0."
    b"eyJzaGEyNTYiOiJNMnB1ZjVlTVBHNnZIT0RoQ1NXM3VCa2ZsM3pRd0VQZ0RHeHBHMWo0R0lNPSJ9"
)

# Deterministic file payload for the file-hash check; mirrored by su_crypto_file_byte() in C.
FILE_PAYLOAD_LEN = 4099


def file_payload():
    return bytes((i * 7 + 3) & 0xFF for i in range(FILE_PAYLOAD_LEN))


def i2b(x, length=None):
    if length is None:
        length = max(1, (x.bit_length() + 7) // 8)
    return x.to_bytes(length, "big")


def b64url(b):
    return base64.urlsafe_b64encode(b).rstrip(b"=").decode()


def key_ne(key):
    pub = key.public_key().public_numbers()
    return i2b(pub.n), i2b(pub.e)


def raw_sign(key, em):
    """RSA private operation over an arbitrary encoded message (for malformed paddings)."""
    priv = key.private_numbers()
    n = priv.public_numbers.n
    k = (n.bit_length() + 7) // 8
    assert len(em) == k
    return i2b(pow(int.from_bytes(em, "big"), priv.d, n), k)


def modulus_len(key):
    return (key.key_size + 7) // 8


def pkcs1_em(key, t, block_type=0x01, trailing=b""):
    k = modulus_len(key)
    ps_len = k - 3 - len(t) - len(trailing)
    assert ps_len >= 8
    fill = b"\xff" * ps_len if block_type == 0x01 else b"\x5a" * ps_len
    return b"\x00" + bytes([block_type]) + fill + b"\x00" + t + trailing


def rs256(key, data):
    return key.sign(data, padding.PKCS1v15(), hashes.SHA256())


def flip(b, idx, mask=0x01):
    b = bytearray(b)
    b[idx] ^= mask
    return bytes(b)


def c_bytes(name, data):
    if len(data) == 0:
        return f"static const uint8_t {name}[1] = {{ 0x00 }}; /* zero length */\n"
    lines = []
    for i in range(0, len(data), 12):
        lines.append("  " + ", ".join(f"0x{x:02x}" for x in data[i : i + 12]) + ",")
    return f"static const uint8_t {name}[{len(data)}] = {{\n" + "\n".join(lines) + "\n};\n"


def c_str(s):
    out = []
    for i in range(0, len(s), 96):
        chunk = s[i : i + 96].replace("\\", "\\\\").replace('"', '\\"')
        out.append(f'  "{chunk}"')
    return "\n".join(out)


class Emitter:
    def __init__(self):
        self.out = []
        self.blobs = {}

    def blob(self, data):
        """Emit @p data once, return its C identifier."""
        key = bytes(data)
        if key not in self.blobs:
            name = f"k_su_vec_blob_{len(self.blobs)}"
            self.blobs[key] = name
            self.out.append(c_bytes(name, key))
        return self.blobs[key]

    def emit(self, text):
        self.out.append(text)


def rs256_row(em, name, n, e, msg, sig):
    return (
        f'  {{ "{name}", {em.blob(n)}, {len(n)}, {em.blob(e)}, {len(e)}, '
        f"{em.blob(msg)}, {len(msg)}, {em.blob(sig)}, {len(sig)} }},"
    )


def gen_rs256(em):
    k2048 = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    k3072 = rsa.generate_private_key(public_exponent=65537, key_size=3072)
    k4096 = rsa.generate_private_key(public_exponent=65537, key_size=4096)
    k2048_e3 = rsa.generate_private_key(public_exponent=3, key_size=2048)
    other = rsa.generate_private_key(public_exponent=65537, key_size=2048)

    n, e = key_ne(k2048)
    n3, e3 = key_ne(k2048_e3)
    sig = rs256(k2048, MESSAGE)
    h = hashlib.sha256(MESSAGE).digest()

    good = []
    for label, key in (
        ("rsa2048_e65537", k2048),
        ("rsa3072_e65537", k3072),
        ("rsa4096_e65537", k4096),
        ("rsa2048_e3", k2048_e3),
    ):
        kn, ke = key_ne(key)
        good.append(rs256_row(em, label, kn, ke, MESSAGE, rs256(key, MESSAGE)))
    good.append(rs256_row(em, "modulus_leading_zero", b"\x00" + n, e, MESSAGE, sig))
    good.append(rs256_row(em, "exponent_leading_zero", n, b"\x00" + e, MESSAGE, sig))

    k = modulus_len(k2048)
    on, oe = key_ne(other)
    bad = [
        rs256_row(em, "signature_first_bit_flipped", n, e, MESSAGE, flip(sig, 0, 0x80)),
        rs256_row(em, "signature_last_bit_flipped", n, e, MESSAGE, flip(sig, k - 1)),
        rs256_row(em, "message_bit_flipped", n, e, flip(MESSAGE, 10), sig),
        rs256_row(em, "message_truncated", n, e, MESSAGE[:-1], sig),
        rs256_row(em, "modulus_bit_flipped", flip(n, k // 2), e, MESSAGE, sig),
        rs256_row(em, "exponent_changed", n, b"\x01\x00\x03", MESSAGE, sig),
        rs256_row(em, "exponent_zero", n, b"\x00", MESSAGE, sig),
        # With e = 1 the encoded message is its own signature; no private key needed.
        rs256_row(em, "exponent_one", n, b"\x01", MESSAGE, pkcs1_em(k2048, SHA256_DIGEST_INFO + h)),
        rs256_row(em, "exponent_even", n, b"\x01\x00\x00", MESSAGE, sig),
        rs256_row(em, "wrong_key", on, oe, MESSAGE, sig),
        rs256_row(em, "signature_truncated", n, e, MESSAGE, sig[:-1]),
        rs256_row(em, "signature_leading_zero", n, e, MESSAGE, b"\x00" + sig),
        rs256_row(em, "signature_trailing_byte", n, e, MESSAGE, sig + b"\x00"),
        rs256_row(em, "signature_equals_modulus", n, e, MESSAGE, n),
        rs256_row(em, "signature_all_ones", n, e, MESSAGE, b"\xff" * k),
        rs256_row(em, "signature_zero", n, e, MESSAGE, b"\x00" * k),
        rs256_row(em, "signature_one", n, e, MESSAGE, b"\x00" * (k - 1) + b"\x01"),
        rs256_row(
            em,
            "pss_sha256",
            n,
            e,
            MESSAGE,
            k2048.sign(
                MESSAGE,
                padding.PSS(mgf=padding.MGF1(hashes.SHA256()), salt_length=32),
                hashes.SHA256(),
            ),
        ),
        rs256_row(em, "pkcs1_sha1", n, e, MESSAGE, k2048.sign(MESSAGE, padding.PKCS1v15(), hashes.SHA1())),
        rs256_row(
            em, "pkcs1_sha384", n, e, MESSAGE, k2048.sign(MESSAGE, padding.PKCS1v15(), hashes.SHA384())
        ),
        rs256_row(
            em, "pkcs1_sha512", n, e, MESSAGE, k2048.sign(MESSAGE, padding.PKCS1v15(), hashes.SHA512())
        ),
        rs256_row(em, "pkcs1_no_digest_info", n, e, MESSAGE, raw_sign(k2048, pkcs1_em(k2048, h))),
        rs256_row(
            em,
            "pkcs1_block_type_2",
            n,
            e,
            MESSAGE,
            raw_sign(k2048, pkcs1_em(k2048, SHA256_DIGEST_INFO + h, block_type=0x02)),
        ),
        rs256_row(
            em,
            "pkcs1_e3_trailing_garbage",
            n3,
            e3,
            MESSAGE,
            raw_sign(k2048_e3, pkcs1_em(k2048_e3, SHA256_DIGEST_INFO + h, trailing=b"\x42" * 32)),
        ),
    ]

    em.emit("/** @brief Known-good RS256 vectors: every row MUST verify. */\n")
    em.emit("static const su_rs256_vector k_su_rs256_good[] = {\n" + "\n".join(good) + "\n};\n\n")
    em.emit("/** @brief Known-bad RS256 vectors: every row MUST be rejected. */\n")
    em.emit("static const su_rs256_vector k_su_rs256_bad[] = {\n" + "\n".join(bad) + "\n};\n\n")


def gen_sha256(em):
    rows = []
    for name, msg, repeat, digest in SHA256_KAT:
        d = bytes.fromhex(digest)
        assert hashlib.sha256(msg * repeat).digest() == d, name
        rows.append(f'  {{ "{name}", {em.blob(msg)}, {len(msg)}, {repeat}, {em.blob(d)} }},')
    em.emit("/** @brief SHA-256 known answers (FIPS 180-4). */\n")
    em.emit("static const su_sha256_vector k_su_sha256_kat[] = {\n" + "\n".join(rows) + "\n};\n\n")


def jws(key, header, payload):
    signing_input = b64url(json.dumps(header, separators=(",", ":")).encode()) + "." + b64url(
        json.dumps(payload, separators=(",", ":")).encode()
    )
    return signing_input, signing_input + "." + b64url(rs256(key, signing_input.encode()))


def manifest_body(file_hash_b64):
    return json.dumps(
        {
            "manifestVersion": "5",
            "updateId": {"provider": "Contoso", "name": "Vectors", "version": "1.0"},
            "compatibility": [{"deviceManufacturer": "Contoso", "deviceModel": "Vectors"}],
            "instructions": {
                "steps": [
                    {
                        "handler": "microsoft/swupdate:1",
                        "files": ["f0"],
                        "handlerProperties": {"installedCriteria": "1.0"},
                    }
                ]
            },
            "files": {
                "f0": {
                    "fileName": "payload.bin",
                    "sizeInBytes": FILE_PAYLOAD_LEN,
                    "hashes": {"sha256": file_hash_b64},
                }
            },
            "createdDateTime": "2026-09-27T00:00:00.0000000Z",
        },
        separators=(",", ":"),
    )


def request(manifest, signature):
    return json.dumps(
        {
            "workflowId": "wf-vectors",
            "updateManifest": manifest,
            "updateManifestSignature": signature,
            "fileUrls": {"f0": "http://example.com/payload.bin"},
        },
        separators=(",", ":"),
    )


def build_chain(root, root_kid, signing, manifest):
    sn, se = key_ne(signing)
    sjwk_input, sjwk = jws(
        root,
        {"alg": "RS256", "kid": root_kid},
        {"kty": "RSA", "n": b64url(sn), "e": b64url(se), "alg": "RS256", "kid": "signing"},
    )
    m_input, m_jws = jws(
        signing,
        {"alg": "RS256", "sjwk": sjwk},
        {"sha256": base64.b64encode(hashlib.sha256(manifest.encode()).digest()).decode()},
    )
    return sjwk_input, sjwk, m_input, m_jws


def tamper_segment(token, index):
    """Flip one bit in the decoded bytes of JWS segment @p index and re-encode it."""
    parts = token.split(".")
    raw = base64.urlsafe_b64decode(parts[index] + "=" * (-len(parts[index]) % 4))
    parts[index] = b64url(flip(raw, len(raw) // 2))
    return ".".join(parts)


def gen_chain(em):
    root = rsa.generate_private_key(public_exponent=65537, key_size=4096)
    rogue_root = rsa.generate_private_key(public_exponent=65537, key_size=4096)
    signing = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    other_signing = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    kid = "ROOT.VECTORS.1"

    payload = file_payload()
    file_hash = base64.b64encode(hashlib.sha256(payload).digest()).decode()
    manifest = manifest_body(file_hash)
    sjwk_input, sjwk, m_input, m_jws = build_chain(root, kid, signing, manifest)

    m_payload = {"sha256": base64.b64encode(hashlib.sha256(manifest.encode()).digest()).decode()}

    # A validly rooted SJWK for another key, while the manifest is signed by the original key.
    _, other_sjwk, _, _ = build_chain(root, kid, other_signing, manifest)
    _, swapped = jws(signing, {"alg": "RS256", "sjwk": other_sjwk}, m_payload)

    # Same chain with the SJWK signed by a root outside the store under the trusted kid.
    _, _, _, rogue_jws = build_chain(rogue_root, kid, signing, manifest)

    # SJWK signature flipped; the manifest JWS over it is validly signed.
    _, sjwk_sig_flipped = jws(signing, {"alg": "RS256", "sjwk": tamper_segment(sjwk, 2)}, m_payload)

    # (name, request, verify calls made, sha256 calls made): the counts pin the failing step.
    tampered = [
        ("manifest_body_changed", request(manifest.replace('"1.0"}', '"1.1"}', 1), m_jws), 2, 1),
        ("manifest_signature_flipped", request(manifest, tamper_segment(m_jws, 2)), 2, 0),
        ("manifest_payload_changed", request(manifest, tamper_segment(m_jws, 1)), 2, 0),
        ("sjwk_signature_flipped", request(manifest, sjwk_sig_flipped), 1, 0),
        ("sjwk_signed_by_rogue_root", request(manifest, rogue_jws), 1, 0),
        ("sjwk_for_other_signing_key", request(manifest, swapped), 2, 0),
    ]
    assert tampered[0][1] != request(manifest, m_jws)

    rn, re_ = key_ne(root)
    sn, se = key_ne(signing)
    em.emit(f'/** @brief `kid` of the chain root key. */\n#define SU_VEC_CHAIN_ROOT_KID "{kid}"\n\n')
    em.emit(c_bytes("k_su_vec_chain_root_n", rn))
    em.emit(c_bytes("k_su_vec_chain_root_e", re_))
    em.emit(c_bytes("k_su_vec_chain_signing_n", sn))
    em.emit(c_bytes("k_su_vec_chain_signing_e", se))
    em.emit(f"\n/** @brief Size of the deterministic file payload; see su_crypto_file_byte(). */\n")
    em.emit(f"#define SU_VEC_FILE_PAYLOAD_LEN {FILE_PAYLOAD_LEN}\n\n")
    em.emit("/** @brief SJWK signing input (`header.payload`) the root key signs. */\n")
    em.emit(f"static const char k_su_vec_chain_sjwk_input[] =\n{c_str(sjwk_input)};\n\n")
    em.emit("/** @brief Manifest JWS signing input (`header.payload`) the signing key signs. */\n")
    em.emit(f"static const char k_su_vec_chain_manifest_input[] =\n{c_str(m_input)};\n\n")
    em.emit("/** @brief Unescaped manifest body; its SHA-256 is bound by the manifest JWS. */\n")
    em.emit(f"static const char k_su_vec_chain_manifest[] =\n{c_str(manifest)};\n\n")
    em.emit("/** @brief Valid update request signed through the chain. */\n")
    em.emit(f"static const char k_su_vec_chain_request[] =\n{c_str(request(manifest, m_jws))};\n\n")
    rows = []
    for i, (name, req, verify_calls, sha256_calls) in enumerate(tampered):
        em.emit(f"static const char k_su_vec_chain_tampered_{i}[] =\n{c_str(req)};\n")
        rows.append(f'  {{ "{name}", k_su_vec_chain_tampered_{i}, {verify_calls}, {sha256_calls} }},')
    em.emit("\n/** @brief Tampered update requests: every row MUST fail with AZ_IOT_ERR_AUTH. */\n")
    em.emit("static const su_chain_vector k_su_chain_tampered[] = {\n" + "\n".join(rows) + "\n};\n")


HEADER = """// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/**
 * @file su_crypto_vectors.h
 * @brief Software updates crypto test vectors. GENERATED by gen_su_crypto_vectors.py; do not edit.
 */
#ifndef SU_CRYPTO_VECTORS_H
#define SU_CRYPTO_VECTORS_H

#include <stddef.h>
#include <stdint.h>

/* clang-format off */

/** @brief One RS256 verification case. */
typedef struct su_rs256_vector
{
  const char* name;
  const uint8_t* modulus;
  size_t modulus_len;
  const uint8_t* exponent;
  size_t exponent_len;
  const uint8_t* message;
  size_t message_len;
  const uint8_t* signature;
  size_t signature_len;
} su_rs256_vector;

/** @brief One SHA-256 case: @p message repeated @p repeat times hashes to @p digest. */
typedef struct su_sha256_vector
{
  const char* name;
  const uint8_t* message;
  size_t message_len;
  size_t repeat;
  const uint8_t* digest;
} su_sha256_vector;

/** @brief One update request for az_iot_su_parse_update_request(). */
typedef struct su_chain_vector
{
  const char* name;
  const char* request;
  size_t verify_calls; /**< verify_rs256 calls made before rejection. */
  size_t sha256_calls; /**< SHA-256 computations started before rejection. */
} su_chain_vector;

/** @brief Byte @p i of the deterministic file payload. */
static inline uint8_t su_crypto_file_byte(size_t i)
{
  return (uint8_t)((i * 7U + 3U) & 0xFFU);
}

"""

FOOTER = """
/* clang-format on */

#endif /* SU_CRYPTO_VECTORS_H */
"""


def main():
    em = Emitter()
    gen_sha256(em)
    gen_rs256(em)
    gen_chain(em)
    sys.stdout.write(HEADER + "\n".join(em.out) + FOOTER)


if __name__ == "__main__":
    main()
