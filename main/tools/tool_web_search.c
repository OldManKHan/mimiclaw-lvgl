#include "tool_web_search.h"
#include "mimi_config.h"
#include "proxy/http_proxy.h"

#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "cJSON.h"

static const char *TAG = "web_search";

static char s_search_key[128] = {0};

#define SEARCH_BUF_SIZE     (16 * 1024)
#define SEARCH_RESULT_COUNT 5

typedef enum {
    SEARCH_PROVIDER_BRAVE = 0,
    SEARCH_PROVIDER_TAVILY = 1,
} search_provider_t;

/* ── Response accumulator ─────────────────────────────────────── */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} search_buf_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    search_buf_t *sb = (search_buf_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        size_t needed = sb->len + evt->data_len;
        if (needed < sb->cap) {
            memcpy(sb->data + sb->len, evt->data, evt->data_len);
            sb->len += evt->data_len;
            sb->data[sb->len] = '\0';
        }
    }
    return ESP_OK;
}

static bool is_tavily_key(const char *key)
{
    return key && strncmp(key, "tvly-", 5) == 0;
}

static search_provider_t get_search_provider(void)
{
    return is_tavily_key(s_search_key) ? SEARCH_PROVIDER_TAVILY : SEARCH_PROVIDER_BRAVE;
}

/* ── Init ─────────────────────────────────────────────────────── */

esp_err_t tool_web_search_init(void)
{
    /* Start with build-time default */
    if (MIMI_SECRET_SEARCH_KEY[0] != '\0') {
        strncpy(s_search_key, MIMI_SECRET_SEARCH_KEY, sizeof(s_search_key) - 1);
    }

    /* NVS overrides take highest priority (set via CLI) */
    nvs_handle_t nvs;
    if (nvs_open(MIMI_NVS_SEARCH, NVS_READONLY, &nvs) == ESP_OK) {
        char tmp[128] = {0};
        size_t len = sizeof(tmp);
        if (nvs_get_str(nvs, MIMI_NVS_KEY_API_KEY, tmp, &len) == ESP_OK && tmp[0]) {
            strncpy(s_search_key, tmp, sizeof(s_search_key) - 1);
        }
        nvs_close(nvs);
    }

    if (s_search_key[0]) {
        ESP_LOGI(TAG, "Web search initialized (provider=%s)",
                 get_search_provider() == SEARCH_PROVIDER_TAVILY ? "tavily" : "brave");
    } else {
        ESP_LOGW(TAG, "No search API key. Use CLI: set_search_key <KEY>");
    }
    return ESP_OK;
}

/* ── URL-encode a query string ────────────────────────────────── */

static size_t url_encode(const char *src, char *dst, size_t dst_size)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t pos = 0;

    for (; *src && pos < dst_size - 3; src++) {
        unsigned char c = (unsigned char)*src;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            dst[pos++] = c;
        } else if (c == ' ') {
            dst[pos++] = '+';
        } else {
            dst[pos++] = '%';
            dst[pos++] = hex[c >> 4];
            dst[pos++] = hex[c & 0x0F];
        }
    }
    dst[pos] = '\0';
    return pos;
}

/* ── Format results as readable text ──────────────────────────── */

static void format_results(cJSON *root, char *output, size_t output_size)
{
    cJSON *web = cJSON_GetObjectItem(root, "web");
    cJSON *results = NULL;
    bool tavily_mode = false;
    if (web) {
        results = cJSON_GetObjectItem(web, "results");
    } else {
        results = cJSON_GetObjectItem(root, "results");
        tavily_mode = true;
    }
    if (!results || !cJSON_IsArray(results) || cJSON_GetArraySize(results) == 0) {
        snprintf(output, output_size, "No web results found.");
        return;
    }

    size_t off = 0;
    int idx = 0;
    cJSON *item;
    cJSON_ArrayForEach(item, results) {
        if (idx >= SEARCH_RESULT_COUNT) break;

        cJSON *title = cJSON_GetObjectItem(item, "title");
        cJSON *url = cJSON_GetObjectItem(item, "url");
        cJSON *desc = cJSON_GetObjectItem(item, tavily_mode ? "content" : "description");

        off += snprintf(output + off, output_size - off,
            "%d. %s\n   %s\n   %s\n\n",
            idx + 1,
            (title && cJSON_IsString(title)) ? title->valuestring : "(no title)",
            (url && cJSON_IsString(url)) ? url->valuestring : "",
            (desc && cJSON_IsString(desc)) ? desc->valuestring : "");

        if (off >= output_size - 1) break;
        idx++;
    }
}

/* ── Direct HTTPS request ─────────────────────────────────────── */

