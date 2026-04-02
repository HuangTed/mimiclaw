#include "skills/skill_loader.h"
#include "mimi_config.h"

#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "skills";

/*
 * Skills are stored as markdown files in spiffs_data/skills/
 * and flashed into the SPIFFS partition at build time.
 */

esp_err_t skill_loader_init(void)
{
    ESP_LOGI(TAG, "Initializing skills system");

    DIR *dir = opendir(MIMI_SPIFFS_BASE);
    if (!dir) {
        ESP_LOGW(TAG, "Cannot open SPIFFS — skills may not be available");
        return ESP_OK;
    }

    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        const char *name = ent->d_name;
        size_t len = strlen(name);
        if (strncmp(name, "skills/", 7) == 0 && len > 10 &&
            strcmp(name + len - 3, ".md") == 0) {
            count++;
        }
    }
    closedir(dir);

    ESP_LOGI(TAG, "Skills system ready (%d skills on SPIFFS)", count);
    return ESP_OK;
}

/* ── Build skills summary for system prompt ──────────────────── */

/**
 * Parse first line as title: expects "# Title".
 * Writes the title (without "# " prefix) into out.
 */
static void extract_title(const char *line, size_t len, char *out, size_t out_size)
{
    const char *start = line;
    if (len >= 2 && line[0] == '#' && line[1] == ' ') {
        start = line + 2;
        len -= 2;
    }

    /* Trim trailing whitespace/newline */
    while (len > 0 && (start[len - 1] == '\n' || start[len - 1] == '\r' || start[len - 1] == ' ')) {
        len--;
    }

    size_t copy = len < out_size - 1 ? len : out_size - 1;
    memcpy(out, start, copy);
    out[copy] = '\0';
}

/**
 * Extract description: text between the first line and the first blank line.
 */
static void extract_description(FILE *f, char *out, size_t out_size)
{
    size_t off = 0;
    char line[256];

    while (fgets(line, sizeof(line), f) && off < out_size - 1) {
        size_t len = strlen(line);

        /* Stop at blank line or section header */
        if (len == 0 || (len == 1 && line[0] == '\n') ||
            (len >= 2 && line[0] == '#' && line[1] == '#')) {
            break;
        }

        /* Skip leading blank lines */
        if (off == 0 && line[0] == '\n') continue;

        /* Trim trailing newline for concatenation */
        if (line[len - 1] == '\n') {
            line[len - 1] = ' ';
        }

        size_t copy = len < out_size - off - 1 ? len : out_size - off - 1;
        memcpy(out + off, line, copy);
        off += copy;
    }

    /* Trim trailing space */
    while (off > 0 && out[off - 1] == ' ') off--;
    out[off] = '\0';
}

size_t skill_loader_build_summary(char *buf, size_t size)
{
    int64_t start = esp_timer_get_time();
    int64_t t0, t1;
    
    t0 = esp_timer_get_time();
    DIR *dir = opendir(MIMI_SPIFFS_BASE);
    t1 = esp_timer_get_time();
    
    if (!dir) {
        ESP_LOGW(TAG, "Cannot open SPIFFS for skill enumeration");
        buf[0] = '\0';
        return 0;
    }
    ESP_LOGI(TAG, "  opendir: %lld ms", (t1 - t0) / 1000);

    size_t off = 0;
    int skill_count = 0;
    struct dirent *ent;
    /* SPIFFS readdir returns filenames relative to the mount point (e.g. "skills/weather.md").
       We match entries that start with "skills/" and end with ".md". */
    const char *skills_subdir = "skills/";
    const size_t subdir_len = strlen(skills_subdir);
    ESP_LOGI(TAG, "Start to read Skills");

    int64_t readdir_total = 0;
    int64_t fileio_total = 0;
    
    while ((ent = readdir(dir)) != NULL && off < size - 1) {
        int64_t iter_start = esp_timer_get_time();
        const char *name = ent->d_name;

        /* Match files under skills/ with .md extension */
        if (strncmp(name, skills_subdir, subdir_len) != 0) continue;

        size_t name_len = strlen(name);
        if (name_len < subdir_len + 4) continue;  /* at least "skills/x.md" */
        if (strcmp(name + name_len - 3, ".md") != 0) continue;

        /* Build full path */
        char full_path[296];
        snprintf(full_path, sizeof(full_path), "%s/%s", MIMI_SPIFFS_BASE, name);

        int64_t ft0 = esp_timer_get_time();
        FILE *f = fopen(full_path, "r");
        int64_t ft1 = esp_timer_get_time();
        int64_t fopen_ms = (ft1 - ft0) / 1000;
        
        if (!f) continue;

        /* Read first line for title */
        char first_line[128];
        ft0 = esp_timer_get_time();
        bool got_line = fgets(first_line, sizeof(first_line), f);
        ft1 = esp_timer_get_time();
        int64_t fgets1_ms = (ft1 - ft0) / 1000;
        
        if (!got_line) {
            fclose(f);
            continue;
        }

        char title[64];
        extract_title(first_line, strlen(first_line), title, sizeof(title));

        /* Read description (until blank line) */
        char desc[256];
        ft0 = esp_timer_get_time();
        extract_description(f, desc, sizeof(desc));
        ft1 = esp_timer_get_time();
        int64_t desc_ms = (ft1 - ft0) / 1000;
        
        ft0 = esp_timer_get_time();
        fclose(f);
        ft1 = esp_timer_get_time();
        int64_t fclose_ms = (ft1 - ft0) / 1000;
        
        int64_t fileio_this = fopen_ms + fgets1_ms + desc_ms + fclose_ms;
        fileio_total += fileio_this;
        
        ESP_LOGI(TAG, "  [%d] %s: fopen %lld ms, fgets %lld ms, desc %lld ms, fclose %lld ms, total %lld ms",
                 skill_count, name, fopen_ms, fgets1_ms, desc_ms, fclose_ms, fileio_this);

        /* Append to summary */
        off += snprintf(buf + off, size - off,
            "- **%s**: %s (read with: read_file %s)\n",
            title, desc, full_path);
        
        skill_count++;
        int64_t iter_end = esp_timer_get_time();
        readdir_total += (iter_end - iter_start);
    }

    t0 = esp_timer_get_time();
    closedir(dir);
    t1 = esp_timer_get_time();
    ESP_LOGI(TAG, "  closedir: %lld ms", (t1 - t0) / 1000);

    buf[off] = '\0';
    int64_t end = esp_timer_get_time();
    ESP_LOGI(TAG, "Skills summary: %d bytes, %d skills, fileio %lld ms, total %lld ms", 
             (int)off, skill_count, fileio_total, (end - start) / 1000);
    return off;
}
