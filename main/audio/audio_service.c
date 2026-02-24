#include "audio/audio_service.h"

#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "driver/i2s.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"

static const char *TAG = "audio_service";

#define AUDIO_SD_MOUNT_POINT     "/sdcard"
#define AUDIO_SD_DIR             "/sdcard/audio"
#define AUDIO_SD_HOST            SPI3_HOST
#define AUDIO_SD_MOSI            4
#define AUDIO_SD_MISO            0
#define AUDIO_SD_SCK             3
#define AUDIO_SD_CS              8

#define AUDIO_I2S_PORT           I2S_NUM_0
#define AUDIO_I2S_BCLK           16
#define AUDIO_I2S_WS             7
#define AUDIO_I2S_DOUT           6
#define AUDIO_I2S_DIN            15

#define AUDIO_REC_SAMPLE_RATE    16000
#define AUDIO_REC_CHANNELS       2
#define AUDIO_REC_BITS           32
/* Software gain for recorded PCM. Increase if volume is still too low. */
#define AUDIO_REC_GAIN_NUM       8
#define AUDIO_REC_GAIN_DEN       1

#define AUDIO_IO_CHUNK_BYTES     1024

typedef struct {
    uint32_t sample_rate;
    uint16_t bits_per_sample;
    uint16_t channels;
    uint32_t data_size;
} wav_info_t;

static SemaphoreHandle_t s_lock = NULL;
static bool s_inited = false;
static bool s_sd_mounted = false;
static bool s_i2s_inited = false;
static sdmmc_card_t *s_card = NULL;

static bool s_recording = false;
static bool s_record_stop = false;
static TaskHandle_t s_record_task = NULL;
static FILE *s_record_fp = NULL;
static size_t s_record_data_bytes = 0;
static char s_record_path[128];

static bool s_playing = false;
static bool s_play_stop = false;
static TaskHandle_t s_play_task = NULL;
static uint64_t s_rec_samples_total = 0;
static int s_play_vol_percent = 25;

static int32_t apply_rec_gain_clip(int32_t s)
{
    int64_t v = (int64_t)s * AUDIO_REC_GAIN_NUM / AUDIO_REC_GAIN_DEN;
    if (v > INT32_MAX) return INT32_MAX;
    if (v < INT32_MIN) return INT32_MIN;
    return (int32_t)v;
}

static int32_t apply_play_vol_clip32(int32_t s, int vol_percent)
{
    int64_t v = (int64_t)s * vol_percent / 100;
    if (v > INT32_MAX) return INT32_MAX;
    if (v < INT32_MIN) return INT32_MIN;
    return (int32_t)v;
}

static int16_t apply_play_vol_clip16(int16_t s, int vol_percent)
{
    int32_t v = (int32_t)s * vol_percent / 100;
    if (v > INT16_MAX) return INT16_MAX;
    if (v < INT16_MIN) return INT16_MIN;
    return (int16_t)v;
}