static esp_err_t brave_search_direct(const char *url, search_buf_t *sb, int *status_out)
{
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = sb,
        .timeout_ms = 15000,
        .buffer_size = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_FAIL;

    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "X-Subscription-Token", s_search_key);

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (status_out) *status_out = status;

    if (err != ESP_OK) return err;
    return ESP_OK;
}

/* ── Proxy HTTPS request ──────────────────────────────────────── */

static esp_err_t brave_search_via_proxy(const char *path, search_buf_t *sb, int *status_out)
{
    proxy_conn_t *conn = proxy_conn_open("api.search.brave.com", 443, 15000);
    if (!conn) return ESP_ERR_HTTP_CONNECT;

    char header[512];
    int hlen = snprintf(header, sizeof(header),
        "GET %s HTTP/1.1\r\n"
        "Host: api.search.brave.com\r\n"
        "Accept: application/json\r\n"
        "X-Subscription-Token: %s\r\n"
        "Connection: close\r\n\r\n",
        path, s_search_key);

    if (proxy_conn_write(conn, header, hlen) < 0) {
        proxy_conn_close(conn);
        return ESP_ERR_HTTP_WRITE_DATA;
    }

    /* Read full response */
    char tmp[4096];
    size_t total = 0;
    while (1) {
        int n = proxy_conn_read(conn, tmp, sizeof(tmp), 15000);
        if (n <= 0) break;
        size_t copy = (total + n < sb->cap - 1) ? (size_t)n : sb->cap - 1 - total;
        if (copy > 0) {
            memcpy(sb->data + total, tmp, copy);
            total += copy;
        }
    }
    sb->data[total] = '\0';
    sb->len = total;
    proxy_conn_close(conn);

    /* Check status */
    int status = 0;
    if (total > 5 && strncmp(sb->data, "HTTP/", 5) == 0) {
        const char *sp = strchr(sb->data, ' ');
        if (sp) status = atoi(sp + 1);
    }
    if (status_out) *status_out = status;

    /* Strip headers */
    char *body = strstr(sb->data, "\r\n\r\n");
    if (body) {
        body += 4;
        size_t blen = total - (body - sb->data);
        memmove(sb->data, body, blen);
        sb->len = blen;
        sb->data[sb->len] = '\0';
    }

    return ESP_OK;
}

