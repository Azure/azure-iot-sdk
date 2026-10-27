// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* Configure-time check (cmake/az_iot_su_limits.cmake): the installed azure-sdk-for-c parses a
 * manifest at the configured limits into the layout this build compiles. Exits 0 when it does. */

#include <stdio.h>
#include <string.h>

#include <azure/core/az_json.h>
#include <azure/core/az_span.h>
#include <azure/iot/az_iot_adu_client.h>

/* A library built with larger limits writes past the manifest; keep that inside this object. */
static union
{
  az_iot_adu_client_update_manifest manifest;
  unsigned char room[1 << 16];
} g_parsed;

static char g_text[1 << 15];

static size_t append(size_t used, const char* text)
{
  size_t length = strlen(text);
  if (used + length >= sizeof(g_text))
  {
    return sizeof(g_text);
  }
  memcpy(g_text + used, text, length);
  return used + length;
}

int main(void)
{
  char item[96];
  size_t used = append(0, "{\"instructions\":{\"steps\":[");
  for (int step = 0; step < _az_IOT_ADU_CLIENT_MAX_INSTRUCTIONS_STEPS; ++step)
  {
    used = append(
        used, step == 0 ? "{\"handler\":\"h\",\"files\":[" : ",{\"handler\":\"h\",\"files\":[");
    int files = step == 0 ? _az_IOT_ADU_CLIENT_MAX_FILE_COUNT_PER_STEP : 1;
    for (int file = 0; file < files; ++file)
    {
      (void)snprintf(item, sizeof(item), "%s\"f%d\"", file == 0 ? "" : ",", file);
      used = append(used, item);
    }
    used = append(used, "]}");
  }
  used = append(used, "]},\"files\":{");
  for (int file = 0; file < _az_IOT_ADU_CLIENT_MAX_TOTAL_FILE_COUNT; ++file)
  {
    (void)snprintf(
        item,
        sizeof(item),
        "%s\"f%d\":{\"fileName\":\"n\",\"sizeInBytes\":1,\"hashes\":{\"sha256\":\"h\"}}",
        file == 0 ? "" : ",",
        file);
    used = append(used, item);
  }
  used = append(used, "},\"createdDateTime\":\"end\"}");
  if (used >= sizeof(g_text))
  {
    return 3;
  }

  az_iot_adu_client client;
  az_json_reader reader;
  if (az_result_failed(az_iot_adu_client_init(&client, NULL))
      || az_result_failed(
          az_json_reader_init(&reader, az_span_create((uint8_t*)g_text, (int32_t)used), NULL)))
  {
    return 3;
  }
  if (az_result_failed(
          az_iot_adu_client_parse_update_manifest(&client, &reader, &g_parsed.manifest)))
  {
    return 1;
  }
  const az_iot_adu_client_update_manifest* manifest = &g_parsed.manifest;
  if (manifest->instructions.steps_count != _az_IOT_ADU_CLIENT_MAX_INSTRUCTIONS_STEPS
      || manifest->instructions.steps[0].files_count != _az_IOT_ADU_CLIENT_MAX_FILE_COUNT_PER_STEP
      || manifest->files_count != _az_IOT_ADU_CLIENT_MAX_TOTAL_FILE_COUNT
      || !az_span_is_content_equal(manifest->create_date_time, AZ_SPAN_FROM_STR("end")))
  {
    return 2;
  }
  return 0;
}
