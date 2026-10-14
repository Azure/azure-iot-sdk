// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */

/* The simulated TPM of samples/common against vectors from a TPM 2.0
 * implementation: swtpm and tpm2-tools built the DPS-format key (credential
 * for the EK, bound to the SRK Name; HMAC key duplicated to the SRK with an
 * inner AES wrapper) and computed the expected HMAC. The EK and SRK below are
 * test keys generated for these vectors. */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>

#include <cmocka.h>

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "azure/iot/az_iot_dps_tpm_client.h"
#include "azure/iot/az_iot_sas_token.h"
#include "az_iot_crypto_openssl.h"
#include "sample_tpm_sim.h"

static const char ek_pem[] =
    "-----BEGIN PRIVATE KEY-----\n"
    "MIIEvQIBADANBgkqhkiG9w0BAQEFAASCBKcwggSjAgEAAoIBAQDDk0BDG17IrKr1\n"
    "L0avJ497zqjWnlQOfAvK+zEaZfs1Ef+TEe+G0bj0IsSr/14vlENBHx4Y8BpeIwgu\n"
    "KrDuPGkUdnhvi5SAhz3+lXAPLbLG3pxfSv8Zi1WKYQG2yvejmvk2jkp4oBvohXLO\n"
    "TrkApP2aQPtPjl9UmitrHO3WANTbaw3PCYIgxGFkTrHdICSE+TQyKDAaXlXYQVkY\n"
    "tnHzejlHOfl1qsHqw7EELP/YL7bpZSZr2/72RSjZN/FeXcUgOaKZzEy698jTjFCe\n"
    "l7tqCYuclHmYLRWK81hkPOHdz2XZGbQb38RWM7icLZu+CnErJ6fJGHWLx9YGtkGc\n"
    "iwIjbHjDAgMBAAECggEAOCvnzh5jV9G3/FnKvG3pWQ7fEV1CGTLplDAXR1S5FOIJ\n"
    "fK0vFZLQw1XlFN1wqMu1kVOlqlXzzMvVAqWCUbqKF7V7G9aEHXI6jQ9QsxtV+KZh\n"
    "VUFOCYEqXWdFPcIH5DaTk2bE3uZlfaAufDDCE/xPVmmJhK0Illz9UXNqoAd3G1mf\n"
    "1BHDVB9Q0r85FlmK6PRkrCRFQedKkxBbbkzpJm2XsY6z5Zdj44NkLRBOPqiZjz9i\n"
    "Fr7sQHDL3mxlmo0qp5Si4LLesw9yCO8BeZV0rpx+izABWm08vIKeLsmbsDEIRJsO\n"
    "UpBNpv7yNkGYuURvsxUjDw3rM5MzGJMjrEsZFxDbKQKBgQD3/Rj3Mf4G4Rl4cWMg\n"
    "02E+E+INvEohvAnT4oJAHUB+rnDA9mb9WrmiF2E5U/2jxxJC6MtH78lLW5pl+p4o\n"
    "SlOAD5UkOyYgnIDitqvYk+J/6e4HgVER1WOThaecDTQ9vl55yocQibLWNJsNJQPc\n"
    "1F+C13Hk6K2fU9hTn26UtM92pwKBgQDJ5K++vPYh6Cm0/+7iLPVjMxxLRmuezOZ/\n"
    "A73n8Psf6unQi5+hwrXjx0QYBiLB+5A/wvksNwJlXn5+lCtBwdkZej0KsC4SBVWu\n"
    "jdxS2S7ZpxwN7iHBSdQBneqG7DEDVXsI9Bt77KWI215mWyfHR2plG6KlBHLPn90t\n"
    "ofNnS8wMhQKBgCDlzIgOc7qJ5g0aTaIvKHlJvfVAgz0SauNW6xyCROoJTh/rECKQ\n"
    "BIxCOb2XLNCrOfryUvVKFBQRJNxecNi7c3RLnOdfiWFOIbLxtL5Yb/W0HMWWHCEX\n"
    "g8wwMNn8zm9ab8IMYfqJqdXMyLznqimDauA6oHtTeqTmRI5jWhIUImmFAoGAU7WL\n"
    "HNWyY+qj6FU5kfO8Js5ZS+i91JD2fPFrOxESvnAjVAGgRWy7B99mH2Pj43RPofBQ\n"
    "agHA75DXd9oteU1Lm6GwT48u+u/VT9On533/S+Va33e97SuUSLtWM20ZAV3e7p5w\n"
    "Ctja5by3Dk97VcgJmDR2Il+OzwFkrnf4uPqXdxUCgYEAw9PDft91JX/puVKYbOtC\n"
    "uJUjEr2KWuhyybbaeQqEKC2KM8ZUd5YETyyiXhcreVMgvzWWGqEpOYKYIzouFlj7\n"
    "Sy+oeimVRCzr4sQ3wXua95tXWvL4mQu+7okkzEPvqD/0kUMzEAS4ddrDFAN2E/Qs\n"
    "q+xGpnxI8OEEOAWhBCcnaKE=\n"
    "-----END PRIVATE KEY-----\n";

