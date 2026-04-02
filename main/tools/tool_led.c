#include "tools/tool_led.h"
#include "mimi_config.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "cJSON.h"

#include <string.h>
#include <stdio.h>

static const char *TAG = "tool_led";

/* LED state: 1 = open (on), 0 = close (off) */
static int s_led_state = 0;

esp_err_t tool_led_init(void)
{
    /* Configure LED GPIO as output */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << MIMI_LED_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure LED GPIO %d", MIMI_LED_GPIO);
        return ret;
    }

    /* Initialize to close (off) - LOW for active-high LED */
    gpio_set_level(MIMI_LED_GPIO, 0);
    s_led_state = 0;
    ESP_LOGI(TAG, "LED GPIO %d set to LOW (LED off)", MIMI_LED_GPIO);
    ESP_LOGI(TAG, "LED tool initialized on GPIO %d (active-high)", MIMI_LED_GPIO);
    return ESP_OK;
}

esp_err_t tool_led_write_execute(const char *input_json, char *output, size_t output_size)
{
    ESP_LOGI(TAG, "json input %s", input_json);
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *state_obj = cJSON_GetObjectItem(root, "state");
    if (!cJSON_IsString(state_obj)) {
        snprintf(output, output_size, "Error: 'state' required (string: \"open\" or \"close\")");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    const char *state_str = state_obj->valuestring;
    int new_state;
    int gpio_level;

    if (strcmp(state_str, "open") == 0) {
        /* LED on: active-high, set GPIO HIGH */
        new_state = 1;
        gpio_level = 1;
    } else if (strcmp(state_str, "close") == 0) {
        /* LED off: active-high, set GPIO LOW */
        new_state = 0;
        gpio_level = 0;
    } else {
        snprintf(output, output_size, "Error: 'state' must be \"open\" or \"close\", got \"%s\"", state_str);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = gpio_set_level(MIMI_LED_GPIO, gpio_level);
    if (ret != ESP_OK) {
        snprintf(output, output_size, "Error: failed to set LED GPIO %d", MIMI_LED_GPIO);
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    s_led_state = new_state;
    snprintf(output, output_size, "LED set to %s", state_str);
    ESP_LOGI(TAG, "led_write: %s (GPIO %d = %s)", state_str, MIMI_LED_GPIO, gpio_level ? "HIGH" : "LOW");

    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t tool_led_read_execute(const char *input_json, char *output, size_t output_size)
{
    (void)input_json;

    const char *state_str = s_led_state ? "open" : "close";
    snprintf(output, output_size, "LED is %s", state_str);
    ESP_LOGI(TAG, "led_read: %s", state_str);

    return ESP_OK;
}