#include "memory_store.h"
#include "mimi_config.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <stdarg.h>
#include "esp_log.h"

static const char *TAG = "memory";

static size_t clamp_offset(size_t offset, size_t size)
{
    if (size == 0) return 0;
    return (offset < size) ? offset : (size - 1);
}

static size_t append_fmt(char *buf, size_t size, size_t offset, const char *fmt, ...)
{
    if (!buf || size == 0 || !fmt) return clamp_offset(offset, size);

    size_t off = clamp_offset(offset, size);

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + off, size - off, fmt, ap);
    va_end(ap);

    if (n < 0) return off;

    size_t avail = size - off - 1;
    if ((size_t)n > avail) return size - 1;
    return off + (size_t)n;
}

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
    FILE *f = fopen(MIMI_MEMORY_FILE, "r");
    if (!f) {
        buf[0] = '\0';
        return ESP_ERR_NOT_FOUND;
    }

    size_t n = fread(buf, 1, size - 1, f);
    buf[n] = '\0';
    fclose(f);
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
    if (!buf || size == 0) return ESP_ERR_INVALID_ARG;

    size_t offset = 0;
    buf[0] = '\0';

    for (int i = 0; i < days && offset < size - 1; i++) {
        char date_str[16];
        get_date_str(date_str, sizeof(date_str), i);

        char path[64];
        snprintf(path, sizeof(path), "%s/%s.md", MIMI_SPIFFS_MEMORY_DIR, date_str);

        FILE *f = fopen(path, "r");
        if (!f) continue;

        if (offset > 0) {
            offset = append_fmt(buf, size, offset, "\n---\n");
        }

        if (offset < size - 1) {
            size_t n = fread(buf + offset, 1, size - offset - 1, f);
            offset += n;
            buf[offset] = '\0';
        }
        fclose(f);
    }

    return ESP_OK;
}