static const char srk_pem[] =
    "-----BEGIN PRIVATE KEY-----\n"
    "MIIEvwIBADANBgkqhkiG9w0BAQEFAASCBKkwggSlAgEAAoIBAQDAMll91A7yBPo0\n"
    "2h5c/of+i6Z3RP88nDN5aXRdBgnq0AY+AV1X+cN/zV8QSUAdZz36NijwnH95duZx\n"
    "Qhz/kIE9tD9/gYu7pdvcDQ1R9qIB85MTI22sNgUbrLzcX//oSUrRI7fj7vrgT7p2\n"
    "CykzwFqX2NAFQhXrgZj4wI7I+Mo1FOLiCTUMerJcGuxswQEZbj+r8bR9cypym7PD\n"
    "33RdSAqFBsJLjZhaRJn1N0exuYvcpGN0xGNWP071lgubZihMO7Z2/QPXGndBmhAJ\n"
    "+OBDVUz+gOKLmF/oX0ejRbNfu76OkQSW+25gjlhngTc/1jtEsmE4kOsMfN6yu+w/\n"
    "GTdsnBvnAgMBAAECggEAQ0s9x09Q/k9c1dWLLQ9XyXnXCsqnPW8JnNieDBYZsdQt\n"
    "Y1dx+eQzHdPoFZk4MDNeYXXGJtz8yygk7oHyCk9r2RF77CaNKOIdVXJieBDNT7ab\n"
    "h+dvNo1hg3ubUuPwA8Jpxkll0RJzV8fCpyMoRXzzm1goGbc2WpOxXmN5eweVMTko\n"
    "DtHYSZBsQ37t6vfKWq6+iWm3sGeEPmXqmH0GmV33LVEaompmPQdfPOyitcDwJBMX\n"
    "Nj6OlwbZO2/sMsPLl0WBKCC+5plcMFCUaR0jKo4s6m9Xeh6fjODP6l0wUdHsSz3N\n"
    "8rtcB64GA8bKJqcUTikaAOaQAOEvd+t12kjDokZVLQKBgQDhoHPGNzcwS9zzCI3p\n"
    "UOsuv/HoddFaVWiSEkhAVDJA8r4tYkiLFJF3sn3ICnmzj69LslGKdlJWob8asTBn\n"
    "uMhE1+gktCMc1D3c+1MjTPzDI6cqrJyeV+ftgtRI4+3C0ysXJtSOOw1ssbvtqHrI\n"
    "k8hVuWSavtVnWjqhONLg+hxEXQKBgQDaEdR0RGmUxgtdPJNKc0VAb2bgEiZ3Xx2O\n"
    "I/Kj2JIizuYLH8HUJwHd9qlqS6/yCcsaQT5Xa7AexljWxI6nTOnkWLxsaWB4l5QY\n"
    "k0vahNv6T11owYYcMXXA4ATyKXIK0yS3X7QzpWIQyl+Dny+6AtviuFworpy5axHw\n"
    "KgeYV46dEwKBgQCauFnL6h85SjyImCwge7HazBtKCkj2xizARjMvkc0oRo1tN8GB\n"
    "Rih+yXKBk9Vi8bwGj5Bk/8B00dPnrOwBpHAI7Xmueay+XzjQ6bNYOLn34uIGXHnG\n"
    "B4fuklGablqjyAT7ilAl/I63I6Y4Udlo5BZz2n8HsqdKiVA9MGdDiZ+KrQKBgQC2\n"
    "FxVUUJvpAsYSgLZtAHvfJia9C+ra2q2PNy0w63J7ZlBpEYeEY14HviQRAkpUzSCv\n"
    "N7Oaw+04hV9BVrfXsz3K0t3jmMpZaBjdvIEmYrJc/VB+XLpjaFlvSKBYpYr4xv5t\n"
    "sJiEV6DfbFz9RztPE3wQgOlTfxqXKXS9Ghtldhc7yQKBgQDP3G9Sn1K8bcFk++bv\n"
    "EG7wK+aNpHP7LeI5jNoHdxeYzw6gkjRPxKerDAF2795pMOvXOJjKK7oBTyxDAYcj\n"
    "hcajiKAR4WoAwUJhq4EfOEQCWb2b7MsXRtVfPbyB0vMiH/zTvDdzrAbR2TWon05b\n"
    "igpspD/rQ/+U60tjnZV5GyAmZw==\n"
    "-----END PRIVATE KEY-----\n";

