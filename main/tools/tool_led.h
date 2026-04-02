#pragma once

#include "esp_err.h"
#include <stddef.h>

/**
 * Initialize LED tool — configure LED GPIO pin.
 */
esp_err_t tool_led_init(void);

/**
 * Set LED state to open (on) or close (off).
 * Input JSON: {"state": "open" | "close"}
 */
esp_err_t tool_led_write_execute(const char *input_json, char *output, size_t output_size);

/**
 * Read current LED state.
 * Input JSON: {} (no parameters)
 * Returns: "open" or "close"
 */
esp_err_t tool_led_read_execute(const char *input_json, char *output, size_t output_size);