static inline uint32_t le32_read(const uint8_t *p)
{
    return ((uint32_t)p[0]) |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static inline uint16_t le16_read(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static inline void le32_write(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static inline void le16_write(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static void lock_take(void)
{
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void lock_give(void)
{
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}

static bool is_wav_name(const char *name)
{
    size_t n = strlen(name);
    if (n < 5) return false;
    const char *ext = name + n - 4;
    return (tolower((int)ext[0]) == '.') &&
           (tolower((int)ext[1]) == 'w') &&
           (tolower((int)ext[2]) == 'a') &&
           (tolower((int)ext[3]) == 'v');
}

static int wav_name_cmp(const void *a, const void *b)
{
    const char *sa = (const char *)a;
    const char *sb = (const char *)b;
    return strcmp(sa, sb);
}

static void collect_wav_from_dir(const char *dir_path,
                                 const char *name_prefix,
                                 char names[][AUDIO_SERVICE_MAX_NAME_LEN],
                                 size_t max_files,
                                 size_t *count)
{
    DIR *dir = opendir(dir_path);
    if (!dir) return;

    struct dirent *ent = NULL;
    while ((ent = readdir(dir)) != NULL && *count < max_files) {
        if (!is_wav_name(ent->d_name)) continue;

        if (name_prefix && name_prefix[0] != '\0') {
            int n = snprintf(names[*count], AUDIO_SERVICE_MAX_NAME_LEN, "%s%s",
                             name_prefix, ent->d_name);
            if (n <= 0 || n >= AUDIO_SERVICE_MAX_NAME_LEN) {
                continue;
            }
        } else {
            strncpy(names[*count], ent->d_name, AUDIO_SERVICE_MAX_NAME_LEN - 1);
            names[*count][AUDIO_SERVICE_MAX_NAME_LEN - 1] = '\0';
        }
        (*count)++;
    }
    closedir(dir);
}

static void wav_write_header(FILE *fp,
                             uint32_t sample_rate,
                             uint16_t bits_per_sample,
                             uint16_t channels,
                             uint32_t data_size)
{
    uint8_t h[44] = {0};
    uint32_t byte_rate = sample_rate * channels * (bits_per_sample / 8);
    uint16_t block_align = (uint16_t)(channels * (bits_per_sample / 8));
    long cur = ftell(fp);

    memcpy(&h[0], "RIFF", 4);
    le32_write(&h[4], 36 + data_size);
    memcpy(&h[8], "WAVE", 4);
    memcpy(&h[12], "fmt ", 4);
    le32_write(&h[16], 16);
    le16_write(&h[20], 1);
    le16_write(&h[22], channels);
    le32_write(&h[24], sample_rate);
    le32_write(&h[28], byte_rate);
    le16_write(&h[32], block_align);
    le16_write(&h[34], bits_per_sample);
    memcpy(&h[36], "data", 4);
    le32_write(&h[40], data_size);

    fseek(fp, 0, SEEK_SET);
    fwrite(h, 1, sizeof(h), fp);
    fseek(fp, cur, SEEK_SET);
}

static esp_err_t wav_parse(FILE *fp, wav_info_t *out)
{
    uint8_t h[12];
    if (fread(h, 1, sizeof(h), fp) != sizeof(h)) {
        return ESP_FAIL;
    }
    if (memcmp(&h[0], "RIFF", 4) != 0 || memcmp(&h[8], "WAVE", 4) != 0) {
        return ESP_FAIL;
    }

    bool got_fmt = false;
    bool got_data = false;
    uint16_t channels = 0;
    uint32_t sample_rate = 0;
    uint16_t bits = 0;
    uint32_t data_size = 0;

    while (1) {
        uint8_t chdr[8];
        if (fread(chdr, 1, sizeof(chdr), fp) != sizeof(chdr)) {
            break;
        }
        uint32_t csize = le32_read(&chdr[4]);

        if (memcmp(&chdr[0], "fmt ", 4) == 0) {
            uint8_t fmt[16];
            if (csize < sizeof(fmt) || fread(fmt, 1, sizeof(fmt), fp) != sizeof(fmt)) {
                return ESP_FAIL;
            }
            if (csize > sizeof(fmt)) {
                fseek(fp, (long)(csize - sizeof(fmt)), SEEK_CUR);
            }
            uint16_t format_tag = le16_read(&fmt[0]);
            channels = le16_read(&fmt[2]);
            sample_rate = le32_read(&fmt[4]);
            bits = le16_read(&fmt[14]);
            if (format_tag != 1) {
                return ESP_ERR_NOT_SUPPORTED;
            }
            got_fmt = true;
        } else if (memcmp(&chdr[0], "data", 4) == 0) {
            data_size = csize;
            got_data = true;
            break;
        } else {
            fseek(fp, (long)csize, SEEK_CUR);
        }
    }

    if (!got_fmt || !got_data || channels == 0 || sample_rate == 0) {
        return ESP_FAIL;
    }

    out->sample_rate = sample_rate;
    out->bits_per_sample = bits;
    out->channels = channels;
    out->data_size = data_size;
    return ESP_OK;
}

static esp_err_t sd_mount_if_needed(void)
{
    if (s_sd_mounted) {
        return ESP_OK;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = AUDIO_SD_HOST;

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = AUDIO_SD_MOSI,
        .miso_io_num = AUDIO_SD_MISO,
        .sclk_io_num = AUDIO_SD_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };

    esp_err_t ret = spi_bus_initialize(AUDIO_SD_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "spi_bus_initialize(SD) failed: %s", esp_err_to_name(ret));
        return ret;
    }

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = AUDIO_SD_CS;
    slot_config.host_id = AUDIO_SD_HOST;

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 6,
        .allocation_unit_size = 16 * 1024,
    };

    ret = esp_vfs_fat_sdspi_mount(AUDIO_SD_MOUNT_POINT, &host, &slot_config, &mount_cfg, &s_card);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "SD mount failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_sd_mounted = true;
    mkdir(AUDIO_SD_DIR, 0775);
    ESP_LOGI(TAG, "SD mounted at %s", AUDIO_SD_MOUNT_POINT);
    return ESP_OK;
}

static esp_err_t i2s_init_if_needed(void)
{
    if (s_i2s_inited) {
        return ESP_OK;
    }

    i2s_config_t i2s_cfg = {
        .mode = I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_RX,
        .sample_rate = AUDIO_REC_SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = 0,
        .dma_buf_count = 8,
        .dma_buf_len = 256,
        .use_apll = false,
        .tx_desc_auto_clear = true,
        .fixed_mclk = 0,
    };

    esp_err_t ret = i2s_driver_install(AUDIO_I2S_PORT, &i2s_cfg, 0, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_driver_install failed: %s", esp_err_to_name(ret));
        return ret;
    }

    i2s_pin_config_t pin_cfg = {
        .mck_io_num = I2S_PIN_NO_CHANGE,
        .bck_io_num = AUDIO_I2S_BCLK,
        .ws_io_num = AUDIO_I2S_WS,
        .data_out_num = AUDIO_I2S_DOUT,
        .data_in_num = AUDIO_I2S_DIN,
    };
    ret = i2s_set_pin(AUDIO_I2S_PORT, &pin_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_set_pin failed: %s", esp_err_to_name(ret));
        i2s_driver_uninstall(AUDIO_I2S_PORT);
        return ret;
    }

    i2s_zero_dma_buffer(AUDIO_I2S_PORT);
    s_i2s_inited = true;
    ESP_LOGI(TAG, "I2S initialized (BCLK=%d WS=%d DOUT=%d DIN=%d)",
             AUDIO_I2S_BCLK, AUDIO_I2S_WS, AUDIO_I2S_DOUT, AUDIO_I2S_DIN);
    return ESP_OK;
}

static void record_task(void *arg)
{
    (void)arg;
    uint8_t buf[AUDIO_IO_CHUNK_BYTES];
    int32_t peak_l = 0;
    int32_t peak_r = 0;
    uint64_t last_log_samples = 0;

    lock_take();
    FILE *fp = s_record_fp;
    lock_give();
    if (!fp) {
        vTaskDelete(NULL);
        return;
    }

    wav_write_header(fp, AUDIO_REC_SAMPLE_RATE, AUDIO_REC_BITS, AUDIO_REC_CHANNELS, 0);
    i2s_zero_dma_buffer(AUDIO_I2S_PORT);

    while (1) {
        lock_take();
        bool stop = s_record_stop;
        lock_give();
        if (stop) {
            break;
        }

        size_t bytes_read = 0;
        esp_err_t ret = i2s_read(AUDIO_I2S_PORT, buf, sizeof(buf), &bytes_read, pdMS_TO_TICKS(200));
        if (ret != ESP_OK || bytes_read == 0) {
            continue;
        }

        /* Data is interleaved LR frames (32-bit each).
         * Mic is mono on many boards, so use the stronger channel and duplicate to LR. */
        size_t frames = bytes_read / (sizeof(int32_t) * 2);
        const int32_t *pcm = (const int32_t *)buf;
        int32_t *pcm_out = (int32_t *)buf;
        for (size_t i = 0; i < frames; i++) {
            int32_t in_l = pcm[i * 2];
            int32_t in_r = pcm[i * 2 + 1];
            int32_t al = (int32_t)((in_l >= 0) ? in_l : -(int64_t)in_l);
            int32_t ar = (int32_t)((in_r >= 0) ? in_r : -(int64_t)in_r);
            int32_t mono = (al >= ar) ? in_l : in_r;
            int32_t out = apply_rec_gain_clip(mono);
            int32_t ao = (int32_t)((out >= 0) ? out : -(int64_t)out);

            pcm_out[i * 2] = out;
            pcm_out[i * 2 + 1] = out;

            if (ao > peak_l) peak_l = ao;
            if (ao > peak_r) peak_r = ao;
        }
        s_rec_samples_total += frames;
        if (s_rec_samples_total - last_log_samples >= AUDIO_REC_SAMPLE_RATE) {
            ESP_LOGI(TAG, "rec level peak L=%ld R=%ld",
                     (long)peak_l, (long)peak_r);
            peak_l = 0;
            peak_r = 0;
            last_log_samples = s_rec_samples_total;
        }

        size_t wrote = fwrite(buf, 1, bytes_read, fp);
        if (wrote != bytes_read) {
            ESP_LOGE(TAG, "record write failed");
            break;
        }

        lock_take();
        s_record_data_bytes += wrote;
        lock_give();
    }

    lock_take();
    wav_write_header(fp, AUDIO_REC_SAMPLE_RATE, AUDIO_REC_BITS, AUDIO_REC_CHANNELS,
                     (uint32_t)s_record_data_bytes);
    fflush(fp);
    fclose(fp);
    ESP_LOGI(TAG, "record saved: %s (%u bytes)", s_record_path, (unsigned)s_record_data_bytes);
    s_record_fp = NULL;
    s_record_data_bytes = 0;
    s_recording = false;
    s_record_stop = false;
    s_record_task = NULL;
    lock_give();

    vTaskDelete(NULL);
}

static void play_task(void *arg)
{
    char *path = (char *)arg;
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        ESP_LOGE(TAG, "open failed: %s", path);
        goto done;
    }

    wav_info_t wav = {0};
    esp_err_t ret = wav_parse(fp, &wav);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "wav parse failed: %s", path);
        fclose(fp);
        goto done;
    }

    if ((wav.bits_per_sample != 16 && wav.bits_per_sample != 32) ||
        (wav.channels != 1 && wav.channels != 2)) {
        ESP_LOGE(TAG, "unsupported wav format: bits=%u ch=%u", wav.bits_per_sample, wav.channels);
        fclose(fp);
        goto done;
    }

    ret = i2s_set_clk(AUDIO_I2S_PORT, wav.sample_rate,
                      (i2s_bits_per_sample_t)wav.bits_per_sample,
                      (i2s_channel_t)wav.channels);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_set_clk(play) failed: %s", esp_err_to_name(ret));
        fclose(fp);
        goto done;
    }

    i2s_zero_dma_buffer(AUDIO_I2S_PORT);
    uint8_t buf[AUDIO_IO_CHUNK_BYTES];
    uint32_t remain = wav.data_size;

    while (remain > 0) {
        lock_take();
        bool stop = s_play_stop;
        int vol_percent = s_play_vol_percent;
        lock_give();
        if (stop) break;

        size_t want = remain > sizeof(buf) ? sizeof(buf) : remain;
        size_t got = fread(buf, 1, want, fp);
        if (got == 0) break;

        if (wav.bits_per_sample == 32) {
            int32_t *p = (int32_t *)buf;
            size_t n = got / sizeof(int32_t);
            for (size_t i = 0; i < n; i++) {
                p[i] = apply_play_vol_clip32(p[i], vol_percent);
            }
        } else if (wav.bits_per_sample == 16) {
            int16_t *p = (int16_t *)buf;
            size_t n = got / sizeof(int16_t);
            for (size_t i = 0; i < n; i++) {
                p[i] = apply_play_vol_clip16(p[i], vol_percent);
            }
        }

        size_t written = 0;
        ret = i2s_write(AUDIO_I2S_PORT, buf, got, &written, pdMS_TO_TICKS(500));
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "i2s_write(play) failed: %s", esp_err_to_name(ret));
            break;
        }
        remain -= (uint32_t)got;
    }

    fclose(fp);