static const uint8_t dps_key[] = {
  0x00, 0x34, 0x00, 0x20, 0x7E, 0x9F, 0x5C, 0xCC, 0xF2, 0x4B, 0xAB, 0x51,
  0x73, 0x24, 0x1E, 0xAC, 0x50, 0x0C, 0x51, 0x55, 0x37, 0x8F, 0xF3, 0x25,
  0x8F, 0x6E, 0x4D, 0xEC, 0xA7, 0x61, 0xDB, 0xA4, 0xB4, 0x81, 0xAE, 0x19,
  0x65, 0x3E, 0x1B, 0x2E, 0xF7, 0x1E, 0x12, 0x06, 0xC0, 0x3A, 0x42, 0x62,
  0x0C, 0x9E, 0xE4, 0x8C, 0x17, 0xDE, 0x01, 0x00, 0xA5, 0xF5, 0x8F, 0xAA,
  0x6E, 0xEC, 0x2B, 0x72, 0x18, 0xCB, 0x89, 0x25, 0x9E, 0xB1, 0x4C, 0x19,
  0x7E, 0xE3, 0x57, 0x86, 0x35, 0xF6, 0x3B, 0x3F, 0x04, 0x09, 0x68, 0x65,
  0xFD, 0xDD, 0x30, 0xB1, 0x77, 0xAC, 0x8E, 0xDE, 0x84, 0xF2, 0xBF, 0x20,
  0x1F, 0x4F, 0x02, 0x51, 0xD6, 0x3D, 0x01, 0xDA, 0x35, 0xBF, 0xFC, 0xA4,
  0xB3, 0x1E, 0x82, 0x52, 0x05, 0x36, 0x13, 0xBB, 0xF4, 0x8C, 0xAF, 0x13,
  0xAD, 0xDE, 0xCB, 0x33, 0xDE, 0x78, 0x30, 0x59, 0x79, 0x44, 0xF0, 0x7F,
  0x8B, 0x6B, 0xDC, 0x07, 0xFE, 0x9E, 0xA6, 0x9D, 0xD6, 0xF9, 0x63, 0xCD,
  0x34, 0xCA, 0x5D, 0x15, 0x33, 0xB9, 0x1A, 0x5A, 0xA2, 0x2F, 0x97, 0xDF,
  0x81, 0x80, 0x06, 0xE1, 0xD6, 0xC9, 0xA4, 0x08, 0x73, 0x37, 0xC3, 0xA6,
  0x8B, 0x74, 0xCD, 0x68, 0x6E, 0xD8, 0xCF, 0xDC, 0x80, 0xB6, 0x44, 0x49,
  0x5C, 0x5A, 0x6A, 0xC0, 0xA9, 0xFB, 0x54, 0xA1, 0x15, 0x0A, 0x8C, 0x65,
  0xC4, 0x3B, 0x57, 0x6C, 0x2C, 0xB1, 0x67, 0x8A, 0x96, 0x02, 0x0F, 0xAC,
  0x26, 0x06, 0x0A, 0x7E, 0xCB, 0x6B, 0x0C, 0x80, 0x95, 0x64, 0x4A, 0x7E,
  0xAB, 0xF2, 0x76, 0xAB, 0x45, 0xD8, 0x67, 0x18, 0x02, 0x97, 0x5C, 0xFB,
  0x0F, 0xCD, 0xEA, 0xCB, 0xC9, 0x77, 0x85, 0x6A, 0x66, 0xF5, 0x34, 0x70,
  0x11, 0x4A, 0x48, 0xC9, 0xD4, 0x0E, 0x25, 0x49, 0xDC, 0xBE, 0x0D, 0x05,
  0x76, 0x5C, 0xD1, 0xBF, 0xAC, 0x30, 0x7F, 0x99, 0xC6, 0x66, 0x25, 0xFA,
  0xA2, 0x98, 0x5F, 0xE5, 0x3E, 0xAC, 0xE6, 0xDB, 0xF7, 0x39, 0x7A, 0xA9,
  0xFD, 0xDC, 0xF0, 0x6B, 0x4A, 0x6E, 0xD7, 0x8F, 0x66, 0x23, 0xAB, 0x47,
  0xF4, 0x9F, 0xCF, 0x94, 0xE3, 0xFE, 0x2C, 0xCE, 0xDA, 0x27, 0x5D, 0xA6,
  0x82, 0xE8, 0x01, 0x07, 0xB9, 0x51, 0x1C, 0xEA, 0xF7, 0x3A, 0x11, 0xF2,
  0x00, 0xAE, 0x00, 0x20, 0x1A, 0x67, 0xA1, 0x99, 0x59, 0x94, 0xFE, 0x13,
  0xB1, 0x5B, 0xD7, 0x23, 0x60, 0xE6, 0xF0, 0xAF, 0x37, 0xA8, 0xD0, 0x40,
  0x3E, 0x59, 0x71, 0xDC, 0x77, 0x8C, 0x7D, 0x89, 0xE5, 0x45, 0x63, 0xBB,
  0x44, 0xF2, 0x40, 0x7B, 0x71, 0x4D, 0xCA, 0xFE, 0x90, 0x7C, 0x8C, 0x60,
  0x4B, 0xF3, 0x81, 0x5F, 0xA2, 0x93, 0x03, 0x05, 0xF0, 0xE9, 0xAD, 0xEB,
  0xD8, 0x61, 0x02, 0x8C, 0x97, 0x80, 0x5B, 0x5E, 0xD1, 0xFD, 0x36, 0x32,
  0xE8, 0x01, 0xE4, 0xA5, 0xDD, 0xB0, 0xA2, 0x30, 0x11, 0x3A, 0xAE, 0x0C,
  0xC3, 0x60, 0xDF, 0xE0, 0x77, 0x9B, 0x68, 0xFA, 0x31, 0x20, 0x95, 0xC2,
  0x0F, 0x85, 0xE1, 0x3E, 0xF1, 0x91, 0x6B, 0x70, 0x93, 0x72, 0x4C, 0x78,
  0xBF, 0x41, 0x20, 0x6C, 0x08, 0xF5, 0x82, 0x7E, 0x5E, 0xBC, 0x81, 0x18,
  0xCF, 0x28, 0x4D, 0x3F, 0xAA, 0x70, 0xE5, 0xF4, 0x35, 0x6C, 0x6E, 0x23,
  0xEE, 0xEA, 0x4D, 0x07, 0xD0, 0x66, 0x67, 0x33, 0x15, 0x75, 0xAB, 0xFD,
  0x26, 0x82, 0xE5, 0x24, 0x4D, 0xDF, 0x32, 0xBE, 0xD8, 0x3E, 0x94, 0xF6,
  0xF6, 0x01, 0x19, 0xD3, 0x5B, 0x86, 0x6B, 0xAA, 0x08, 0x2B, 0x00, 0x73,
  0x9B, 0x8C, 0x70, 0xCA, 0x99, 0xA1, 0x59, 0x03, 0x01, 0x00, 0x26, 0x12,
  0x25, 0x2A, 0x96, 0x2E, 0xB9, 0x82, 0x2B, 0xB5, 0x4A, 0xEE, 0x27, 0x46,
  0x22, 0xE2, 0xB4, 0x32, 0x4F, 0xA0, 0x2E, 0xDD, 0xA8, 0x9F, 0x69, 0x1A,
  0x9A, 0xD7, 0xA1, 0x40, 0x4E, 0xDA, 0x37, 0x94, 0x91, 0x4A, 0xFD, 0x42,
  0xF9, 0x80, 0x00, 0xF8, 0xAD, 0xE4, 0x5D, 0x38, 0x22, 0xE0, 0xA6, 0xC4,
  0x4C, 0x20, 0x73, 0xB3, 0x3F, 0xD3, 0xAC, 0xEF, 0x6A, 0x95, 0xF2, 0xBC,
  0x3B, 0xCD, 0x4B, 0xC5, 0x21, 0xAE, 0x21, 0xE9, 0xFC, 0x6E, 0xC9, 0xD9,
  0x79, 0x3C, 0xB2, 0x11, 0x4D, 0x2F, 0xA8, 0x15, 0xD7, 0x7E, 0x61, 0x85,
  0x8E, 0xAD, 0xAF, 0x25, 0x93, 0x95, 0x45, 0x2D, 0x7C, 0x8F, 0x47, 0xE5,
  0xF8, 0xB0, 0xB3, 0xB4, 0xA5, 0xF4, 0xC2, 0xC6, 0x5E, 0x8C, 0xD8, 0x7F,
  0xBB, 0x69, 0xDD, 0x17, 0x9A, 0x92, 0x16, 0x6F, 0x1D, 0x2E, 0x9E, 0xD6,
  0x94, 0x32, 0xDA, 0xC1, 0x01, 0x43, 0x95, 0xD7, 0xD9, 0xEB, 0xFE, 0x02,
  0x3F, 0xC4, 0x20, 0x41, 0x21, 0xBC, 0xC8, 0x3F, 0xA4, 0xEF, 0x45, 0x5E,
  0x24, 0x85, 0x45, 0xC1, 0x60, 0xC8, 0x64, 0x6D, 0x5B, 0x3C, 0xAD, 0xB7,
  0x2A, 0xD9, 0xE6, 0x6C, 0x3D, 0x90, 0xA9, 0x75, 0x86, 0xE9, 0x48, 0xA1,
  0x83, 0xF0, 0xF0, 0x36, 0x59, 0x6C, 0xDB, 0x9F, 0x5E, 0x38, 0x54, 0x71,
  0x28, 0x1C, 0xEA, 0xCA, 0x9A, 0x0B, 0x1D, 0xD6, 0x8D, 0x35, 0xBD, 0x17,
  0xCD, 0x7E, 0xDC, 0x79, 0x5D, 0x63, 0xE4, 0xD8, 0x74, 0x9F, 0x60, 0x33,
  0x55, 0xAF, 0x38, 0x2D, 0xE1, 0x63, 0x7A, 0x12, 0xF9, 0x48, 0x59, 0xD3,
  0x9F, 0x6C, 0xFB, 0x1B, 0x44, 0xF6, 0xD8, 0x65, 0x94, 0x70, 0xF2, 0xE5,
  0xF4, 0x6C, 0xFB, 0x68, 0x72, 0x73, 0xFB, 0x85, 0x5D, 0x15, 0x2B, 0x08,
  0x83, 0xD9, 0x9A, 0x1C, 0xAB, 0xBA, 0x09, 0x4B, 0x0A, 0x27, 0xBD, 0x86,
  0x05, 0xB9, 0x00, 0x50, 0x00, 0x08, 0x00, 0x0B, 0x00, 0x04, 0x00, 0x60,
  0x00, 0x20, 0xBE, 0xF5, 0x6B, 0x8C, 0x1C, 0xC8, 0x4E, 0x11, 0xED, 0xD7,
  0x17, 0x52, 0x8D, 0x2C, 0xD9, 0x93, 0x56, 0xBD, 0x2B, 0xBF, 0x8F, 0x01,
  0x52, 0x09, 0xC3, 0xF8, 0x4A, 0xEE, 0xAB, 0xA8, 0xE8, 0xA2, 0x00, 0x05,
  0x00, 0x0B, 0x00, 0x20, 0xB4, 0x1D, 0xD3, 0x2D, 0xD7, 0xB9, 0x51, 0x6F,
  0xA9, 0xC4, 0x58, 0x7E, 0x92, 0x26, 0xA7, 0xD0, 0x01, 0x2A, 0xA1, 0xFE,
  0x9F, 0xA9, 0x20, 0x03, 0xC5, 0x75, 0x9C, 0x69, 0x50, 0x76, 0xA0, 0x73,
};

