#pragma once

#include "esp_err.h"
#include <stddef.h>

/**
 * Execute web_fetch tool.
 * Input JSON: {"url": "https://example.com/path"}
 */
esp_err_t tool_web_fetch_execute(const char *input_json, char *output, size_t output_size);