done:
    lock_take();
    s_playing = false;
    s_play_stop = false;
    s_play_task = NULL;
    lock_give();

    free(path);
    vTaskDelete(NULL);
}

esp_err_t audio_service_init(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) return ESP_ERR_NO_MEM;
    }

    lock_take();
    if (s_inited) {
        lock_give();
        return ESP_OK;
    }

    esp_err_t ret = sd_mount_if_needed();
    if (ret == ESP_OK) {
        ret = i2s_init_if_needed();
    }
    if (ret == ESP_OK) {
        s_inited = true;
    }
    lock_give();
    return ret;
}

bool audio_service_is_recording(void)
{
    bool v = false;
    lock_take();
    v = s_recording;
    lock_give();
    return v;
}

bool audio_service_is_playing(void)
{
    bool v = false;
    lock_take();
    v = s_playing;
    lock_give();
    return v;
}

esp_err_t audio_service_start_recording(char *out_path, size_t out_path_size)
{
    esp_err_t ret = audio_service_init();
    if (ret != ESP_OK) return ret;

    lock_take();
    if (s_recording || s_playing) {
        lock_give();
        return ESP_ERR_INVALID_STATE;
    }

    uint64_t sec = (uint64_t)(esp_timer_get_time() / 1000000ULL);
    snprintf(s_record_path, sizeof(s_record_path), "%s/rec_%llu.wav",
             AUDIO_SD_DIR, (unsigned long long)sec);

    FILE *fp = fopen(s_record_path, "wb");
    if (!fp) {
        lock_give();
        return ESP_FAIL;
    }

    ret = i2s_set_clk(AUDIO_I2S_PORT, AUDIO_REC_SAMPLE_RATE,
                      I2S_BITS_PER_SAMPLE_32BIT, I2S_CHANNEL_STEREO);
    if (ret != ESP_OK) {
        fclose(fp);
        lock_give();
        return ret;
    }

    s_record_fp = fp;
    s_record_data_bytes = 0;
    s_rec_samples_total = 0;
    s_record_stop = false;
    s_recording = true;
    BaseType_t ok = xTaskCreatePinnedToCore(record_task, "audio_rec", 4096, NULL, 4, &s_record_task, 0);
    if (ok != pdPASS) {
        fclose(fp);
        s_record_fp = NULL;
        s_recording = false;
        lock_give();
        return ESP_ERR_NO_MEM;
    }

    if (out_path && out_path_size > 0) {
        strncpy(out_path, s_record_path, out_path_size - 1);
        out_path[out_path_size - 1] = '\0';
    }
    lock_give();
    return ESP_OK;
}

