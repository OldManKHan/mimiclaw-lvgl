#include "feishu_bot.h"

#include "mimi_config.h"
#include "bus/message_bus.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>

#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_transport.h"
#include "esp_transport_ws.h"
#include "esp_transport_ssl.h"
#include "esp_transport_tcp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "cJSON.h"

static const char *TAG = "feishu";

#define FEISHU_DOMAIN                         "https://open.feishu.cn"
#define FEISHU_AUTH_URL                       FEISHU_DOMAIN "/open-apis/auth/v3/tenant_access_token/internal"
#define FEISHU_WS_ENDPOINT_URL                FEISHU_DOMAIN "/callback/ws/endpoint"
#define FEISHU_SEND_MSG_URL                   FEISHU_DOMAIN "/open-apis/im/v1/messages?receive_id_type=chat_id"

#define FEISHU_MAX_HEADERS                    16
#define FEISHU_KEY_MAX                        32
#define FEISHU_VAL_MAX                        128
#define FEISHU_URL_MAX                        512

#define FEISHU_CHUNK_CACHE_MAX                4
#define FEISHU_CHUNK_MAX                      16
#define FEISHU_CHUNK_EXPIRE_MS                10000
#define FEISHU_WS_PAYLOAD_MAX                 (96 * 1024)
#define FEISHU_EVENT_DEDUP_MAX                2048
#define FEISHU_EVENT_DEDUP_EXPIRE_MS          (24 * 60 * 60 * 1000)
#define FEISHU_EVENT_DEDUP_FLUSH_INTERVAL_MS  5000
#define FEISHU_EVENT_DEDUP_FLUSH_WRITES       16
#define FEISHU_EVENT_DEDUP_MAGIC              0x46444450u /* FDDP */
#define FEISHU_EVENT_DEDUP_FILE_VERSION       1
#define FEISHU_EVENT_DEDUP_FILE               MIMI_SPIFFS_BASE "/feishu_dedup.bin"
#define FEISHU_EVENT_DEDUP_FILE_TMP           MIMI_SPIFFS_BASE "/feishu_dedup.bin.tmp"

#define FEISHU_EVENT_IM_RECEIVE               "im.message.receive_v1"
#define FEISHU_MSG_TYPE_TEXT                  "text"

#define FEISHU_HTTP_TIMEOUT_MS                15000
#define FEISHU_TOKEN_SKEW_MS                  120000

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} http_resp_t;

typedef struct {
    char key[FEISHU_KEY_MAX];
    char value[FEISHU_VAL_MAX];
} pb_header_t;

typedef struct {
    uint64_t seq_id;
    uint64_t log_id;
    int32_t service;
    int32_t method;
    pb_header_t headers[FEISHU_MAX_HEADERS];
    size_t header_count;
    uint8_t *payload;
    size_t payload_len;
} pb_frame_t;

typedef struct {
    bool used;
    char message_id[64];
    char trace_id[64];
    int sum;
    int got;
    int64_t create_ms;
    uint8_t *parts[FEISHU_CHUNK_MAX];
    size_t part_lens[FEISHU_CHUNK_MAX];
} chunk_cache_t;

typedef struct {
    uint8_t *buf;
    size_t len;
    size_t cap;
} pb_writer_t;

typedef struct {
    bool used;
    char key[96];
    int64_t ts_ms;
} event_dedup_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    uint32_t count;
} dedup_file_header_t;

typedef struct {
    uint8_t used;
    int64_t ts_ms;
    char key[96];
} dedup_file_entry_t;

static char s_webhook_url[320] = MIMI_SECRET_FEISHU_WEBHOOK;
static char s_app_id[96] = MIMI_SECRET_FEISHU_APP_ID;
static char s_app_secret[128] = MIMI_SECRET_FEISHU_APP_SECRET;
static char s_default_chat_id[96] = MIMI_SECRET_FEISHU_DEFAULT_CHAT_ID;

static char s_tenant_token[640];
static int64_t s_tenant_token_expire_ms = 0;

static TaskHandle_t s_longconn_task = NULL;
static volatile bool s_longconn_should_run = false;
static esp_transport_handle_t s_ws_transport = NULL;

static int s_ws_service_id = 0;
static int s_ws_ping_interval_ms = 120000;
static int s_ws_reconnect_count = -1;
static int s_ws_reconnect_interval_ms = 120000;
static int s_ws_reconnect_nonce_ms = 30000;

static uint8_t *s_ws_rx_buf = NULL;
static size_t s_ws_rx_expected = 0;
static size_t s_ws_rx_received = 0;
static uint8_t s_ws_read_buf[4096];

static chunk_cache_t s_chunk_cache[FEISHU_CHUNK_CACHE_MAX];
static event_dedup_t *s_event_dedup = NULL;
static bool s_event_dedup_dirty = false;
static uint16_t s_event_dedup_dirty_writes = 0;
static int64_t s_event_dedup_last_flush_ms = 0;

static void str_copy(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t n = strlen(src);
    if (n >= dst_size) n = dst_size - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static uint32_t fnv1a32_str(const char *s)
{
    uint32_t h = 2166136261u;
    if (!s) return h;
    while (*s) {
        h ^= (uint8_t)(*s++);
        h *= 16777619u;
    }
    return h;
}

static void event_dedup_mark_dirty(void)
{
    s_event_dedup_dirty = true;
    if (s_event_dedup_dirty_writes < UINT16_MAX) {
        s_event_dedup_dirty_writes++;
    }
}

static esp_err_t event_dedup_save_to_disk(void)
{
    if (!s_event_dedup) return ESP_ERR_INVALID_STATE;

    FILE *f = fopen(FEISHU_EVENT_DEDUP_FILE_TMP, "wb");
    if (!f) {
        return ESP_FAIL;
    }

    dedup_file_header_t header = {
        .magic = FEISHU_EVENT_DEDUP_MAGIC,
        .version = FEISHU_EVENT_DEDUP_FILE_VERSION,
        .reserved = 0,
        .count = FEISHU_EVENT_DEDUP_MAX,
    };
    if (fwrite(&header, sizeof(header), 1, f) != 1) {
        fclose(f);
        remove(FEISHU_EVENT_DEDUP_FILE_TMP);
        return ESP_FAIL;
    }

    for (int i = 0; i < FEISHU_EVENT_DEDUP_MAX; i++) {
        dedup_file_entry_t entry = {0};
        entry.used = s_event_dedup[i].used ? 1 : 0;
        entry.ts_ms = s_event_dedup[i].ts_ms;
        str_copy(entry.key, sizeof(entry.key), s_event_dedup[i].key);
        if (fwrite(&entry, sizeof(entry), 1, f) != 1) {
            fclose(f);
            remove(FEISHU_EVENT_DEDUP_FILE_TMP);
            return ESP_FAIL;
        }
    }

    if (fclose(f) != 0) {
        remove(FEISHU_EVENT_DEDUP_FILE_TMP);
        return ESP_FAIL;
    }

    remove(FEISHU_EVENT_DEDUP_FILE);
    if (rename(FEISHU_EVENT_DEDUP_FILE_TMP, FEISHU_EVENT_DEDUP_FILE) != 0) {
        remove(FEISHU_EVENT_DEDUP_FILE_TMP);
        return ESP_FAIL;
    }

    s_event_dedup_dirty = false;
    s_event_dedup_dirty_writes = 0;
    s_event_dedup_last_flush_ms = now_ms();
    return ESP_OK;
}

static void event_dedup_maybe_flush(bool force)
{
    if (!s_event_dedup || !s_event_dedup_dirty) return;

    int64_t t = now_ms();
    bool enough_writes = s_event_dedup_dirty_writes >= FEISHU_EVENT_DEDUP_FLUSH_WRITES;
    bool time_due = (t - s_event_dedup_last_flush_ms) >= FEISHU_EVENT_DEDUP_FLUSH_INTERVAL_MS;

    if (!force && !enough_writes && !time_due) {
        return;
    }

    esp_err_t err = event_dedup_save_to_disk();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Dedup cache flush failed: %s", esp_err_to_name(err));
    }
}