#define DPS_KEY_B64 "ADQAIH6fXMzyS6tRcyQerFAMUVU3j/Mlj25N7Kdh26S0ga4ZZT4bLvceEgbAOkJiDJ7kjBfeAQCl9Y+qbuwrchjLiSWesUwZfuNXhjX2Oz8ECWhl/d0wsXesjt6E8r8gH08CUdY9Ado1v/yksx6CUgU2E7v0jK8Trd7LM954MFl5RPB/i2vcB/6epp3W+WPNNMpdFTO5GlqiL5ffgYAG4dbJpAhzN8Omi3TNaG7Yz9yAtkRJXFpqwKn7VKEVCoxlxDtXbCyxZ4qWAg+sJgYKfstrDICVZEp+q/J2q0XYZxgCl1z7D83qy8l3hWpm9TRwEUpIydQOJUncvg0FdlzRv6wwf5nGZiX6ophf5T6s5tv3OXqp/dzwa0pu149mI6tH9J/PlOP+LM7aJ12mgugBB7lRHOr3OhHyAK4AIBpnoZlZlP4TsVvXI2Dm8K83qNBAPllx3HeMfYnlRWO7RPJAe3FNyv6QfIxgS/OBX6KTAwXw6a3r2GECjJeAW17R/TYy6AHkpd2wojAROq4Mw2Df4HebaPoxIJXCD4XhPvGRa3CTckx4v0EgbAj1gn5evIEYzyhNP6pw5fQ1bG4j7upNB9BmZzMVdav9JoLlJE3fMr7YPpT29gEZ01uGa6oIKwBzm4xwypmhWQMBACYSJSqWLrmCK7VK7idGIuK0Mk+gLt2on2kamtehQE7aN5SRSv1C+YAA+K3kXTgi4KbETCBzsz/TrO9qlfK8O81LxSGuIen8bsnZeTyyEU0vqBXXfmGFjq2vJZOVRS18j0fl+LCztKX0wsZejNh/u2ndF5qSFm8dLp7WlDLawQFDldfZ6/4CP8QgQSG8yD+k70VeJIVFwWDIZG1bPK23KtnmbD2QqXWG6Uihg/DwNlls259eOFRxKBzqypoLHdaNNb0XzX7ceV1j5Nh0n2AzVa84LeFjehL5SFnTn2z7G0T22GWUcPLl9Gz7aHJz+4VdFSsIg9maHKu6CUsKJ72GBbkAUAAIAAsABABgACC+9WuMHMhOEe3XF1KNLNmTVr0rv48BUgnD+Eruq6joogAFAAsAILQd0y3XuVFvqcRYfpImp9ABKqH+n6kgA8V1nGlQdqBz"

