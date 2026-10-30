// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
#include "e2e_notify_time.h"

#include <string.h>

/**
 * @brief Parses @p count decimal digits at @p p.
 *
 * Stops at the first non-digit, so it never reads past a NUL terminator.
 *
 * @return true with the value in @p out; false if any of them is not a digit.
 */
static bool parse_digits(const char* p, int count, int* out)
{
  int value = 0;
  for (int i = 0; i < count; i++)
  {
    if (p[i] < '0' || p[i] > '9')
    {
      return false;
    }
    value = value * 10 + (p[i] - '0');
  }
  *out = value;
  return true;
}

/**
 * @brief Converts a proleptic Gregorian UTC date to days since 1970-01-01.
 */
static int64_t days_from_civil(int year, int month, int day)
{
  year -= (month <= 2) ? 1 : 0;
  int64_t const era = (year >= 0 ? year : year - 399) / 400;
  int64_t const yoe = year - era * 400;
  int64_t const doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  int64_t const doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

/**
 * @brief Number of days in @p month (1-12) of @p year.
 */
static int days_in_month(int year, int month)
{
  static const int days[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
  bool const leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
  return (month == 2 && leap) ? 29 : days[month - 1];
}

/**
 * @brief Skips JSON whitespace (space, tab, LF, CR).
 *
 * @return The first non-whitespace character at or after @p p.
 */
static const char* skip_json_whitespace(const char* p)
{
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
  {
    p++;
  }
  return p;
}

bool e2e_notify_enqueued_time(const char* body, int64_t* out_epoch_s)
{
  static const char key[] = "\"enqueuedTimeUtc\"";
  const char* p = strstr(body, key);
  if (p == NULL)
  {
    return false;
  }
  p += sizeof(key) - 1;
  p = skip_json_whitespace(p);
  if (*p != ':')
  {
    return false;
  }
  p = skip_json_whitespace(p + 1);
  if (*p != '"')
  {
    return false;
  }
  p++;

  /* Each check runs only if the previous ones matched non-NUL characters, so no
   * read goes past the terminator of a truncated body. */
  int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
  if (!parse_digits(p, 4, &year) || p[4] != '-' || !parse_digits(p + 5, 2, &month) || p[7] != '-'
      || !parse_digits(p + 8, 2, &day) || p[10] != 'T' || !parse_digits(p + 11, 2, &hour)
      || p[13] != ':' || !parse_digits(p + 14, 2, &minute) || p[16] != ':'
      || !parse_digits(p + 17, 2, &second) || month < 1 || month > 12 || day < 1
      || day > days_in_month(year, month) || hour > 23 || minute > 59 || second > 59)
  {
    return false;
  }

  p += 19;
  if (*p == '.')
  {
    p++;
    if (*p < '0' || *p > '9')
    {
      return false;
    }
    while (*p >= '0' && *p <= '9')
    {
      p++;
    }
  }
  if (*p == 'Z')
  {
    p++;
  }
  else if (strncmp(p, "+00:00", 6) == 0)
  {
    p += 6;
  }
  else
  {
    return false;
  }
  if (*p != '"')
  {
    return false;
  }

  *out_epoch_s = days_from_civil(year, month, day) * 86400 + hour * 3600 + minute * 60 + second;
  return true;
}
