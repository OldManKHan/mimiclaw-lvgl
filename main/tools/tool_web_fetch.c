#include "tool_web_fetch.h"
#include "proxy/http_proxy.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "cJSON.h"

static const char *TAG = "web_fetch";

#define FETCH_BUF_SIZE       (16 * 1024)
#define FETCH_TIMEOUT_MS     15000
#define FETCH_MAX_URL_LEN    1024

typedef struct {
    char *data;
    size_t len;
    size_t cap;
    bool overflow;
} fetch_buf_t;

typedef struct {
    bool https;
    char host[192];
    int port;
    char path[768];
} parsed_url_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    fetch_buf_t *fb = (fetch_buf_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        size_t needed = fb->len + evt->data_len;
        if (needed < fb->cap) {
            memcpy(fb->data + fb->len, evt->data, evt->data_len);
            fb->len += evt->data_len;
            fb->data[fb->len] = '\0';
        } else {
            fb->overflow = true;
        }
    }
    return ESP_OK;
}

static bool is_space_char(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static bool trim_copy_url(const char *src, char *dst, size_t dst_size)
{
    if (!src || !dst || dst_size == 0) return false;

    const char *start = src;
    while (*start && is_space_char(*start)) start++;

    const char *end = src + strlen(src);
    while (end > start && is_space_char(*(end - 1))) end--;

    size_t len = (size_t)(end - start);
    if (len == 0 || len >= dst_size) return false;

    memcpy(dst, start, len);
    dst[len] = '\0';
    return true;
}

static bool parse_url(const char *url, parsed_url_t *out)
{
    if (!url || !out) return false;
    memset(out, 0, sizeof(*out));

    const char *p = NULL;
    if (strncmp(url, "https://", 8) == 0) {
        out->https = true;
        out->port = 443;
        p = url + 8;
    } else if (strncmp(url, "http://", 7) == 0) {
        out->https = false;
        out->port = 80;
        p = url + 7;
    } else {
        return false;
    }

    const char *slash = strchr(p, '/');
    const char *qmark = strchr(p, '?');
    const char *hash = strchr(p, '#');

    const char *host_end = p + strlen(p);
    if (slash && slash < host_end) host_end = slash;
    if (qmark && qmark < host_end) host_end = qmark;
    if (hash && hash < host_end) host_end = hash;

    if (host_end <= p) return false;

    char host_port[256];
    size_t hp_len = (size_t)(host_end - p);
    if (hp_len == 0 || hp_len >= sizeof(host_port)) return false;
    memcpy(host_port, p, hp_len);
    host_port[hp_len] = '\0';

    /* Keep parser simple: userinfo and IPv6 literals are unsupported. */
    if (strchr(host_port, '@') || host_port[0] == '[') return false;

    char *colon = strrchr(host_port, ':');
    if (colon) {
        *colon = '\0';
        int port = atoi(colon + 1);
        if (port <= 0 || port > 65535) return false;
        out->port = port;
    }

    if (host_port[0] == '\0' || strlen(host_port) >= sizeof(out->host)) return false;
    strncpy(out->host, host_port, sizeof(out->host) - 1);

    if (slash && slash == host_end) {
        if (strlen(slash) >= sizeof(out->path)) return false;
        strncpy(out->path, slash, sizeof(out->path) - 1);
    } else if (qmark && qmark == host_end) {
        if (strlen(qmark) + 1 >= sizeof(out->path)) return false;
        out->path[0] = '/';
        strncpy(out->path + 1, qmark, sizeof(out->path) - 2);
    } else if (hash && hash == host_end) {
        if (strlen(hash) + 1 >= sizeof(out->path)) return false;
        out->path[0] = '/';
        strncpy(out->path + 1, hash, sizeof(out->path) - 2);
    } else {
        strcpy(out->path, "/");
    }

    return true;
}

static esp_err_t fetch_direct(const char *url, fetch_buf_t *fb, int *status_out)
{
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = fb,
        .timeout_ms = FETCH_TIMEOUT_MS,
        .buffer_size = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_FAIL;

    esp_http_client_set_header(client, "Accept", "text/html, text/plain;q=0.9,*/*;q=0.1");
    esp_http_client_set_header(client, "Accept-Encoding", "identity");
    esp_http_client_set_header(client, "User-Agent", "MimiClaw/1.0");

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (status_out) *status_out = status;
    if (err != ESP_OK) return err;
    if (status < 200 || status >= 300) return ESP_FAIL;
    return ESP_OK;
}

static esp_err_t fetch_via_proxy(const parsed_url_t *url, fetch_buf_t *fb, int *status_out)
{
    proxy_conn_t *conn = proxy_conn_open(url->host, url->port, FETCH_TIMEOUT_MS);
    if (!conn) return ESP_ERR_HTTP_CONNECT;

    char header[1400];
    int hlen = snprintf(header, sizeof(header),
        "GET %s HTTP/1.0\r\n"
        "Host: %s\r\n"
        "Accept: text/html, text/plain;q=0.9,*/*;q=0.1\r\n"
        "Accept-Encoding: identity\r\n"
        "User-Agent: MimiClaw/1.0\r\n"
        "Connection: close\r\n\r\n",
        url->path, url->host);

    if (hlen <= 0 || hlen >= (int)sizeof(header)) {
        proxy_conn_close(conn);
        return ESP_ERR_INVALID_SIZE;
    }

    if (proxy_conn_write(conn, header, hlen) < 0) {
        proxy_conn_close(conn);
        return ESP_ERR_HTTP_WRITE_DATA;
    }

    char tmp[2048];
    size_t total = 0;
    while (1) {
        int n = proxy_conn_read(conn, tmp, sizeof(tmp), FETCH_TIMEOUT_MS);
        if (n <= 0) break;

        size_t copy = (total + (size_t)n < fb->cap - 1) ? (size_t)n : (fb->cap - 1 - total);
        if (copy > 0) {
            memcpy(fb->data + total, tmp, copy);
            total += copy;
        }
        if (copy < (size_t)n) {
            fb->overflow = true;
        }
    }
    fb->data[total] = '\0';
    fb->len = total;
    proxy_conn_close(conn);

    int status = 0;
    if (total > 5 && strncmp(fb->data, "HTTP/", 5) == 0) {
        const char *sp = strchr(fb->data, ' ');
        if (sp) status = atoi(sp + 1);
    }
    if (status_out) *status_out = status;

    char *body = strstr(fb->data, "\r\n\r\n");
    if (!body) return ESP_FAIL;

    body += 4;
    size_t body_len = total - (size_t)(body - fb->data);
    memmove(fb->data, body, body_len);
    fb->len = body_len;
    fb->data[fb->len] = '\0';

    if (status < 200 || status >= 300) return ESP_FAIL;
    return ESP_OK;
}

static size_t copy_text_sanitized(char *dst, size_t dst_size, const char *src, size_t src_len)
{
    if (!dst || dst_size == 0 || !src) return 0;

    size_t pos = 0;
    for (size_t i = 0; i < src_len && pos < dst_size - 1; i++) {
        unsigned char c = (unsigned char)src[i];

        if (c == '\r') {
            continue;
        }

        if (c == '\n' || c == '\t' || c >= 0x20) {
            dst[pos++] = (char)c;
        }
    }

    dst[pos] = '\0';
    return pos;
}

esp_err_t tool_web_fetch_execute(const char *input_json, char *output, size_t output_size)
{
    if (!output || output_size == 0) return ESP_ERR_INVALID_ARG;
    if (!input_json) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *input = cJSON_Parse(input_json);
    if (!input) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *url_item = cJSON_GetObjectItem(input, "url");
    if (!url_item || !cJSON_IsString(url_item) || url_item->valuestring[0] == '\0') {
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: missing 'url' field");
        return ESP_ERR_INVALID_ARG;
    }

    char url[FETCH_MAX_URL_LEN];
    if (!trim_copy_url(url_item->valuestring, url, sizeof(url))) {
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: invalid 'url' value");
        return ESP_ERR_INVALID_ARG;
    }
    cJSON_Delete(input);

    parsed_url_t parsed;
    if (!parse_url(url, &parsed)) {
        snprintf(output, output_size, "Error: unsupported URL. Use http:// or https://");
        return ESP_ERR_INVALID_ARG;
    }

    fetch_buf_t fb = {0};
    fb.data = heap_caps_calloc(1, FETCH_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!fb.data) {
        fb.data = calloc(1, FETCH_BUF_SIZE);
    }
    if (!fb.data) {
        snprintf(output, output_size, "Error: out of memory");
        return ESP_ERR_NO_MEM;
    }
    fb.cap = FETCH_BUF_SIZE;

    ESP_LOGI(TAG, "Fetching URL: %s", url);

    int status = 0;
    esp_err_t err;
    if (http_proxy_is_enabled() && parsed.https) {
        err = fetch_via_proxy(&parsed, &fb, &status);
    } else {
        err = fetch_direct(url, &fb, &status);
    }

    if (err != ESP_OK) {
        free(fb.data);
        if (status > 0) {
            snprintf(output, output_size, "Error: failed to fetch URL (HTTP %d)", status);
        } else {
            snprintf(output, output_size, "Error: failed to fetch URL (%s)", esp_err_to_name(err));
        }
        return err;
    }

    if (fb.len == 0) {
        free(fb.data);
        snprintf(output, output_size, "Error: fetch succeeded but response body is empty");
        return ESP_FAIL;
    }

    size_t off = snprintf(output, output_size, "Source: %s\n\n", url);
    if (off >= output_size) {
        off = output_size - 1;
        output[off] = '\0';
    }

    size_t copied = copy_text_sanitized(output + off, output_size - off, fb.data, fb.len);
    bool truncated = fb.overflow || (copied < fb.len && copied + off < output_size - 1);
    free(fb.data);

    if (copied == 0) {
        snprintf(output, output_size, "Error: fetched content is not readable text");
        return ESP_FAIL;
    }

    if (truncated) {
        size_t cur = strlen(output);
        if (cur + 15 < output_size) {
            strcat(output, "\n\n[truncated]");
        }
    }

    ESP_LOGI(TAG, "Fetch complete: %d bytes", (int)strlen(output));
    return ESP_OK;
}