#define MESSAGE "hello from swtpm vs sim"

#define SWTPM_HMAC "fd40ca987297b69c10f24d6585317f5af0f94de0fcce3d8fe6e71dc29c12087f"

static void to_hex(const uint8_t* in, size_t len, char* out)
{
  static const char digits[] = "0123456789abcdef";
  for (size_t i = 0; i < len; ++i)
  {
    out[2 * i] = digits[in[i] >> 4];
    out[2 * i + 1] = digits[in[i] & 0x0F];
  }
  out[2 * len] = '\0';
}

static sample_tpm_sim* vector_sim(void)
{
  sample_tpm_sim* sim = sample_tpm_sim_create_from_pem(ek_pem, srk_pem);
  assert_non_null(sim);
  return sim;
}

static void imports_the_swtpm_key_and_matches_its_hmac(void** state)
{
  (void)state;
  sample_tpm_sim* sim = vector_sim();
  uint8_t mac[AZ_IOT_SHA256_SIZE];
  assert_false(sample_tpm_sim_hmac(sim, (const uint8_t*)MESSAGE, strlen(MESSAGE), mac));

  az_iot_dps_tpm_ops ops = sample_tpm_sim_ops(sim);
  assert_int_equal(ops.import_key(ops.ctx, dps_key, sizeof(dps_key)), AZ_IOT_OK);
  assert_int_equal(
      ops.hmac_sha256(ops.ctx, (const uint8_t*)MESSAGE, strlen(MESSAGE), mac), AZ_IOT_OK);
  char hex[2 * AZ_IOT_SHA256_SIZE + 1];
  to_hex(mac, sizeof(mac), hex);
  assert_string_equal(hex, SWTPM_HMAC);
  sample_tpm_sim_destroy(sim);
}

