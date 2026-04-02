#include "memory_store.h"
#include "mimi_config.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "memory";

static void get_date_str(char *buf, size_t size, int days_ago)
{
    time_t now;
    time(&now);
    now -= days_ago * 86400;
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(buf, size, "%Y-%m-%d", &tm);
}

esp_err_t memory_store_init(void)
{
    /* SPIFFS is flat — no real directory creation needed.
       Just verify we can open the base path. */
    ESP_LOGI(TAG, "Memory store initialized at %s", MIMI_SPIFFS_BASE);
    return ESP_OK;
}

esp_err_t memory_read_long_term(char *buf, size_t size)
{
    int64_t start = esp_timer_get_time();
    ESP_LOGI(TAG, "read %s...", MIMI_MEMORY_FILE);
    
    int64_t t0 = esp_timer_get_time();
    FILE *f = fopen(MIMI_MEMORY_FILE, "r");
    int64_t t1 = esp_timer_get_time();
    ESP_LOGI(TAG, "  fopen took %lld ms", (t1 - t0) / 1000);
    
    if (!f) {
        buf[0] = '\0';
        return ESP_ERR_NOT_FOUND;
    }

    t0 = esp_timer_get_time();
    size_t n = fread(buf, 1, size - 1, f);
    t1 = esp_timer_get_time();
    ESP_LOGI(TAG, "  fread %d bytes took %lld ms", (int)n, (t1 - t0) / 1000);
    
    buf[n] = '\0';
    
    t0 = esp_timer_get_time();
    fclose(f);
    t1 = esp_timer_get_time();
    ESP_LOGI(TAG, "  fclose took %lld ms", (t1 - t0) / 1000);
    
    int64_t end = esp_timer_get_time();
    ESP_LOGI(TAG, "memory_read_long_term total: %lld ms", (end - start) / 1000);
    return ESP_OK;
}

esp_err_t memory_write_long_term(const char *content)
{
    FILE *f = fopen(MIMI_MEMORY_FILE, "w");
    if (!f) {
        ESP_LOGE(TAG, "Cannot write %s", MIMI_MEMORY_FILE);
        return ESP_FAIL;
    }
    fputs(content, f);
    fclose(f);
    ESP_LOGI(TAG, "Long-term memory updated (%d bytes)", (int)strlen(content));
    return ESP_OK;
}

esp_err_t memory_append_today(const char *note)
{
    char date_str[16];
    get_date_str(date_str, sizeof(date_str), 0);

    char path[64];
    snprintf(path, sizeof(path), "%s/%s.md", MIMI_SPIFFS_MEMORY_DIR, date_str);

    FILE *f = fopen(path, "a");
    if (!f) {
        /* Try creating — if file doesn't exist yet, write header */
        f = fopen(path, "w");
        if (!f) {
            ESP_LOGE(TAG, "Cannot open %s", path);
            return ESP_FAIL;
        }
        fprintf(f, "# %s\n\n", date_str);
    }

    fprintf(f, "%s\n", note);
    fclose(f);
    return ESP_OK;
}

esp_err_t memory_read_recent(char *buf, size_t size, int days)
{
    int64_t start = esp_timer_get_time();
    size_t offset = 0;
    buf[0] = '\0';

    ESP_LOGI(TAG, "read recent memory (last %d days)...", days);
    for (int i = 0; i < days && offset < size - 1; i++) {
        int64_t iter_start = esp_timer_get_time();
        char date_str[16];
        get_date_str(date_str, sizeof(date_str), i);

        char path[64];
        snprintf(path, sizeof(path), "%s/%s.md", MIMI_SPIFFS_MEMORY_DIR, date_str);

        int64_t t0 = esp_timer_get_time();
        FILE *f = fopen(path, "r");
        int64_t t1 = esp_timer_get_time();
        
        if (!f) {
            ESP_LOGI(TAG, "  [%d] %s not found (fopen %lld ms)", i, date_str, (t1 - t0) / 1000);
            continue;
        }

        ESP_LOGI(TAG, "  [%d] %s: fopen %lld ms", i, date_str, (t1 - t0) / 1000);

        if (offset > 0 && offset < size - 4) {
            offset += snprintf(buf + offset, size - offset, "\n---\n");
        }

        t0 = esp_timer_get_time();
        size_t n = fread(buf + offset, 1, size - offset - 1, f);
        t1 = esp_timer_get_time();
        ESP_LOGI(TAG, "      fread %d bytes %lld ms", (int)n, (t1 - t0) / 1000);
        
        offset += n;
        buf[offset] = '\0';
        
        t0 = esp_timer_get_time();
        fclose(f);
        t1 = esp_timer_get_time();
        ESP_LOGI(TAG, "      fclose %lld ms, iter total %lld ms", 
                 (t1 - t0) / 1000, (t1 - iter_start) / 1000);
    }

    int64_t end = esp_timer_get_time();
    ESP_LOGI(TAG, "memory_read_recent total: %lld ms, %d bytes", (end - start) / 1000, (int)offset);
    return ESP_OK;
}