static esp_err_t tavily_search_direct(const char *body, search_buf_t *sb, int *status_out)
{
    esp_http_client_config_t config = {
        .url = "https://api.tavily.com/search",
        .event_handler = http_event_handler,
        .user_data = sb,
        .timeout_ms = 15000,
        .buffer_size = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_FAIL;

    char auth[180];
    snprintf(auth, sizeof(auth), "Bearer %s", s_search_key);

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_post_field(client, body, (int)strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (status_out) *status_out = status;

    return err;
}

static esp_err_t tavily_search_via_proxy(const char *body, search_buf_t *sb, int *status_out)
{
    proxy_conn_t *conn = proxy_conn_open("api.tavily.com", 443, 15000);
    if (!conn) return ESP_ERR_HTTP_CONNECT;

    char auth[180];
    snprintf(auth, sizeof(auth), "Bearer %s", s_search_key);

    char header[768];
    int body_len = (int)strlen(body);
    int hlen = snprintf(header, sizeof(header),
        "POST /search HTTP/1.1\r\n"
        "Host: api.tavily.com\r\n"
        "Accept: application/json\r\n"
        "Content-Type: application/json\r\n"
        "Authorization: %s\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n\r\n",
        auth, body_len);

    if (proxy_conn_write(conn, header, hlen) < 0 ||
        proxy_conn_write(conn, body, body_len) < 0) {
        proxy_conn_close(conn);
        return ESP_ERR_HTTP_WRITE_DATA;
    }

    char tmp[4096];
    size_t total = 0;
    while (1) {
        int n = proxy_conn_read(conn, tmp, sizeof(tmp), 15000);
        if (n <= 0) break;
        size_t copy = (total + n < sb->cap - 1) ? (size_t)n : sb->cap - 1 - total;
        if (copy > 0) {
            memcpy(sb->data + total, tmp, copy);
            total += copy;
        }
    }
    sb->data[total] = '\0';
    sb->len = total;
    proxy_conn_close(conn);

    int status = 0;
    if (total > 5 && strncmp(sb->data, "HTTP/", 5) == 0) {
        const char *sp = strchr(sb->data, ' ');
        if (sp) status = atoi(sp + 1);
    }
    if (status_out) *status_out = status;

    char *resp_body = strstr(sb->data, "\r\n\r\n");
    if (resp_body) {
        resp_body += 4;
        size_t blen = total - (resp_body - sb->data);
        memmove(sb->data, resp_body, blen);
        sb->len = blen;
        sb->data[sb->len] = '\0';
    }

    return ESP_OK;
}

/* ── Execute ──────────────────────────────────────────────────── */

esp_err_t tool_web_search_execute(const char *input_json, char *output, size_t output_size)
{
    if (s_search_key[0] == '\0') {
        snprintf(output, output_size, "Error: No search API key configured. Set MIMI_SECRET_SEARCH_KEY in mimi_secrets.h");
        return ESP_ERR_INVALID_STATE;
    }

    /* Parse input to get query */
    cJSON *input = cJSON_Parse(input_json);
    if (!input) {
        snprintf(output, output_size, "Error: Invalid input JSON");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *query = cJSON_GetObjectItem(input, "query");
    if (!query || !cJSON_IsString(query) || query->valuestring[0] == '\0') {
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: Missing 'query' field");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Searching: %s", query->valuestring);

    search_provider_t provider = get_search_provider();
    cJSON *tavily_req = NULL;
    char *tavily_body = NULL;
    char path[384] = {0};
    char url[512] = {0};

    if (provider == SEARCH_PROVIDER_TAVILY) {
        tavily_req = cJSON_CreateObject();
        if (!tavily_req) {
            cJSON_Delete(input);
            snprintf(output, output_size, "Error: Out of memory");
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddStringToObject(tavily_req, "query", query->valuestring);
        cJSON_AddNumberToObject(tavily_req, "max_results", SEARCH_RESULT_COUNT);
        cJSON_AddStringToObject(tavily_req, "search_depth", "basic");
        tavily_body = cJSON_PrintUnformatted(tavily_req);
        cJSON_Delete(tavily_req);
        if (!tavily_body) {
            cJSON_Delete(input);
            snprintf(output, output_size, "Error: Failed to build Tavily request");
            return ESP_ERR_NO_MEM;
        }
    } else {
        char encoded_query[256];
        url_encode(query->valuestring, encoded_query, sizeof(encoded_query));
        snprintf(path, sizeof(path),
                 "/res/v1/web/search?q=%s&count=%d", encoded_query, SEARCH_RESULT_COUNT);
        snprintf(url, sizeof(url), "https://api.search.brave.com%s", path);
    }
    cJSON_Delete(input);

    /* Allocate response buffer from PSRAM */
    search_buf_t sb = {0};
    sb.data = heap_caps_calloc(1, SEARCH_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!sb.data) {
        snprintf(output, output_size, "Error: Out of memory");
        return ESP_ERR_NO_MEM;
    }
    sb.cap = SEARCH_BUF_SIZE;

    /* Make HTTP request */
    esp_err_t err = ESP_FAIL;
    int status = 0;
    if (provider == SEARCH_PROVIDER_TAVILY) {
        if (http_proxy_is_enabled()) {
            err = tavily_search_via_proxy(tavily_body, &sb, &status);
        } else {
            err = tavily_search_direct(tavily_body, &sb, &status);
        }
    } else {
        if (http_proxy_is_enabled()) {
            err = brave_search_via_proxy(path, &sb, &status);
        } else {
            err = brave_search_direct(url, &sb, &status);
        }
    }
    free(tavily_body);

    if (err != ESP_OK) {
        free(sb.data);
        snprintf(output, output_size, "Error: Search request failed");
        return err;
    }
    if (status != 200) {
        ESP_LOGE(TAG, "Search API returned %d (provider=%s) body=%.240s",
                 status,
                 provider == SEARCH_PROVIDER_TAVILY ? "tavily" : "brave",
                 sb.data ? sb.data : "");
        snprintf(output, output_size, "Error: Search API returned HTTP %d", status);
        free(sb.data);
        return ESP_FAIL;
    }

    /* Parse and format results */
    cJSON *root = cJSON_Parse(sb.data);
    free(sb.data);

    if (!root) {
        snprintf(output, output_size, "Error: Failed to parse search results");
        return ESP_FAIL;
    }

    format_results(root, output, output_size);
    cJSON_Delete(root);

    ESP_LOGI(TAG, "Search complete, %d bytes result", (int)strlen(output));
    return ESP_OK;
}

esp_err_t tool_web_search_set_key(const char *api_key)
{
    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open(MIMI_NVS_SEARCH, NVS_READWRITE, &nvs));
    ESP_ERROR_CHECK(nvs_set_str(nvs, MIMI_NVS_KEY_API_KEY, api_key));
    ESP_ERROR_CHECK(nvs_commit(nvs));
    nvs_close(nvs);

    strncpy(s_search_key, api_key, sizeof(s_search_key) - 1);
    ESP_LOGI(TAG, "Search API key saved (provider=%s)",
             get_search_provider() == SEARCH_PROVIDER_TAVILY ? "tavily" : "brave");
    return ESP_OK;
}