static void a_tampered_key_is_refused(void** state)
{
  (void)state;
  /* Inside the credential, the duplicate, and the object public area. */
  static const size_t offsets[] = { 20, 400, sizeof(dps_key) - 10 };
  for (size_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]); ++i)
  {
    sample_tpm_sim* sim = vector_sim();
    uint8_t key[sizeof(dps_key)];
    memcpy(key, dps_key, sizeof(key));
    key[offsets[i]] ^= 0x01;
    az_iot_dps_tpm_ops ops = sample_tpm_sim_ops(sim);
    assert_int_not_equal(ops.import_key(ops.ctx, key, sizeof(key)), AZ_IOT_OK);
    uint8_t mac[AZ_IOT_SHA256_SIZE];
    assert_false(sample_tpm_sim_hmac(sim, (const uint8_t*)"x", 1, mac));
    sample_tpm_sim_destroy(sim);
  }
}

static void another_tpm_cannot_import_the_key(void** state)
{
  (void)state;
  sample_tpm_sim* sim = sample_tpm_sim_create(NULL);
  assert_non_null(sim);
  az_iot_dps_tpm_ops ops = sample_tpm_sim_ops(sim);
  assert_int_equal(ops.import_key(ops.ctx, dps_key, sizeof(dps_key)), AZ_IOT_ERR_AUTH);
  assert_int_equal(ops.import_key(ops.ctx, dps_key, 10), AZ_IOT_ERR_PROTOCOL);
  sample_tpm_sim_destroy(sim);
}

