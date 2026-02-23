#include "context_builder.h"
#include "mimi_config.h"
#include "memory/memory_store.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "context";

static size_t clamp_offset(size_t offset, size_t size)
{
    if (size == 0) return 0;
    return (offset < size) ? offset : (size - 1);
}

static size_t append_fmt(char *buf, size_t size, size_t offset, const char *fmt, ...)
{
    if (!buf || size == 0 || !fmt) {
        return clamp_offset(offset, size);
    }

    size_t off = clamp_offset(offset, size);

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + off, size - off, fmt, ap);
    va_end(ap);

    if (n < 0) {
        return off;
    }

    size_t avail = size - off - 1;
    if ((size_t)n > avail) {
        return size - 1;
    }
    return off + (size_t)n;
}

static size_t append_file(char *buf, size_t size, size_t offset, const char *path, const char *header)
{
    if (!buf || size == 0 || !path) return clamp_offset(offset, size);

    size_t off = clamp_offset(offset, size);
    FILE *f = fopen(path, "r");
    if (!f) return off;

    if (header) {
        off = append_fmt(buf, size, off, "\n## %s\n\n", header);
    }

    if (off < size - 1) {
        size_t n = fread(buf + off, 1, size - off - 1, f);
        off += n;
    }
    buf[off] = '\0';
    fclose(f);
    return off;
}

esp_err_t context_build_system_prompt(char *buf, size_t size)
{
    if (!buf || size == 0) return ESP_ERR_INVALID_ARG;

    /* Keep large scratch buffers off task stack to avoid stack overflow corruption. */
    static char s_mem_buf[4096];
    static char s_recent_buf[4096];

    size_t off = 0;
    buf[0] = '\0';

    off = append_fmt(buf, size, off,
        "# MimiClaw\n\n"
        "You are MimiClaw, a personal AI assistant running on an ESP32-S3 device.\n"
        "You communicate through Telegram and WebSocket.\n\n"
        "Be helpful, accurate, and concise.\n\n"
        "## Available Tools\n"
        "You have access to the following tools:\n"
        "- web_search: Search the web for current information. "
        "Use this when you need up-to-date facts, news, weather, or anything beyond your training data.\n"
        "- web_fetch: Fetch content from a specific URL (http/https). "
        "Use this when the user provides a direct link and wants the page content.\n"
        "- get_current_time: Get the current date and time. "
        "You do NOT have an internal clock — always use this tool when you need to know the time or date.\n"
        "- read_file: Read a file from SPIFFS (path must start with /spiffs/).\n"
        "- write_file: Write/overwrite a file on SPIFFS.\n"
        "- edit_file: Find-and-replace edit a file on SPIFFS.\n"
        "- list_dir: List files on SPIFFS, optionally filter by prefix.\n\n"
        "Use tools when needed. Provide your final answer as text after using tools.\n\n"
        "## Memory\n"
        "You have persistent memory stored on local flash:\n"
        "- Long-term memory: /spiffs/memory/MEMORY.md\n"
        "- Daily notes: /spiffs/memory/daily/<YYYY-MM-DD>.md\n\n"
        "IMPORTANT: Actively use memory to remember things across conversations.\n"
        "- When you learn something new about the user (name, preferences, habits, context), write it to MEMORY.md.\n"
        "- When something noteworthy happens in a conversation, append it to today's daily note.\n"
        "- Always read_file MEMORY.md before writing, so you can edit_file to update without losing existing content.\n"
        "- Use get_current_time to know today's date before writing daily notes.\n"
        "- Keep MEMORY.md concise and organized — summarize, don't dump raw conversation.\n"
        "- You should proactively save memory without being asked. If the user tells you their name, preferences, or important facts, persist them immediately.\n");

    /* Bootstrap files */
    off = append_file(buf, size, off, MIMI_AGENTS_FILE, "Agent Rules");
    off = append_file(buf, size, off, MIMI_SOUL_FILE, "Personality");
    off = append_file(buf, size, off, MIMI_USER_FILE, "User Info");

    /* Long-term memory */
    if (memory_read_long_term(s_mem_buf, sizeof(s_mem_buf)) == ESP_OK && s_mem_buf[0]) {
        off = append_fmt(buf, size, off, "\n## Long-term Memory\n\n%s\n", s_mem_buf);
    }

    /* Recent daily notes (last 3 days) */
    if (memory_read_recent(s_recent_buf, sizeof(s_recent_buf), 3) == ESP_OK && s_recent_buf[0]) {
        off = append_fmt(buf, size, off, "\n## Recent Notes\n\n%s\n", s_recent_buf);
    }

    ESP_LOGI(TAG, "System prompt built: %d bytes", (int)off);
    return ESP_OK;
}

esp_err_t context_build_messages(const char *history_json, const char *user_message,
                                 char *buf, size_t size)
{
    /* Parse existing history */
    cJSON *history = cJSON_Parse(history_json);
    if (!history) {
        history = cJSON_CreateArray();
    }

    /* Append current user message */
    cJSON *user_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(user_msg, "role", "user");
    cJSON_AddStringToObject(user_msg, "content", user_message);
    cJSON_AddItemToArray(history, user_msg);

    /* Serialize */
    char *json_str = cJSON_PrintUnformatted(history);
    cJSON_Delete(history);

    if (json_str) {
        strncpy(buf, json_str, size - 1);
        buf[size - 1] = '\0';
        free(json_str);
    } else {
        snprintf(buf, size, "[{\"role\":\"user\",\"content\":\"%s\"}]", user_message);
    }

    return ESP_OK;
}