esp_err_t audio_service_stop_recording(void)
{
    lock_take();
    if (!s_recording) {
        lock_give();
        return ESP_ERR_INVALID_STATE;
    }
    s_record_stop = true;
    lock_give();

    for (int i = 0; i < 40; i++) {
        if (!audio_service_is_recording()) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t audio_service_play_file(const char *path)
{
    if (!path || path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = audio_service_init();
    if (ret != ESP_OK) return ret;

    for (int i = 0; i < 20; i++) {
        lock_take();
        if (!s_playing) {
            lock_give();
            break;
        }
        s_play_stop = true;
        lock_give();
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    lock_take();
    if (s_recording || s_playing) {
        lock_give();
        return ESP_ERR_INVALID_STATE;
    }

    char *path_dup = strdup(path);
    if (!path_dup) {
        lock_give();
        return ESP_ERR_NO_MEM;
    }

    s_play_stop = false;
    s_playing = true;
    BaseType_t ok = xTaskCreatePinnedToCore(play_task, "audio_play", 4096, path_dup, 4, &s_play_task, 0);
    if (ok != pdPASS) {
        s_playing = false;
        free(path_dup);
        lock_give();
        return ESP_ERR_NO_MEM;
    }
    lock_give();
    return ESP_OK;
}

esp_err_t audio_service_set_playback_volume_percent(int percent)
{
    if (percent < 0 || percent > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    lock_take();
    s_play_vol_percent = percent;
    lock_give();
    return ESP_OK;
}

int audio_service_get_playback_volume_percent(void)
{
    int v = 0;
    lock_take();
    v = s_play_vol_percent;
    lock_give();
    return v;
}

esp_err_t audio_service_list_files(char names[][AUDIO_SERVICE_MAX_NAME_LEN],
                                   size_t max_files,
                                   size_t *out_count)
{
    if (!names || max_files == 0 || !out_count) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_count = 0;

    esp_err_t ret = audio_service_init();
    if (ret != ESP_OK) return ret;

    collect_wav_from_dir(AUDIO_SD_MOUNT_POINT, "", names, max_files, out_count);
    collect_wav_from_dir(AUDIO_SD_DIR, "audio/", names, max_files, out_count);

    if (*out_count > 1) {
        qsort(names, *out_count, AUDIO_SERVICE_MAX_NAME_LEN, wav_name_cmp);
    }
    return ESP_OK;
}