static void public_areas_are_rsa2048_ek_and_srk(void** state)
{
  (void)state;
  sample_tpm_sim* sim = vector_sim();
  const uint8_t* pub = NULL;
  size_t len = 0;
  sample_tpm_sim_ek_public(sim, &pub, &len);
  assert_int_equal(len, 316); /* TPM2B_PUBLIC: RSA 2048, 32-byte policy */
  assert_int_equal((pub[0] << 8) | pub[1], len - 2);
  assert_int_equal((pub[2] << 8) | pub[3], 0x0001); /* RSA */
  assert_int_equal((pub[4] << 8) | pub[5], 0x000B); /* SHA-256 */
  sample_tpm_sim_srk_public(sim, &pub, &len);
  assert_int_equal(len, 284);
  sample_tpm_sim_destroy(sim);
}

/* The SDK client against the simulated TPM: DPS sends the swtpm-built key in
 * its 401, and the next request carries a token the TPM signed with it. */
static void the_client_signs_with_the_imported_key(void** state)
{
  (void)state;
  sample_tpm_sim* sim = vector_sim();
  const uint8_t* ek = NULL;
  const uint8_t* srk = NULL;
  size_t ek_len = 0;
  size_t srk_len = 0;
  sample_tpm_sim_ek_public(sim, &ek, &ek_len);
  sample_tpm_sim_srk_public(sim, &srk, &srk_len);

  char reg_id[AZ_IOT_DPS_TPM_REGISTRATION_ID_SIZE];
  assert_int_equal(
      az_iot_dps_tpm_registration_id_from_ek(az_iot_crypto_openssl(), ek, ek_len, reg_id),
      AZ_IOT_OK);

  az_iot_dps_tpm_client_options o = az_iot_dps_tpm_client_options_default();
  o.id_scope = "0ne00000001";
  o.registration_id = reg_id;
  o.endorsement_key = ek;
  o.endorsement_key_len = ek_len;
  o.storage_root_key = srk;
  o.storage_root_key_len = srk_len;
  o.tpm = sample_tpm_sim_ops(sim);
  az_iot_dps_tpm_client c;
  assert_int_equal(az_iot_dps_tpm_client_init(&c, &o), AZ_IOT_OK);

  static char buf[AZ_IOT_DPS_TPM_REQUEST_BUFFER_SIZE];
  az_iot_dps_tpm_request req;
  az_iot_dps_tpm_next next;
  assert_int_equal(az_iot_dps_tpm_client_get_request(&c, 1700000000u, buf, sizeof(buf), &req), AZ_IOT_OK);
  static char body[sizeof(DPS_KEY_B64) + 64];
  int n = snprintf(
      body, sizeof(body), "{\"authenticationKey\":\"%s\",\"keyName\":\"registration\"}", DPS_KEY_B64);
  assert_true(n > 0 && (size_t)n < sizeof(body));
  assert_int_equal(
      az_iot_dps_tpm_client_handle_response(
          &c, 401, az_span_create((uint8_t*)body, n), 0, &next),
      AZ_IOT_OK);
  assert_int_equal(az_iot_dps_tpm_client_get_request(&c, 1700000000u, buf, sizeof(buf), &req), AZ_IOT_OK);
  assert_non_null(req.authorization);

  /* The same token from the swtpm HMAC of the string to sign. */
  char uri[256];
  n = snprintf(uri, sizeof(uri), "0ne00000001%%2fregistrations%%2f%s", reg_id);
  assert_true(n > 0 && (size_t)n < sizeof(uri));
  char to_sign[300];
  size_t to_sign_len = 0;
  uint64_t expiry = 1700000000u + AZ_IOT_DPS_TPM_DEFAULT_SAS_TOKEN_LIFETIME_SECONDS;
  assert_int_equal(
      az_iot_sas_token_string_to_sign(uri, expiry, to_sign, sizeof(to_sign), &to_sign_len),
      AZ_IOT_OK);
  uint8_t mac[AZ_IOT_SHA256_SIZE];
  assert_true(sample_tpm_sim_hmac(sim, (const uint8_t*)to_sign, to_sign_len, mac));
  char token[512];
  size_t token_len = 0;
  assert_int_equal(
      az_iot_sas_token_from_signature(
          uri, "registration", expiry, mac, token, sizeof(token), &token_len),
      AZ_IOT_OK);
  assert_string_equal(req.authorization, token);
  sample_tpm_sim_destroy(sim);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(imports_the_swtpm_key_and_matches_its_hmac),
    cmocka_unit_test(a_tampered_key_is_refused),
    cmocka_unit_test(another_tpm_cannot_import_the_key),
    cmocka_unit_test(public_areas_are_rsa2048_ek_and_srk),
    cmocka_unit_test(the_client_signs_with_the_imported_key),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