static esp_err_t event_dedup_load_from_disk(void)
{
    if (!s_event_dedup) return ESP_ERR_INVALID_STATE;

    FILE *f = fopen(FEISHU_EVENT_DEDUP_FILE, "rb");
    if (!f) {
        return ESP_ERR_NOT_FOUND;
    }

    dedup_file_header_t header = {0};
    if (fread(&header, sizeof(header), 1, f) != 1) {
        fclose(f);
        return ESP_FAIL;
    }
    if (header.magic != FEISHU_EVENT_DEDUP_MAGIC ||
        header.version != FEISHU_EVENT_DEDUP_FILE_VERSION ||
        header.count != FEISHU_EVENT_DEDUP_MAX) {
        fclose(f);
        return ESP_ERR_INVALID_RESPONSE;
    }

    memset(s_event_dedup, 0, FEISHU_EVENT_DEDUP_MAX * sizeof(event_dedup_t));
    for (int i = 0; i < FEISHU_EVENT_DEDUP_MAX; i++) {
        dedup_file_entry_t entry = {0};
        if (fread(&entry, sizeof(entry), 1, f) != 1) {
            fclose(f);
            return ESP_FAIL;
        }
        s_event_dedup[i].used = (entry.used != 0);
        s_event_dedup[i].ts_ms = entry.ts_ms;
        str_copy(s_event_dedup[i].key, sizeof(s_event_dedup[i].key), entry.key);
    }
    fclose(f);

    int64_t t = now_ms();
    int expired = 0;
    for (int i = 0; i < FEISHU_EVENT_DEDUP_MAX; i++) {
        event_dedup_t *slot = &s_event_dedup[i];
        if (!slot->used) continue;
        if (t - slot->ts_ms > FEISHU_EVENT_DEDUP_EXPIRE_MS) {
            memset(slot, 0, sizeof(*slot));
            expired++;
        }
    }
    if (expired > 0) {
        event_dedup_mark_dirty();
    }

    return ESP_OK;
}

static bool event_dedup_contains(const char *key)
{
    if (!s_event_dedup || !key || !key[0]) return false;
    int64_t t = now_ms();
    bool changed = false;

    for (int i = 0; i < FEISHU_EVENT_DEDUP_MAX; i++) {
        event_dedup_t *slot = &s_event_dedup[i];
        if (!slot->used) continue;
        if (t - slot->ts_ms > FEISHU_EVENT_DEDUP_EXPIRE_MS) {
            memset(slot, 0, sizeof(*slot));
            changed = true;
            continue;
        }
        if (strcmp(slot->key, key) == 0) {
            if (changed) {
                event_dedup_mark_dirty();
            }
            return true;
        }
    }
    if (changed) {
        event_dedup_mark_dirty();
    }
    return false;
}

static void event_dedup_mark_seen(const char *key)
{
    if (!s_event_dedup || !key || !key[0]) return;

    int64_t t = now_ms();
    int free_idx = -1;
    int oldest_idx = 0;
    int64_t oldest_ts = INT64_MAX;

    for (int i = 0; i < FEISHU_EVENT_DEDUP_MAX; i++) {
        event_dedup_t *slot = &s_event_dedup[i];
        if (!slot->used) {
            if (free_idx < 0) free_idx = i;
            continue;
        }
        if (t - slot->ts_ms > FEISHU_EVENT_DEDUP_EXPIRE_MS) {
            memset(slot, 0, sizeof(*slot));
            event_dedup_mark_dirty();
            if (free_idx < 0) free_idx = i;
            continue;
        }
        if (strcmp(slot->key, key) == 0) {
            slot->ts_ms = t;
            event_dedup_mark_dirty();
            event_dedup_maybe_flush(false);
            return;
        }
        if (slot->ts_ms < oldest_ts) {
            oldest_ts = slot->ts_ms;
            oldest_idx = i;
        }
    }

    int idx = (free_idx >= 0) ? free_idx : oldest_idx;
    event_dedup_t *slot = &s_event_dedup[idx];
    memset(slot, 0, sizeof(*slot));
    slot->used = true;
    str_copy(slot->key, sizeof(slot->key), key);
    slot->ts_ms = t;
    event_dedup_mark_dirty();
    event_dedup_maybe_flush(false);
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_resp_t *resp = (http_resp_t *)evt->user_data;
    if (!resp || evt->event_id != HTTP_EVENT_ON_DATA) {
        return ESP_OK;
    }

    if (resp->len + evt->data_len + 1 > resp->cap) {
        size_t new_cap = resp->cap ? resp->cap * 2 : 1024;
        while (new_cap < resp->len + evt->data_len + 1) {
            new_cap *= 2;
        }
        char *tmp = realloc(resp->buf, new_cap);
        if (!tmp) {
            return ESP_ERR_NO_MEM;
        }
        resp->buf = tmp;
        resp->cap = new_cap;
    }

    memcpy(resp->buf + resp->len, evt->data, evt->data_len);
    resp->len += evt->data_len;
    resp->buf[resp->len] = '\0';
    return ESP_OK;
}

static esp_err_t http_json_request(const char *url,
                                   esp_http_client_method_t method,
                                   const char *bearer_token,
                                   const char *body,
                                   int timeout_ms,
                                   char **resp_out,
                                   int *status_out)
{
    if (!url || !resp_out) {
        return ESP_ERR_INVALID_ARG;
    }

    *resp_out = NULL;
    if (status_out) *status_out = 0;

    http_resp_t resp = {
        .buf = calloc(1, 1024),
        .len = 0,
        .cap = 1024,
    };
    if (!resp.buf) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t cfg = {
        .url = url,
        .method = method,
        .event_handler = http_event_handler,
        .user_data = &resp,
        .timeout_ms = timeout_ms,
        .buffer_size = 4096,
        .buffer_size_tx = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        free(resp.buf);
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json; charset=utf-8");
    if (bearer_token && bearer_token[0]) {
        char auth[720];
        snprintf(auth, sizeof(auth), "Bearer %s", bearer_token);
        esp_http_client_set_header(client, "Authorization", auth);
    }

    if (body) {
        esp_http_client_set_post_field(client, body, (int)strlen(body));
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (status_out) *status_out = status;

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP request failed: %s url=%s", esp_err_to_name(err), url);
        free(resp.buf);
        return err;
    }

    *resp_out = resp.buf;
    return ESP_OK;
}

static esp_err_t nvs_get_string(const char *ns, const char *key, char *out, size_t out_size)
{
    nvs_handle_t nvs;
    out[0] = '\0';

    esp_err_t err = nvs_open(ns, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    size_t len = out_size;
    err = nvs_get_str(nvs, key, out, &len);
    nvs_close(nvs);
    return err;
}

static esp_err_t nvs_set_string(const char *ns, const char *key, const char *value)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(ns, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;

    err = nvs_set_str(nvs, key, value ? value : "");
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

static esp_err_t nvs_erase_string(const char *ns, const char *key)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(ns, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;

    err = nvs_erase_key(nvs, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

static int parse_url_query_int(const char *url, const char *key, int fallback)
{
    if (!url || !key) return fallback;

    char pattern[64];
    snprintf(pattern, sizeof(pattern), "%s=", key);

    const char *q = strchr(url, '?');
    if (!q) return fallback;
    q++;

    const char *p = strstr(q, pattern);
    if (!p) return fallback;
    p += strlen(pattern);

    char tmp[24];
    size_t i = 0;
    while (p[i] && p[i] != '&' && i < sizeof(tmp) - 1) {
        tmp[i] = p[i];
        i++;
    }
    tmp[i] = '\0';
    if (tmp[0] == '\0') return fallback;
    return atoi(tmp);
}

static void chunk_slot_clear(chunk_cache_t *slot)
{
    if (!slot) return;
    for (int i = 0; i < FEISHU_CHUNK_MAX; i++) {
        free(slot->parts[i]);
        slot->parts[i] = NULL;
        slot->part_lens[i] = 0;
    }
    memset(slot, 0, sizeof(*slot));
}

static void chunk_cache_clear_expired(void)
{
    int64_t t = now_ms();
    for (int i = 0; i < FEISHU_CHUNK_CACHE_MAX; i++) {
        chunk_cache_t *slot = &s_chunk_cache[i];
        if (slot->used && t - slot->create_ms > FEISHU_CHUNK_EXPIRE_MS) {
            ESP_LOGW(TAG, "Drop expired chunk message_id=%s trace_id=%s",
                     slot->message_id, slot->trace_id);
            chunk_slot_clear(slot);
        }
    }
}

static chunk_cache_t *chunk_cache_find_or_alloc(const char *message_id, int sum, const char *trace_id)
{
    chunk_cache_t *free_slot = NULL;
    for (int i = 0; i < FEISHU_CHUNK_CACHE_MAX; i++) {
        chunk_cache_t *slot = &s_chunk_cache[i];
        if (slot->used && strcmp(slot->message_id, message_id) == 0) {
            return slot;
        }
        if (!slot->used && !free_slot) {
            free_slot = slot;
        }
    }

    if (!free_slot) {
        int oldest = 0;
        for (int i = 1; i < FEISHU_CHUNK_CACHE_MAX; i++) {
            if (s_chunk_cache[i].create_ms < s_chunk_cache[oldest].create_ms) {
                oldest = i;
            }
        }
        chunk_slot_clear(&s_chunk_cache[oldest]);
        free_slot = &s_chunk_cache[oldest];
    }

    free_slot->used = true;
    str_copy(free_slot->message_id, sizeof(free_slot->message_id), message_id);
    str_copy(free_slot->trace_id, sizeof(free_slot->trace_id), trace_id ? trace_id : "");
    free_slot->sum = sum;
    free_slot->got = 0;
    free_slot->create_ms = now_ms();
    return free_slot;
}

static esp_err_t chunk_merge_payload(const char *message_id,
                                     int sum,
                                     int seq,
                                     const char *trace_id,
                                     const uint8_t *part,
                                     size_t part_len,
                                     uint8_t **merged,
                                     size_t *merged_len)
{
    if (!message_id || !part || !merged || !merged_len) {
        return ESP_ERR_INVALID_ARG;
    }
    *merged = NULL;
    *merged_len = 0;

    if (sum <= 1) {
        uint8_t *buf = malloc(part_len);
        if (!buf) return ESP_ERR_NO_MEM;
        memcpy(buf, part, part_len);
        *merged = buf;
        *merged_len = part_len;
        return ESP_OK;
    }

    if (sum > FEISHU_CHUNK_MAX || seq < 0 || seq >= sum) {
        return ESP_ERR_INVALID_ARG;
    }

    chunk_cache_t *slot = chunk_cache_find_or_alloc(message_id, sum, trace_id);
    if (!slot) return ESP_FAIL;

    if (!slot->parts[seq]) {
        slot->parts[seq] = malloc(part_len);
        if (!slot->parts[seq]) {
            return ESP_ERR_NO_MEM;
        }
        memcpy(slot->parts[seq], part, part_len);
        slot->part_lens[seq] = part_len;
        slot->got++;
    }

    if (slot->got < slot->sum) {
        return ESP_ERR_NOT_FINISHED;
    }

    size_t total = 0;
    for (int i = 0; i < slot->sum; i++) {
        if (!slot->parts[i]) {
            return ESP_ERR_NOT_FOUND;
        }
        total += slot->part_lens[i];
    }

    uint8_t *buf = malloc(total);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }

    size_t off = 0;
    for (int i = 0; i < slot->sum; i++) {
        memcpy(buf + off, slot->parts[i], slot->part_lens[i]);
        off += slot->part_lens[i];
    }

    *merged = buf;
    *merged_len = total;
    chunk_slot_clear(slot);
    return ESP_OK;
}

static bool pb_writer_reserve(pb_writer_t *w, size_t need)
{
    if (w->len + need <= w->cap) return true;
    size_t new_cap = w->cap ? w->cap * 2 : 128;
    while (new_cap < w->len + need) {
        new_cap *= 2;
    }
    uint8_t *tmp = realloc(w->buf, new_cap);
    if (!tmp) return false;
    w->buf = tmp;
    w->cap = new_cap;
    return true;
}

static bool pb_writer_put(pb_writer_t *w, const void *data, size_t len)
{
    if (!pb_writer_reserve(w, len)) return false;
    memcpy(w->buf + w->len, data, len);
    w->len += len;
    return true;
}

static bool pb_write_varint(pb_writer_t *w, uint64_t v)
{
    uint8_t tmp[10];
    size_t n = 0;
    do {
        uint8_t b = (uint8_t)(v & 0x7F);
        v >>= 7;
        if (v) b |= 0x80;
        tmp[n++] = b;
    } while (v && n < sizeof(tmp));
    return pb_writer_put(w, tmp, n);
}

static bool pb_write_tag(pb_writer_t *w, uint32_t field, uint32_t wire_type)
{
    uint64_t tag = ((uint64_t)field << 3) | wire_type;
    return pb_write_varint(w, tag);
}

static bool pb_write_string_field(pb_writer_t *w, uint32_t field, const char *str)
{
    if (!pb_write_tag(w, field, 2)) return false;
    size_t len = str ? strlen(str) : 0;
    if (!pb_write_varint(w, len)) return false;
    return len ? pb_writer_put(w, str, len) : true;
}

static bool pb_write_bytes_field(pb_writer_t *w, uint32_t field, const uint8_t *bytes, size_t len)
{
    if (!pb_write_tag(w, field, 2)) return false;
    if (!pb_write_varint(w, len)) return false;
    return len ? pb_writer_put(w, bytes, len) : true;
}

static bool pb_read_varint(const uint8_t *buf, size_t len, size_t *off, uint64_t *out)
{
    uint64_t v = 0;
    int shift = 0;
    while (*off < len && shift < 64) {
        uint8_t b = buf[(*off)++];
        v |= ((uint64_t)(b & 0x7F) << shift);
        if ((b & 0x80) == 0) {
            *out = v;
            return true;
        }
        shift += 7;
    }
    return false;
}

static bool pb_read_len_delim(const uint8_t *buf, size_t len, size_t *off,
                              const uint8_t **data, size_t *data_len)
{
    uint64_t l = 0;
    if (!pb_read_varint(buf, len, off, &l)) return false;
    if (*off + l > len) return false;
    *data = buf + *off;
    *data_len = (size_t)l;
    *off += (size_t)l;
    return true;
}

static bool pb_skip_field(const uint8_t *buf, size_t len, size_t *off, uint32_t wire_type)
{
    uint64_t tmp = 0;
    switch (wire_type) {
        case 0:
            return pb_read_varint(buf, len, off, &tmp);
        case 1:
            if (*off + 8 > len) return false;
            *off += 8;
            return true;
        case 2: {
            const uint8_t *p = NULL;
            size_t l = 0;
            return pb_read_len_delim(buf, len, off, &p, &l);
        }
        case 5:
            if (*off + 4 > len) return false;
            *off += 4;
            return true;
        default:
            return false;
    }
}

static void pb_copy_string(char *dst, size_t dst_size, const uint8_t *src, size_t src_len)
{
    if (dst_size == 0) return;
    if (!src || src_len == 0) {
        dst[0] = '\0';
        return;
    }
    size_t n = src_len;
    if (n >= dst_size) n = dst_size - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static bool pb_decode_header(const uint8_t *buf, size_t len, pb_header_t *header)
{
    memset(header, 0, sizeof(*header));
    size_t off = 0;
    while (off < len) {
        uint64_t tag = 0;
        if (!pb_read_varint(buf, len, &off, &tag)) return false;
        uint32_t field = (uint32_t)(tag >> 3);
        uint32_t wt = (uint32_t)(tag & 0x07);
        if (wt != 2) {
            if (!pb_skip_field(buf, len, &off, wt)) return false;
            continue;
        }
        const uint8_t *p = NULL;
        size_t l = 0;
        if (!pb_read_len_delim(buf, len, &off, &p, &l)) return false;
        if (field == 1) {
            pb_copy_string(header->key, sizeof(header->key), p, l);
        } else if (field == 2) {
            pb_copy_string(header->value, sizeof(header->value), p, l);
        }
    }
    return header->key[0] != '\0';
}

static bool pb_decode_frame(const uint8_t *buf, size_t len, pb_frame_t *frame)
{
    memset(frame, 0, sizeof(*frame));
    bool has_seq = false, has_log = false, has_service = false, has_method = false;
    size_t off = 0;
    while (off < len) {
        uint64_t tag = 0;
        if (!pb_read_varint(buf, len, &off, &tag)) return false;
        uint32_t field = (uint32_t)(tag >> 3);
        uint32_t wt = (uint32_t)(tag & 0x07);

        if (field == 1 || field == 2 || field == 3 || field == 4) {
            if (wt != 0) return false;
            uint64_t v = 0;
            if (!pb_read_varint(buf, len, &off, &v)) return false;
            if (field == 1) {
                frame->seq_id = v;
                has_seq = true;
            } else if (field == 2) {
                frame->log_id = v;
                has_log = true;
            } else if (field == 3) {
                frame->service = (int32_t)v;
                has_service = true;
            } else {
                frame->method = (int32_t)v;
                has_method = true;
            }
            continue;
        }

        if (field == 5) {
            if (wt != 2) return false;
            const uint8_t *sub = NULL;
            size_t sub_len = 0;
            if (!pb_read_len_delim(buf, len, &off, &sub, &sub_len)) return false;
            if (frame->header_count < FEISHU_MAX_HEADERS) {
                pb_decode_header(sub, sub_len, &frame->headers[frame->header_count]);
                frame->header_count++;
            }
            continue;
        }

        if (field == 8) {
            if (wt != 2) return false;
            const uint8_t *p = NULL;
            size_t l = 0;
            if (!pb_read_len_delim(buf, len, &off, &p, &l)) return false;
            if (l > 0) {
                frame->payload = malloc(l);
                if (!frame->payload) return false;
                memcpy(frame->payload, p, l);
            }
            frame->payload_len = l;
            continue;
        }

        if (!pb_skip_field(buf, len, &off, wt)) {
            return false;
        }
    }

    return has_seq && has_log && has_service && has_method;
}

static void pb_frame_free(pb_frame_t *frame)
{
    if (!frame) return;
    free(frame->payload);
    frame->payload = NULL;
    frame->payload_len = 0;
}

static bool pb_encode_frame(const pb_frame_t *frame, uint8_t **out, size_t *out_len)
{
    *out = NULL;
    *out_len = 0;

    pb_writer_t w = {0};
    bool ok = true;

    ok = ok && pb_write_tag(&w, 1, 0) && pb_write_varint(&w, frame->seq_id);
    ok = ok && pb_write_tag(&w, 2, 0) && pb_write_varint(&w, frame->log_id);
    ok = ok && pb_write_tag(&w, 3, 0) && pb_write_varint(&w, (uint64_t)(uint32_t)frame->service);
    ok = ok && pb_write_tag(&w, 4, 0) && pb_write_varint(&w, (uint64_t)(uint32_t)frame->method);

    for (size_t i = 0; ok && i < frame->header_count; i++) {
        pb_writer_t sub = {0};
        ok = ok && pb_write_string_field(&sub, 1, frame->headers[i].key);
        ok = ok && pb_write_string_field(&sub, 2, frame->headers[i].value);
        ok = ok && pb_write_tag(&w, 5, 2);
        ok = ok && pb_write_varint(&w, sub.len);
        ok = ok && pb_writer_put(&w, sub.buf, sub.len);
        free(sub.buf);
    }

    if (ok && frame->payload && frame->payload_len > 0) {
        ok = pb_write_bytes_field(&w, 8, frame->payload, frame->payload_len);
    }

    if (!ok) {
        free(w.buf);
        return false;
    }

    *out = w.buf;
    *out_len = w.len;
    return true;
}

static const char *frame_header_get(const pb_frame_t *frame, const char *key)
{
    if (!frame || !key) return NULL;
    for (size_t i = 0; i < frame->header_count; i++) {
        if (strcmp(frame->headers[i].key, key) == 0) {
            return frame->headers[i].value;
        }
    }
    return NULL;
}

static esp_err_t ws_send_frame(const pb_frame_t *frame)
{
    if (!s_ws_transport) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t *data = NULL;
    size_t data_len = 0;
    if (!pb_encode_frame(frame, &data, &data_len)) {
        return ESP_FAIL;
    }

    int sent = esp_transport_ws_send_raw(s_ws_transport,
                                         WS_TRANSPORT_OPCODES_BINARY,
                                         (const char *)data,
                                         (int)data_len,
                                         5000);
    free(data);
    return (sent < 0) ? ESP_FAIL : ESP_OK;
}

static esp_err_t ws_send_ping(void)
{
    pb_frame_t frame = {0};
    frame.seq_id = 0;
    frame.log_id = 0;
    frame.service = s_ws_service_id;
    frame.method = 0;
    frame.header_count = 1;
    str_copy(frame.headers[0].key, sizeof(frame.headers[0].key), "type");
    str_copy(frame.headers[0].value, sizeof(frame.headers[0].value), "ping");
    return ws_send_frame(&frame);
}

static esp_err_t ws_send_event_ack(const pb_frame_t *req, int code)
{
    if (!req) return ESP_ERR_INVALID_ARG;

    cJSON *ack = cJSON_CreateObject();
    cJSON_AddNumberToObject(ack, "code", code);
    char *ack_json = cJSON_PrintUnformatted(ack);
    cJSON_Delete(ack);
    if (!ack_json) {
        return ESP_ERR_NO_MEM;
    }

    pb_frame_t resp = {0};
    resp.seq_id = req->seq_id;
    resp.log_id = req->log_id;
    resp.service = req->service;
    resp.method = req->method;
    resp.header_count = req->header_count;
    if (resp.header_count > FEISHU_MAX_HEADERS) {
        resp.header_count = FEISHU_MAX_HEADERS;
    }
    for (size_t i = 0; i < resp.header_count; i++) {
        str_copy(resp.headers[i].key, sizeof(resp.headers[i].key), req->headers[i].key);
        str_copy(resp.headers[i].value, sizeof(resp.headers[i].value), req->headers[i].value);
    }
    resp.payload = (uint8_t *)ack_json;
    resp.payload_len = strlen(ack_json);

    esp_err_t err = ws_send_frame(&resp);
    free(ack_json);
    return err;
}

static esp_err_t feishu_send_via_webhook(const char *text)
{
    if (!text || !text[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_webhook_url[0]) {
        return ESP_ERR_INVALID_STATE;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *content = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "msg_type", "text");
    cJSON_AddStringToObject(content, "text", text);
    cJSON_AddItemToObject(root, "content", content);

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) return ESP_ERR_NO_MEM;

    char *resp = NULL;
    int status = 0;
    esp_err_t err = http_json_request(s_webhook_url, HTTP_METHOD_POST, NULL, body,
                                      FEISHU_HTTP_TIMEOUT_MS, &resp, &status);
    free(body);
    if (err != ESP_OK) return err;

    if (status < 200 || status >= 300) {
        ESP_LOGE(TAG, "Webhook HTTP %d: %.200s", status, resp ? resp : "");
        free(resp);
        return ESP_FAIL;
    }
    free(resp);
    return ESP_OK;
}

static esp_err_t feishu_get_tenant_token(char *out, size_t out_size)
{
    if (!out || out_size == 0) return ESP_ERR_INVALID_ARG;
    out[0] = '\0';

    if (!s_app_id[0] || !s_app_secret[0]) {
        return ESP_ERR_INVALID_STATE;
    }

    int64_t t = now_ms();
    if (s_tenant_token[0] && t + FEISHU_TOKEN_SKEW_MS < s_tenant_token_expire_ms) {
        str_copy(out, out_size, s_tenant_token);
        return ESP_OK;
    }

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "app_id", s_app_id);
    cJSON_AddStringToObject(req, "app_secret", s_app_secret);
    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!body) {
        return ESP_ERR_NO_MEM;
    }

    char *resp = NULL;
    int status = 0;
    esp_err_t err = http_json_request(FEISHU_AUTH_URL, HTTP_METHOD_POST, NULL, body,
                                      FEISHU_HTTP_TIMEOUT_MS, &resp, &status);
    free(body);
    if (err != ESP_OK) {
        return err;
    }
    if (status != 200) {
        ESP_LOGE(TAG, "tenant_access_token HTTP %d: %.200s", status, resp ? resp : "");
        free(resp);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(resp);
    free(resp);
    if (!root) {
        return ESP_FAIL;
    }

    cJSON *code = cJSON_GetObjectItem(root, "code");
    cJSON *token = cJSON_GetObjectItem(root, "tenant_access_token");
    cJSON *expire = cJSON_GetObjectItem(root, "expire");
    if (!cJSON_IsNumber(code) || code->valueint != 0 ||
        !cJSON_IsString(token) || !token->valuestring) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    int expire_s = cJSON_IsNumber(expire) ? expire->valueint : 7200;
    if (expire_s < 300) expire_s = 300;

    str_copy(s_tenant_token, sizeof(s_tenant_token), token->valuestring);
    s_tenant_token_expire_ms = now_ms() + (int64_t)expire_s * 1000;
    str_copy(out, out_size, s_tenant_token);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t feishu_send_via_im(const char *chat_id, const char *text)
{
    if (!chat_id || !chat_id[0] || !text || !text[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_app_id[0] || !s_app_secret[0]) {
        return ESP_ERR_INVALID_STATE;
    }

    cJSON *content = cJSON_CreateObject();
    cJSON_AddStringToObject(content, "text", text);
    char *content_str = cJSON_PrintUnformatted(content);
    cJSON_Delete(content);
    if (!content_str) {
        return ESP_ERR_NO_MEM;
    }

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "receive_id", chat_id);
    cJSON_AddStringToObject(req, "msg_type", "text");
    cJSON_AddStringToObject(req, "content", content_str);
    free(content_str);

    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!body) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < 2; attempt++) {
        char token[640];
        err = feishu_get_tenant_token(token, sizeof(token));
        if (err != ESP_OK) {
            break;
        }

        char *resp = NULL;
        int status = 0;
        err = http_json_request(FEISHU_SEND_MSG_URL, HTTP_METHOD_POST, token, body,
                                FEISHU_HTTP_TIMEOUT_MS, &resp, &status);
        if (err != ESP_OK) {
            free(resp);
            break;
        }

        if (status == 401 && attempt == 0) {
            s_tenant_token[0] = '\0';
            s_tenant_token_expire_ms = 0;
            free(resp);
            continue;
        }

        if (status != 200) {
            ESP_LOGE(TAG, "im/v1/messages HTTP %d chat=%s resp=%.200s", status, chat_id, resp ? resp : "");
            free(resp);
            err = ESP_FAIL;
            break;
        }

        cJSON *root = cJSON_Parse(resp);
        free(resp);
        if (!root) {
            err = ESP_FAIL;
            break;
        }
        cJSON *code = cJSON_GetObjectItem(root, "code");
        if (!cJSON_IsNumber(code) || code->valueint != 0) {
            cJSON *msg = cJSON_GetObjectItem(root, "msg");
            ESP_LOGE(TAG, "im/v1/messages failed code=%d msg=%s",
                     cJSON_IsNumber(code) ? code->valueint : -1,
                     cJSON_IsString(msg) ? msg->valuestring : "");
            cJSON_Delete(root);
            err = ESP_FAIL;
            break;
        }
        cJSON_Delete(root);
        err = ESP_OK;
        break;
    }
    free(body);
    return err;
}

static esp_err_t feishu_pull_ws_connect_config(char *url_out, size_t url_out_size)
{
    if (!url_out || url_out_size == 0) return ESP_ERR_INVALID_ARG;

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "AppID", s_app_id);
    cJSON_AddStringToObject(req, "AppSecret", s_app_secret);
    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!body) {
        return ESP_ERR_NO_MEM;
    }

    char *resp = NULL;
    int status = 0;
    esp_err_t err = http_json_request(FEISHU_WS_ENDPOINT_URL, HTTP_METHOD_POST, NULL, body,
                                      FEISHU_HTTP_TIMEOUT_MS, &resp, &status);
    free(body);
    if (err != ESP_OK) return err;
    if (status != 200) {
        ESP_LOGE(TAG, "ws endpoint HTTP %d: %.200s", status, resp ? resp : "");
        free(resp);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(resp);
    free(resp);
    if (!root) return ESP_FAIL;

    cJSON *code = cJSON_GetObjectItem(root, "code");
    cJSON *data = cJSON_GetObjectItem(root, "data");
    cJSON *url = data ? cJSON_GetObjectItem(data, "URL") : NULL;
    cJSON *client_cfg = data ? cJSON_GetObjectItem(data, "ClientConfig") : NULL;

    if (!cJSON_IsNumber(code) || code->valueint != 0 || !cJSON_IsString(url) || !url->valuestring) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    str_copy(url_out, url_out_size, url->valuestring);
    s_ws_service_id = parse_url_query_int(url_out, "service_id", 0);
    if (s_ws_service_id <= 0) {
        ESP_LOGW(TAG, "service_id parse failed from URL");
    }

    if (client_cfg) {
        cJSON *ping = cJSON_GetObjectItem(client_cfg, "PingInterval");
        cJSON *r_count = cJSON_GetObjectItem(client_cfg, "ReconnectCount");
        cJSON *r_interval = cJSON_GetObjectItem(client_cfg, "ReconnectInterval");
        cJSON *r_nonce = cJSON_GetObjectItem(client_cfg, "ReconnectNonce");
        if (cJSON_IsNumber(ping)) s_ws_ping_interval_ms = ping->valueint * 1000;
        if (cJSON_IsNumber(r_count)) s_ws_reconnect_count = r_count->valueint;
        if (cJSON_IsNumber(r_interval)) s_ws_reconnect_interval_ms = r_interval->valueint * 1000;
        if (cJSON_IsNumber(r_nonce)) s_ws_reconnect_nonce_ms = r_nonce->valueint * 1000;
    }

    if (s_ws_ping_interval_ms <= 0) s_ws_ping_interval_ms = 120000;
    if (s_ws_reconnect_interval_ms <= 0) s_ws_reconnect_interval_ms = 120000;
    if (s_ws_reconnect_nonce_ms < 0) s_ws_reconnect_nonce_ms = 0;

    cJSON_Delete(root);
    return ESP_OK;
}

static const char *json_obj_get_str(cJSON *obj, const char *key)
{
    if (!obj) return NULL;
    cJSON *it = cJSON_GetObjectItem(obj, key);
    return cJSON_IsString(it) ? it->valuestring : NULL;
}

static esp_err_t feishu_event_to_bus(cJSON *root)
{
    cJSON *header = cJSON_GetObjectItem(root, "header");
    const char *event_type = json_obj_get_str(header, "event_type");
    if (!event_type || strcmp(event_type, FEISHU_EVENT_IM_RECEIVE) != 0) {
        return ESP_OK;
    }

    cJSON *event = cJSON_GetObjectItem(root, "event");
    cJSON *message = event ? cJSON_GetObjectItem(event, "message") : NULL;
    cJSON *sender = event ? cJSON_GetObjectItem(event, "sender") : NULL;
    cJSON *sender_id = sender ? cJSON_GetObjectItem(sender, "sender_id") : NULL;
    const char *message_type = json_obj_get_str(message, "message_type");
    const char *chat_id = json_obj_get_str(message, "chat_id");
    const char *message_id = json_obj_get_str(message, "message_id");
    const char *create_time = json_obj_get_str(message, "create_time");
    const char *sender_open_id = json_obj_get_str(sender_id, "open_id");
    const char *sender_user_id = json_obj_get_str(sender_id, "user_id");
    const char *sender_union_id = json_obj_get_str(sender_id, "union_id");
    const char *sender_key = (sender_open_id && sender_open_id[0]) ? sender_open_id :
                             (sender_user_id && sender_user_id[0]) ? sender_user_id :
                             (sender_union_id && sender_union_id[0]) ? sender_union_id : "";
    const char *raw_content = json_obj_get_str(message, "content");
    const char *event_id = json_obj_get_str(header, "event_id");
    const char *dedup_key = (message_id && message_id[0]) ? message_id : event_id;
    char semantic_key[96];
    uint32_t content_hash = fnv1a32_str(raw_content ? raw_content : "");
    snprintf(semantic_key, sizeof(semantic_key), "c:%s|s:%s|t:%s|h:%08lx",
             chat_id ? chat_id : "", sender_key, create_time ? create_time : "",
             (unsigned long)content_hash);

    if (event_dedup_contains(dedup_key) || event_dedup_contains(semantic_key)) {
        ESP_LOGI(TAG, "Drop duplicate Feishu event id=%s", dedup_key ? dedup_key : "");
        return ESP_OK;
    }

    if (!chat_id || !chat_id[0]) {
        ESP_LOGW(TAG, "Feishu event has no chat_id");
        return ESP_FAIL;
    }

    char *owned_text = NULL;

    if (message_type && strcmp(message_type, FEISHU_MSG_TYPE_TEXT) == 0) {
        const char *content = json_obj_get_str(message, "content");
        if (content && content[0]) {
            cJSON *content_json = cJSON_Parse(content);
            if (content_json) {
                const char *text = json_obj_get_str(content_json, "text");
                if (!text) {
                    cJSON *zh = cJSON_GetObjectItem(content_json, "zh_cn");
                    text = zh ? json_obj_get_str(zh, "text") : NULL;
                }
                owned_text = text ? strdup(text) : NULL;
                cJSON_Delete(content_json);
            } else {
                owned_text = strdup(content);
            }
        }
    } else {
        owned_text = strdup("[non-text message]");
    }

    if (!owned_text || !owned_text[0]) {
        free(owned_text);
        return ESP_OK;
    }

    mimi_msg_t msg = {0};
    str_copy(msg.channel, sizeof(msg.channel), MIMI_CHAN_FEISHU);
    str_copy(msg.chat_id, sizeof(msg.chat_id), chat_id);
    msg.content = owned_text;

    esp_err_t err = message_bus_push_inbound(&msg);
    if (err != ESP_OK) {
        free(owned_text);
        ESP_LOGW(TAG, "Feishu inbound queue full");
        return err;
    }

    event_dedup_mark_seen(dedup_key);
    event_dedup_mark_seen(semantic_key);
    ESP_LOGI(TAG, "Feishu inbound event -> bus chat=%s text=%.48s", msg.chat_id, msg.content);
    return ESP_OK;
}

static void feishu_ws_handle_control_frame(const pb_frame_t *frame)
{
    const char *type = frame_header_get(frame, "type");
    if (!type) return;

    if (strcmp(type, "pong") == 0 && frame->payload && frame->payload_len > 0) {
        char *payload = calloc(1, frame->payload_len + 1);
        if (!payload) return;
        memcpy(payload, frame->payload, frame->payload_len);

        cJSON *root = cJSON_Parse(payload);
        free(payload);
        if (!root) return;

        cJSON *ping = cJSON_GetObjectItem(root, "PingInterval");
        cJSON *r_count = cJSON_GetObjectItem(root, "ReconnectCount");
        cJSON *r_interval = cJSON_GetObjectItem(root, "ReconnectInterval");
        cJSON *r_nonce = cJSON_GetObjectItem(root, "ReconnectNonce");
        if (cJSON_IsNumber(ping) && ping->valueint > 0) s_ws_ping_interval_ms = ping->valueint * 1000;
        if (cJSON_IsNumber(r_count)) s_ws_reconnect_count = r_count->valueint;
        if (cJSON_IsNumber(r_interval) && r_interval->valueint > 0) s_ws_reconnect_interval_ms = r_interval->valueint * 1000;
        if (cJSON_IsNumber(r_nonce) && r_nonce->valueint >= 0) s_ws_reconnect_nonce_ms = r_nonce->valueint * 1000;

        cJSON_Delete(root);
        return;
    }

    const char *hs = frame_header_get(frame, "handshake-status");
    const char *hs_msg = frame_header_get(frame, "handshake-msg");
    if (hs && strcmp(hs, "0") != 0) {
        ESP_LOGW(TAG, "Feishu handshake status=%s msg=%s", hs, hs_msg ? hs_msg : "");
    }
}

static void feishu_ws_handle_data_frame(const pb_frame_t *frame)
{
    const char *type = frame_header_get(frame, "type");
    if (!type || strcmp(type, "event") != 0) {
        return;
    }

    const char *message_id = frame_header_get(frame, "message_id");
    const char *sum_s = frame_header_get(frame, "sum");
    const char *seq_s = frame_header_get(frame, "seq");
    const char *trace_id = frame_header_get(frame, "trace_id");
    int sum = sum_s ? atoi(sum_s) : 1;
    int seq = seq_s ? atoi(seq_s) : 0;

    if (!message_id || !frame->payload) {
        ws_send_event_ack(frame, 500);
        return;
    }

    uint8_t *merged = NULL;
    size_t merged_len = 0;
    esp_err_t merge_err = chunk_merge_payload(message_id, sum, seq, trace_id,
                                              frame->payload, frame->payload_len,
                                              &merged, &merged_len);
    if (merge_err == ESP_ERR_NOT_FINISHED) {
        return;
    }
    if (merge_err != ESP_OK || !merged || merged_len == 0) {
        ESP_LOGW(TAG, "Feishu chunk merge failed message_id=%s", message_id);
        ws_send_event_ack(frame, 500);
        free(merged);
        return;
    }

    char *json = calloc(1, merged_len + 1);
    if (!json) {
        free(merged);
        ws_send_event_ack(frame, 500);
        return;
    }
    memcpy(json, merged, merged_len);
    free(merged);

    cJSON *root = cJSON_Parse(json);
    free(json);
    if (!root) {
        ws_send_event_ack(frame, 500);
        return;
    }

    esp_err_t bus_err = feishu_event_to_bus(root);
    cJSON_Delete(root);
    ws_send_event_ack(frame, bus_err == ESP_OK ? 200 : 500);
}

static void feishu_ws_process_binary(const uint8_t *data, size_t len)
{
    pb_frame_t *frame = calloc(1, sizeof(pb_frame_t));
    if (!frame) {
        ESP_LOGW(TAG, "No memory for Feishu protobuf frame");
        return;
    }

    if (!pb_decode_frame(data, len, frame)) {
        ESP_LOGW(TAG, "Failed to decode Feishu protobuf frame");
        free(frame);
        return;
    }

    if (frame->method == 0) {
        feishu_ws_handle_control_frame(frame);
    } else if (frame->method == 1) {
        feishu_ws_handle_data_frame(frame);
    }
    pb_frame_free(frame);
    free(frame);
}

static void feishu_ws_rx_reset(void)
{
    free(s_ws_rx_buf);
    s_ws_rx_buf = NULL;
    s_ws_rx_expected = 0;
    s_ws_rx_received = 0;
}

static void feishu_ws_handle_read_chunk(const uint8_t *chunk, size_t chunk_len, int payload_len)
{
    if (!chunk || chunk_len == 0) {
        return;
    }

    if (s_ws_rx_expected == 0) {
        size_t expected = (payload_len > 0) ? (size_t)payload_len : chunk_len;
        if (expected < chunk_len) expected = chunk_len;
        if (expected > FEISHU_WS_PAYLOAD_MAX) {
            ESP_LOGW(TAG, "Drop too large ws payload: %u", (unsigned)expected);
            return;
        }
        s_ws_rx_buf = malloc(expected);
        if (!s_ws_rx_buf) {
            return;
        }
        s_ws_rx_expected = expected;
        s_ws_rx_received = 0;
    }

    if (!s_ws_rx_buf || s_ws_rx_received + chunk_len > s_ws_rx_expected) {
        feishu_ws_rx_reset();
        return;
    }

    memcpy(s_ws_rx_buf + s_ws_rx_received, chunk, chunk_len);
    s_ws_rx_received += chunk_len;

    if (s_ws_rx_received >= s_ws_rx_expected) {
        feishu_ws_process_binary(s_ws_rx_buf, s_ws_rx_expected);
        feishu_ws_rx_reset();
    }
}

static bool ws_parse_url(const char *url, bool *use_ssl,
                         char *host, size_t host_size, int *port,
                         char *path, size_t path_size)
{
    if (!url || !use_ssl || !host || !port || !path) {
        return false;
    }

    const char *p = NULL;
    if (strncmp(url, "wss://", 6) == 0) {
        *use_ssl = true;
        *port = 443;
        p = url + 6;
    } else if (strncmp(url, "ws://", 5) == 0) {
        *use_ssl = false;
        *port = 80;
        p = url + 5;
    } else {
        return false;
    }

    const char *slash = strchr(p, '/');
    const char *host_end = slash ? slash : (p + strlen(p));
    size_t hostport_len = (size_t)(host_end - p);
    if (hostport_len == 0 || hostport_len >= 128) {
        return false;
    }

    char hostport[128];
    memcpy(hostport, p, hostport_len);
    hostport[hostport_len] = '\0';

    char *colon = strrchr(hostport, ':');
    if (colon) {
        *colon = '\0';
        int parsed_port = atoi(colon + 1);
        if (parsed_port > 0 && parsed_port <= 65535) {
            *port = parsed_port;
        }
    }

    str_copy(host, host_size, hostport);
    if (host[0] == '\0') {
        return false;
    }

    if (slash) {
        str_copy(path, path_size, slash);
    } else {
        str_copy(path, path_size, "/");
    }

    return true;
}

static void ws_client_cleanup(void)
{
    feishu_ws_rx_reset();
    if (s_ws_transport) {
        esp_transport_close(s_ws_transport);
        esp_transport_destroy(s_ws_transport);
        s_ws_transport = NULL;
    }
}

static bool ws_client_connect(const char *url)
{
    bool use_ssl = true;
    int port = 443;
    char host[128] = {0};
    char path[384] = {0};

    if (!ws_parse_url(url, &use_ssl, host, sizeof(host), &port, path, sizeof(path))) {
        ESP_LOGE(TAG, "Invalid ws url: %s", url);
        return false;
    }

    esp_transport_handle_t base = use_ssl ? esp_transport_ssl_init() : esp_transport_tcp_init();
    if (!base) {
        return false;
    }
    if (use_ssl) {
        esp_transport_ssl_crt_bundle_attach(base, esp_crt_bundle_attach);
    }

    esp_transport_handle_t ws = esp_transport_ws_init(base);
    if (!ws) {
        esp_transport_destroy(base);
        return false;
    }
    esp_transport_ws_set_path(ws, path);

    int ret = esp_transport_connect(ws, host, port, FEISHU_HTTP_TIMEOUT_MS);
    if (ret < 0) {
        esp_transport_destroy(ws);
        return false;
    }

    s_ws_transport = ws;
    ESP_LOGI(TAG, "Feishu WS connected host=%s port=%d", host, port);
    return true;
}

static void feishu_longconn_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Feishu long connection task started");

    int reconnect_tries = 0;
    while (s_longconn_should_run) {
        if (!feishu_bot_has_app_credentials()) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        char ws_url[FEISHU_URL_MAX];
        esp_err_t cfg_err = feishu_pull_ws_connect_config(ws_url, sizeof(ws_url));
        if (cfg_err != ESP_OK) {
            ESP_LOGW(TAG, "Pull ws config failed: %s", esp_err_to_name(cfg_err));
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        if (!ws_client_connect(ws_url)) {
            reconnect_tries++;
            if (s_ws_reconnect_count >= 0 && reconnect_tries >= s_ws_reconnect_count) {
                ESP_LOGE(TAG, "Feishu WS reconnect reached max count=%d", s_ws_reconnect_count);
                break;
            }
            int delay_ms = s_ws_reconnect_interval_ms;
            if (s_ws_reconnect_nonce_ms > 0) {
                delay_ms += (int)(esp_random() % (uint32_t)s_ws_reconnect_nonce_ms);
            }
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
            continue;
        }

        reconnect_tries = 0;
        int64_t next_ping = now_ms() + 1000;
        uint32_t loop = 0;

        while (s_longconn_should_run && s_ws_transport) {
            int r = esp_transport_read(s_ws_transport, (char *)s_ws_read_buf, sizeof(s_ws_read_buf), 200);
            if (r < 0) {
                ESP_LOGW(TAG, "Feishu WS read error, reconnecting");
                break;
            }
            if (r > 0) {
                ws_transport_opcodes_t op = esp_transport_ws_get_read_opcode(s_ws_transport);
                if (op == WS_TRANSPORT_OPCODES_BINARY || op == WS_TRANSPORT_OPCODES_CONT) {
                    int payload_len = esp_transport_ws_get_read_payload_len(s_ws_transport);
                    feishu_ws_handle_read_chunk(s_ws_read_buf, (size_t)r, payload_len);
                }
            }

            int64_t t = now_ms();
            if (t >= next_ping) {
                if (ws_send_ping() != ESP_OK) {
                    ESP_LOGW(TAG, "Feishu ping send failed");
                    break;
                }
                next_ping = t + s_ws_ping_interval_ms;
            }
            if ((loop++ % 10) == 0) {
                chunk_cache_clear_expired();
                event_dedup_maybe_flush(false);
            }
        }

        ws_client_cleanup();
        if (!s_longconn_should_run) {
            break;
        }
        int delay_ms = s_ws_reconnect_interval_ms;
        if (s_ws_reconnect_nonce_ms > 0) {
            delay_ms += (int)(esp_random() % (uint32_t)s_ws_reconnect_nonce_ms);
        }
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }

    ws_client_cleanup();
    s_longconn_task = NULL;
    ESP_LOGW(TAG, "Feishu long connection task stopped");
    vTaskDelete(NULL);
}

esp_err_t feishu_bot_init(void)
{
    char tmp[320];

    if (nvs_get_string(MIMI_NVS_FEISHU, MIMI_NVS_KEY_FEISHU_WEBHOOK, tmp, sizeof(tmp)) == ESP_OK && tmp[0]) {
        str_copy(s_webhook_url, sizeof(s_webhook_url), tmp);
    }
    if (nvs_get_string(MIMI_NVS_FEISHU, MIMI_NVS_KEY_FEISHU_APP_ID, tmp, sizeof(tmp)) == ESP_OK && tmp[0]) {
        str_copy(s_app_id, sizeof(s_app_id), tmp);
    }
    if (nvs_get_string(MIMI_NVS_FEISHU, MIMI_NVS_KEY_FEISHU_APP_SECRET, tmp, sizeof(tmp)) == ESP_OK && tmp[0]) {
        str_copy(s_app_secret, sizeof(s_app_secret), tmp);
    }
    if (nvs_get_string(MIMI_NVS_FEISHU, MIMI_NVS_KEY_FEISHU_DEF_CHAT, tmp, sizeof(tmp)) == ESP_OK && tmp[0]) {
        str_copy(s_default_chat_id, sizeof(s_default_chat_id), tmp);
    }

    s_tenant_token[0] = '\0';
    s_tenant_token_expire_ms = 0;

    memset(s_chunk_cache, 0, sizeof(s_chunk_cache));
    if (!s_event_dedup) {
        s_event_dedup = heap_caps_calloc(FEISHU_EVENT_DEDUP_MAX, sizeof(event_dedup_t), MALLOC_CAP_SPIRAM);
        if (!s_event_dedup) {
            s_event_dedup = heap_caps_calloc(FEISHU_EVENT_DEDUP_MAX, sizeof(event_dedup_t), MALLOC_CAP_8BIT);
        }
        if (!s_event_dedup) {
            ESP_LOGW(TAG, "Failed to allocate dedup cache (%d), duplicate filtering disabled",
                     FEISHU_EVENT_DEDUP_MAX);
        }
    } else {
        memset(s_event_dedup, 0, FEISHU_EVENT_DEDUP_MAX * sizeof(event_dedup_t));
    }
    s_event_dedup_dirty = false;
    s_event_dedup_dirty_writes = 0;
    s_event_dedup_last_flush_ms = now_ms();
    if (s_event_dedup) {
        esp_err_t load_err = event_dedup_load_from_disk();
        if (load_err == ESP_OK) {
            ESP_LOGI(TAG, "Dedup cache loaded from disk (%d entries)", FEISHU_EVENT_DEDUP_MAX);
        } else if (load_err != ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "Dedup cache load failed: %s", esp_err_to_name(load_err));
        }
        event_dedup_maybe_flush(false);
    }

    ESP_LOGI(TAG, "Feishu init: webhook=%s app=%s default_chat=%s",
             s_webhook_url[0] ? "yes" : "no",
             (s_app_id[0] && s_app_secret[0]) ? "yes" : "no",
             s_default_chat_id[0] ? "yes" : "no");
    return ESP_OK;
}

esp_err_t feishu_bot_start(void)
{
    if (!s_app_id[0] || !s_app_secret[0]) {
        ESP_LOGW(TAG, "Feishu app credentials not configured, skip long connection");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_longconn_task) {
        return ESP_OK;
    }
    s_longconn_should_run = true;

    BaseType_t ok = xTaskCreatePinnedToCore(
        feishu_longconn_task,
        "feishu_ws",
        MIMI_FEISHU_WS_STACK,
        NULL,
        MIMI_FEISHU_WS_PRIO,
        &s_longconn_task,
        MIMI_FEISHU_WS_CORE);
    return ok == pdPASS ? ESP_OK : ESP_FAIL;
}

esp_err_t feishu_bot_stop(void)
{
    event_dedup_maybe_flush(true);
    s_longconn_should_run = false;
    if (s_ws_transport) {
        esp_transport_close(s_ws_transport);
    }
    for (int i = 0; i < 20; i++) {
        if (!s_longconn_task) break;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (!s_longconn_task) {
        ws_client_cleanup();
    }
    return ESP_OK;
}

esp_err_t feishu_bot_set_webhook(const char *webhook_url)
{
    if (!webhook_url) return ESP_ERR_INVALID_ARG;
    esp_err_t err = nvs_set_string(MIMI_NVS_FEISHU, MIMI_NVS_KEY_FEISHU_WEBHOOK, webhook_url);
    if (err != ESP_OK) return err;
    str_copy(s_webhook_url, sizeof(s_webhook_url), webhook_url);
    return ESP_OK;
}

esp_err_t feishu_bot_clear_webhook(void)
{
    esp_err_t err = nvs_erase_string(MIMI_NVS_FEISHU, MIMI_NVS_KEY_FEISHU_WEBHOOK);
    if (err != ESP_OK) return err;
    s_webhook_url[0] = '\0';
    return ESP_OK;
}

const char *feishu_bot_get_webhook(void)
{
    return s_webhook_url;
}

esp_err_t feishu_bot_set_app_credentials(const char *app_id, const char *app_secret)
{
    if (!app_id || !app_secret || !app_id[0] || !app_secret[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    bool restart = (s_longconn_task != NULL);
    if (restart) {
        feishu_bot_stop();
    }

    esp_err_t err = nvs_set_string(MIMI_NVS_FEISHU, MIMI_NVS_KEY_FEISHU_APP_ID, app_id);
    if (err != ESP_OK) return err;
    err = nvs_set_string(MIMI_NVS_FEISHU, MIMI_NVS_KEY_FEISHU_APP_SECRET, app_secret);
    if (err != ESP_OK) return err;

    str_copy(s_app_id, sizeof(s_app_id), app_id);
    str_copy(s_app_secret, sizeof(s_app_secret), app_secret);
    s_tenant_token[0] = '\0';
    s_tenant_token_expire_ms = 0;

    if (restart) {
        feishu_bot_start();
    }
    return ESP_OK;
}

esp_err_t feishu_bot_clear_app_credentials(void)
{
    feishu_bot_stop();

    esp_err_t err = nvs_erase_string(MIMI_NVS_FEISHU, MIMI_NVS_KEY_FEISHU_APP_ID);
    if (err != ESP_OK) return err;
    err = nvs_erase_string(MIMI_NVS_FEISHU, MIMI_NVS_KEY_FEISHU_APP_SECRET);
    if (err != ESP_OK) return err;

    s_app_id[0] = '\0';
    s_app_secret[0] = '\0';
    s_tenant_token[0] = '\0';
    s_tenant_token_expire_ms = 0;
    return ESP_OK;
}

bool feishu_bot_has_app_credentials(void)
{
    return s_app_id[0] && s_app_secret[0];
}

const char *feishu_bot_get_app_id(void)
{
    return s_app_id;
}

esp_err_t feishu_bot_set_default_chat_id(const char *chat_id)
{
    if (!chat_id) return ESP_ERR_INVALID_ARG;
    esp_err_t err = nvs_set_string(MIMI_NVS_FEISHU, MIMI_NVS_KEY_FEISHU_DEF_CHAT, chat_id);
    if (err != ESP_OK) return err;
    str_copy(s_default_chat_id, sizeof(s_default_chat_id), chat_id);
    return ESP_OK;
}

esp_err_t feishu_bot_clear_default_chat_id(void)
{
    esp_err_t err = nvs_erase_string(MIMI_NVS_FEISHU, MIMI_NVS_KEY_FEISHU_DEF_CHAT);
    if (err != ESP_OK) return err;
    s_default_chat_id[0] = '\0';
    return ESP_OK;
}

const char *feishu_bot_get_default_chat_id(void)
{
    return s_default_chat_id;
}

bool feishu_bot_is_configured(void)
{
    return (s_app_id[0] && s_app_secret[0]) || s_webhook_url[0];
}

esp_err_t feishu_bot_send_message_to(const char *chat_id, const char *text)
{
    if (!text || !text[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_app_id[0] && s_app_secret[0]) {
        const char *target = (chat_id && chat_id[0]) ? chat_id : s_default_chat_id;
        if (!target || !target[0]) {
            ESP_LOGW(TAG, "No chat_id provided for im/v1/messages");
            return ESP_ERR_INVALID_ARG;
        }
        return feishu_send_via_im(target, text);
    }

    if (s_webhook_url[0]) {
        return feishu_send_via_webhook(text);
    }

    return ESP_ERR_INVALID_STATE;
}

esp_err_t feishu_bot_send_message(const char *text)
{
    return feishu_bot_send_message_to(s_default_chat_id, text);
}
