#include "config_ui.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl/lvgl.h"
#include "nvs.h"

#include "llm/llm_proxy.h"
#include "mimi_config.h"
#include "proxy/http_proxy.h"
#include "feishu/feishu_bot.h"
#include "tools/tool_web_search.h"
#include "audio/audio_service.h"
#include "ui/board_config.h"
#include "ui/display_port.h"
#include "wifi/wifi_manager.h"

static const char *TAG = "config_ui";

static TaskHandle_t s_ui_task = NULL;

static lv_obj_t *s_kb = NULL;
static lv_obj_t *s_current_status_label = NULL;
static lv_obj_t *s_active_form_page = NULL;

static lv_obj_t *s_scr_home = NULL;
static lv_obj_t *s_scr_wifi = NULL;
static lv_obj_t *s_scr_proxy = NULL;
static lv_obj_t *s_scr_llm = NULL;
static lv_obj_t *s_scr_feishu = NULL;
static lv_obj_t *s_scr_search = NULL;
static lv_obj_t *s_scr_system = NULL;
static lv_obj_t *s_scr_audio = NULL;

static lv_obj_t *s_home_ip_label = NULL;
static lv_obj_t *s_home_machine_label = NULL;

static lv_obj_t *s_status_wifi = NULL;
static lv_obj_t *s_status_proxy = NULL;
static lv_obj_t *s_status_llm = NULL;
static lv_obj_t *s_status_feishu = NULL;
static lv_obj_t *s_status_search = NULL;
static lv_obj_t *s_status_system = NULL;
static lv_obj_t *s_status_audio = NULL;

static lv_obj_t *s_form_wifi = NULL;
static lv_obj_t *s_form_proxy = NULL;
static lv_obj_t *s_form_llm = NULL;
static lv_obj_t *s_form_feishu = NULL;
static lv_obj_t *s_form_search = NULL;
static lv_obj_t *s_form_system = NULL;
static lv_obj_t *s_form_audio = NULL;

static lv_obj_t *s_ta_wifi_ssid = NULL;
static lv_obj_t *s_ta_wifi_pass = NULL;
static lv_obj_t *s_ta_proxy_host = NULL;
static lv_obj_t *s_ta_proxy_port = NULL;
static lv_obj_t *s_ta_api_key = NULL;
static lv_obj_t *s_ta_model = NULL;
static lv_obj_t *s_ta_feishu_app_id = NULL;
static lv_obj_t *s_ta_feishu_app_secret = NULL;
static lv_obj_t *s_ta_feishu_chat_id = NULL;
static lv_obj_t *s_ta_search_key = NULL;
static lv_obj_t *s_audio_list = NULL;
static lv_obj_t *s_audio_vol_slider = NULL;
static lv_obj_t *s_audio_vol_label = NULL;

static char s_audio_files[AUDIO_SERVICE_MAX_FILES][AUDIO_SERVICE_MAX_NAME_LEN];
static size_t s_audio_file_count = 0;

#define UI_MARGIN               6
#define HOME_TOP_Y              4
#define HOME_GRID_Y             28
#define HOME_GRID_W             (MIMI_LCD_HOR_RES - 2 * UI_MARGIN)
#define HOME_GRID_H             150
#define HOME_CARD_W             94
#define HOME_CARD_H             62
#define HOME_CARD_GAP_X         10
#define HOME_CARD_GAP_Y         10
#define HOME_ICON_BG_D          24
#define HOME_ICON_Y             6
#define HOME_TITLE_Y            (HOME_ICON_Y + HOME_ICON_BG_D + 3)

#define SUB_TOP_H               34
#define SUB_STATUS_H            18
#define SUB_BODY_H              (MIMI_LCD_VER_RES - SUB_TOP_H - SUB_STATUS_H)
#define SUB_FIELD_LABEL_H       14
#define SUB_FIELD_BLOCK_H       42
#define SUB_FIELD_W             (MIMI_LCD_HOR_RES - 24)
#define SUB_KB_H                88

static void open_home(void);
static void open_wifi(void);
static void open_proxy(void);
static void open_llm(void);
static void open_feishu(void);
static void open_search(void);
static void open_system(void);
static void open_audio(void);
static void audio_list_item_cb(lv_obj_t *btn, lv_event_t event);
static void audio_vol_slider_cb(lv_obj_t *obj, lv_event_t event);

static void audio_update_volume_label(int vol_percent)
{
    if (!s_audio_vol_label) return;
    char buf[32];
    snprintf(buf, sizeof(buf), "Play Vol: %d%%", vol_percent);
    lv_label_set_text(s_audio_vol_label, buf);
}

static void ui_set_status(const char *msg)
{
    if (s_current_status_label) {
        lv_label_set_text(s_current_status_label, msg);
    }
    ESP_LOGI(TAG, "%s", msg);
}

static void ui_load_nvs_str(const char *ns, const char *key, const char *fallback,
                            char *out, size_t out_size)
{
    out[0] = '\0';
    nvs_handle_t nvs;
    if (nvs_open(ns, NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = out_size;
        if (nvs_get_str(nvs, key, out, &len) == ESP_OK && out[0] != '\0') {
            nvs_close(nvs);
            return;
        }
        nvs_close(nvs);
    }

    if (fallback && fallback[0] != '\0') {
        strncpy(out, fallback, out_size - 1);
        out[out_size - 1] = '\0';
    }
}

static uint16_t ui_load_nvs_u16(const char *ns, const char *key, uint16_t fallback)
{
    nvs_handle_t nvs;
    if (nvs_open(ns, NVS_READONLY, &nvs) == ESP_OK) {
        uint16_t v = 0;
        if (nvs_get_u16(nvs, key, &v) == ESP_OK && v != 0) {
            nvs_close(nvs);
            return v;
        }
        nvs_close(nvs);
    }
    return fallback;
}

static void close_keyboard(void)
{
    if (s_kb) {
        lv_keyboard_set_textarea(s_kb, NULL);
        lv_obj_del(s_kb);
        s_kb = NULL;
    }
}

static void kb_event_cb(lv_obj_t *kb, lv_event_t event)
{
    lv_keyboard_def_event_cb(kb, event);

    if (event == LV_EVENT_CANCEL || event == LV_EVENT_APPLY) {
        close_keyboard();
    }
}

static void ta_event_cb(lv_obj_t *ta, lv_event_t event)
{
    if (event != LV_EVENT_CLICKED) {
        return;
    }

    if (s_active_form_page) {
        lv_page_focus(s_active_form_page, ta, LV_ANIM_ON);
    }

    if (!s_kb) {
        s_kb = lv_keyboard_create(lv_scr_act(), NULL);
        lv_keyboard_set_cursor_manage(s_kb, true);
        lv_obj_set_event_cb(s_kb, kb_event_cb);
        lv_obj_set_height(s_kb, SUB_KB_H);
        lv_obj_align(s_kb, NULL, LV_ALIGN_IN_BOTTOM_MID, 0, 0);
    }
    lv_keyboard_set_textarea(s_kb, ta);
}

static int create_labeled_ta(lv_obj_t *parent, lv_obj_t **ta_out, const char *label_txt, int y, bool pwd)
{
    lv_obj_t *label = lv_label_create(parent, NULL);
    lv_label_set_text(label, label_txt);
    lv_obj_align(label, NULL, LV_ALIGN_IN_TOP_LEFT, UI_MARGIN, y);

    lv_obj_t *ta = lv_textarea_create(parent, NULL);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_pwd_mode(ta, pwd);
    lv_obj_set_width(ta, SUB_FIELD_W);
    lv_obj_align(ta, NULL, LV_ALIGN_IN_TOP_LEFT, UI_MARGIN, y + SUB_FIELD_LABEL_H);
    lv_obj_set_event_cb(ta, ta_event_cb);

    *ta_out = ta;
    return y + SUB_FIELD_BLOCK_H;
}

static lv_obj_t *create_action_btn(lv_obj_t *parent, const char *txt, int x, int y, int w, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_btn_create(parent, NULL);
    lv_obj_set_size(btn, w, 30);
    lv_obj_align(btn, NULL, LV_ALIGN_IN_TOP_LEFT, x, y);
    lv_obj_set_event_cb(btn, cb);

    lv_obj_t *label = lv_label_create(btn, NULL);
    lv_label_set_text(label, txt);
    return btn;
}

static void show_screen(lv_obj_t *scr, lv_obj_t *status_label, lv_obj_t *form_page)
{
    close_keyboard();
    s_current_status_label = status_label;
    s_active_form_page = form_page;
    lv_scr_load(scr);
}

static void update_home_status_labels(void)
{
    if (!s_home_ip_label || !s_home_machine_label) {
        return;
    }

    char ip_line[48];
    if (wifi_manager_is_connected()) {
        snprintf(ip_line, sizeof(ip_line), "IP: %s", wifi_manager_get_ip());
    } else {
        snprintf(ip_line, sizeof(ip_line), "IP: 0.0.0.0");
    }
    lv_label_set_text(s_home_ip_label, ip_line);

    lv_label_set_text(s_home_machine_label, "Machine: RUNNING");
}

static void load_values_to_widgets(void)
{
    char tmp[128];

    if (s_ta_wifi_ssid) {
        ui_load_nvs_str(MIMI_NVS_WIFI, MIMI_NVS_KEY_SSID, MIMI_SECRET_WIFI_SSID, tmp, sizeof(tmp));
        lv_textarea_set_text(s_ta_wifi_ssid, tmp);
    }
    if (s_ta_wifi_pass) {
        ui_load_nvs_str(MIMI_NVS_WIFI, MIMI_NVS_KEY_PASS, MIMI_SECRET_WIFI_PASS, tmp, sizeof(tmp));
        lv_textarea_set_text(s_ta_wifi_pass, tmp);
    }
    if (s_ta_proxy_host) {
        ui_load_nvs_str(MIMI_NVS_PROXY, MIMI_NVS_KEY_PROXY_HOST, MIMI_SECRET_PROXY_HOST, tmp, sizeof(tmp));
        lv_textarea_set_text(s_ta_proxy_host, tmp);
    }
    if (s_ta_proxy_port) {
        uint16_t port = ui_load_nvs_u16(MIMI_NVS_PROXY, MIMI_NVS_KEY_PROXY_PORT,
                                        (uint16_t)atoi(MIMI_SECRET_PROXY_PORT));
        if (port == 0) {
            tmp[0] = '\0';
        } else {
            snprintf(tmp, sizeof(tmp), "%u", (unsigned)port);
        }
        lv_textarea_set_text(s_ta_proxy_port, tmp);
    }
    if (s_ta_api_key) {
        ui_load_nvs_str(MIMI_NVS_LLM, MIMI_NVS_KEY_API_KEY, MIMI_SECRET_API_KEY, tmp, sizeof(tmp));
        lv_textarea_set_text(s_ta_api_key, tmp);
    }
    if (s_ta_model) {
        ui_load_nvs_str(MIMI_NVS_LLM, MIMI_NVS_KEY_MODEL, MIMI_SECRET_MODEL, tmp, sizeof(tmp));
        if (tmp[0] == '\0') {
            strncpy(tmp, MIMI_LLM_DEFAULT_MODEL, sizeof(tmp) - 1);
            tmp[sizeof(tmp) - 1] = '\0';
        }
        lv_textarea_set_text(s_ta_model, tmp);
    }
    if (s_ta_feishu_app_id) {
        ui_load_nvs_str(MIMI_NVS_FEISHU, MIMI_NVS_KEY_FEISHU_APP_ID,
                        MIMI_SECRET_FEISHU_APP_ID, tmp, sizeof(tmp));
        lv_textarea_set_text(s_ta_feishu_app_id, tmp);
    }
    if (s_ta_feishu_app_secret) {
        ui_load_nvs_str(MIMI_NVS_FEISHU, MIMI_NVS_KEY_FEISHU_APP_SECRET,
                        MIMI_SECRET_FEISHU_APP_SECRET, tmp, sizeof(tmp));
        lv_textarea_set_text(s_ta_feishu_app_secret, tmp);
    }
    if (s_ta_feishu_chat_id) {
        ui_load_nvs_str(MIMI_NVS_FEISHU, MIMI_NVS_KEY_FEISHU_DEF_CHAT,
                        MIMI_SECRET_FEISHU_DEFAULT_CHAT_ID, tmp, sizeof(tmp));
        lv_textarea_set_text(s_ta_feishu_chat_id, tmp);
    }
    if (s_ta_search_key) {
        ui_load_nvs_str(MIMI_NVS_SEARCH, MIMI_NVS_KEY_API_KEY, MIMI_SECRET_SEARCH_KEY, tmp, sizeof(tmp));
        lv_textarea_set_text(s_ta_search_key, tmp);
    }
}

static void back_event_cb(lv_obj_t *obj, lv_event_t event)
{
    (void)obj;
    if (event == LV_EVENT_CLICKED) {
        open_home();
    }
}

static void wifi_save_cb(lv_obj_t *btn, lv_event_t event)
{
    (void)btn;
    if (event != LV_EVENT_CLICKED) return;

    const char *ssid = lv_textarea_get_text(s_ta_wifi_ssid);
    const char *pass = lv_textarea_get_text(s_ta_wifi_pass);
    if (!ssid || ssid[0] == '\0') {
        ui_set_status("WiFi SSID is empty");
        return;
    }

    esp_err_t ret = wifi_manager_set_credentials(ssid, pass ? pass : "");
    ui_set_status(ret == ESP_OK ? "WiFi saved (restart recommended)" : "Save WiFi failed");
}

static void proxy_save_cb(lv_obj_t *btn, lv_event_t event)
{
    (void)btn;
    if (event != LV_EVENT_CLICKED) return;

    const char *host = lv_textarea_get_text(s_ta_proxy_host);
    const char *port_str = lv_textarea_get_text(s_ta_proxy_port);

    if (!host || host[0] == '\0') {
        http_proxy_clear();
        ui_set_status("Proxy cleared");
        return;
    }

    long port = strtol(port_str ? port_str : "0", NULL, 10);
    if (port <= 0 || port > 65535) {
        ui_set_status("Invalid proxy port");
        return;
    }

    esp_err_t ret = http_proxy_set(host, (uint16_t)port);
    ui_set_status(ret == ESP_OK ? "Proxy saved (restart recommended)" : "Save proxy failed");
}

static void llm_save_cb(lv_obj_t *btn, lv_event_t event)
{
    (void)btn;
    if (event != LV_EVENT_CLICKED) return;

    const char *api_key = lv_textarea_get_text(s_ta_api_key);
    const char *model = lv_textarea_get_text(s_ta_model);
    if (llm_set_api_key(api_key ? api_key : "") != ESP_OK) {
        ui_set_status("Save API key failed");
        return;
    }
    if (llm_set_model(model ? model : "") != ESP_OK) {
        ui_set_status("Save model failed");
        return;
    }
    ui_set_status("LLM config saved");
}

static void feishu_save_cb(lv_obj_t *btn, lv_event_t event)
{
    (void)btn;
    if (event != LV_EVENT_CLICKED) return;

    const char *app_id = lv_textarea_get_text(s_ta_feishu_app_id);
    const char *app_secret = lv_textarea_get_text(s_ta_feishu_app_secret);
    const char *chat_id = lv_textarea_get_text(s_ta_feishu_chat_id);

    esp_err_t ret = ESP_OK;
    if ((!app_id || app_id[0] == '\0') && (!app_secret || app_secret[0] == '\0')) {
        ret = feishu_bot_clear_app_credentials();
    } else if (!app_id || !app_id[0] || !app_secret || !app_secret[0]) {
        ui_set_status("AppID/AppSecret must both be set");
        return;
    } else {
        ret = feishu_bot_set_app_credentials(app_id, app_secret);
    }
    if (ret != ESP_OK) {
        ui_set_status("Save Feishu app failed");
        return;
    }

    if (!chat_id || chat_id[0] == '\0') {
        ret = feishu_bot_clear_default_chat_id();
    } else {
        ret = feishu_bot_set_default_chat_id(chat_id);
    }
    if (ret != ESP_OK) {
        ui_set_status("Save Feishu chat_id failed");
        return;
    }

    esp_err_t start_ret = feishu_bot_start();
    if (start_ret == ESP_OK) {
        ui_set_status("Feishu app saved, long conn started");
    } else if (start_ret == ESP_ERR_INVALID_STATE) {
        ui_set_status("Feishu app cleared");
    } else {
        ui_set_status("Feishu saved, long conn start failed");
    }
}

static void feishu_test_cb(lv_obj_t *btn, lv_event_t event)
{
    (void)btn;
    if (event != LV_EVENT_CLICKED) return;

    esp_err_t ret = feishu_bot_send_message("MimiClaw Feishu test message");
    ui_set_status(ret == ESP_OK ? "Feishu test sent" : "Feishu test failed (set chat_id?)");
}

static void search_save_cb(lv_obj_t *btn, lv_event_t event)
{
    (void)btn;
    if (event != LV_EVENT_CLICKED) return;

    const char *key = lv_textarea_get_text(s_ta_search_key);
    esp_err_t ret = tool_web_search_set_key(key ? key : "");
    ui_set_status(ret == ESP_OK ? "Search key saved" : "Save search key failed");
}

static void audio_refresh_file_list(void)
{
    if (!s_audio_list) return;

    s_audio_file_count = 0;
    lv_list_clean(s_audio_list);

    esp_err_t ret = audio_service_list_files(s_audio_files, AUDIO_SERVICE_MAX_FILES, &s_audio_file_count);
    if (ret != ESP_OK) {
        ui_set_status("SD not ready or no card");
        return;
    }

    if (s_audio_file_count == 0) {
        lv_list_add_btn(s_audio_list, NULL, "(No WAV files)");
        ui_set_status("Audio folder empty");
        return;
    }

    for (size_t i = 0; i < s_audio_file_count; i++) {
        lv_obj_t *btn = lv_list_add_btn(s_audio_list, NULL, s_audio_files[i]);
        lv_obj_set_event_cb(btn, audio_list_item_cb);
    }
    ui_set_status("File list refreshed");
}

static void audio_list_item_cb(lv_obj_t *btn, lv_event_t event)
{
    if (event != LV_EVENT_CLICKED) return;

    const char *name = lv_list_get_btn_text(btn);
    if (!name || name[0] == '\0' || name[0] == '(') {
        return;
    }

    char path[128];
    snprintf(path, sizeof(path), "/sdcard/%s", name);
    esp_err_t ret = audio_service_play_file(path);
    if (ret == ESP_OK) {
        ui_set_status("Playing...");
    } else if (ret == ESP_ERR_INVALID_STATE) {
        ui_set_status("Audio busy (recording/playing)");
    } else {
        ui_set_status("Play failed");
    }
}

static void audio_start_rec_cb(lv_obj_t *btn, lv_event_t event)
{
    (void)btn;
    if (event != LV_EVENT_CLICKED) return;

    char path[128];
    esp_err_t ret = audio_service_start_recording(path, sizeof(path));
    if (ret == ESP_OK) {
        ui_set_status("Recording...");
    } else if (ret == ESP_ERR_INVALID_STATE) {
        ui_set_status("Audio busy (recording/playing)");
    } else {
        ui_set_status("Start record failed");
    }
}

static void audio_stop_rec_cb(lv_obj_t *btn, lv_event_t event)
{
    (void)btn;
    if (event != LV_EVENT_CLICKED) return;

    esp_err_t ret = audio_service_stop_recording();
    if (ret == ESP_OK) {
        ui_set_status("Recording saved");
        audio_refresh_file_list();
    } else if (ret == ESP_ERR_INVALID_STATE) {
        ui_set_status("Not recording");
    } else {
        ui_set_status("Stop record timeout/fail");
    }
}

static void audio_refresh_cb(lv_obj_t *btn, lv_event_t event)
{
    (void)btn;
    if (event != LV_EVENT_CLICKED) return;
    audio_refresh_file_list();
}

static void audio_vol_slider_cb(lv_obj_t *obj, lv_event_t event)
{
    if (event != LV_EVENT_VALUE_CHANGED) return;
    int vol = lv_slider_get_value(obj);
    if (audio_service_set_playback_volume_percent(vol) == ESP_OK) {
        audio_update_volume_label(vol);
    }
}

static void system_audio_cb(lv_obj_t *btn, lv_event_t event)
{
    (void)btn;
    if (event == LV_EVENT_CLICKED) open_audio();
}

static void restart_cb(lv_obj_t *btn, lv_event_t event)
{
    (void)btn;
    if (event != LV_EVENT_CLICKED) return;

    ui_set_status("Restarting...");
    vTaskDelay(pdMS_TO_TICKS(150));
    esp_restart();
}

static lv_obj_t *create_subscreen(lv_obj_t **status_label_out, lv_obj_t **form_page_out)
{
    lv_obj_t *scr = lv_obj_create(NULL, NULL);

    lv_obj_t *btn_back = lv_btn_create(scr, NULL);
    lv_obj_set_size(btn_back, 74, 26);
    lv_obj_align(btn_back, NULL, LV_ALIGN_IN_TOP_LEFT, UI_MARGIN, 4);
    lv_obj_set_event_cb(btn_back, back_event_cb);

    lv_obj_t *lbl_back = lv_label_create(btn_back, NULL);
    lv_label_set_text(lbl_back, LV_SYMBOL_LEFT " Back");

    lv_obj_t *form_page = lv_page_create(scr, NULL);
    lv_obj_set_size(form_page, MIMI_LCD_HOR_RES, SUB_BODY_H);
    lv_obj_align(form_page, NULL, LV_ALIGN_IN_TOP_MID, 0, SUB_TOP_H);
    lv_page_set_scrollbar_mode(form_page, LV_SCROLLBAR_MODE_AUTO);

    lv_obj_t *status = lv_label_create(scr, NULL);
    lv_label_set_text(status, "Ready");
    lv_label_set_long_mode(status, LV_LABEL_LONG_DOT);
    lv_obj_set_width(status, MIMI_LCD_HOR_RES - 2 * UI_MARGIN);
    lv_obj_align(status, NULL, LV_ALIGN_IN_BOTTOM_LEFT, UI_MARGIN, -2);

    *status_label_out = status;
    *form_page_out = form_page;
    return scr;
}

static lv_obj_t *create_home_card(lv_obj_t *parent, int x, int y,
                                  const char *icon, const char *title, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_btn_create(parent, NULL);
    lv_obj_set_size(btn, HOME_CARD_W, HOME_CARD_H);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_event_cb(btn, cb);

    lv_obj_t *icon_bg = lv_obj_create(btn, NULL);
    lv_obj_set_size(icon_bg, HOME_ICON_BG_D, HOME_ICON_BG_D);
    lv_obj_align(icon_bg, NULL, LV_ALIGN_IN_TOP_MID, 0, HOME_ICON_Y);
    lv_obj_set_click(icon_bg, false);
    lv_obj_set_style_local_radius(icon_bg, LV_OBJ_PART_MAIN, LV_STATE_DEFAULT, HOME_ICON_BG_D / 2);

    lv_obj_t *icon_label = lv_label_create(icon_bg, NULL);
    lv_label_set_text(icon_label, icon);
    lv_obj_align(icon_label, NULL, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *text_label = lv_label_create(btn, NULL);
    lv_label_set_text(text_label, title);
    lv_obj_align(text_label, NULL, LV_ALIGN_IN_TOP_MID, 0, HOME_TITLE_Y);
    return btn;
}

static void home_wifi_cb(lv_obj_t *obj, lv_event_t event)   { (void)obj; if (event == LV_EVENT_CLICKED) open_wifi(); }
static void home_proxy_cb(lv_obj_t *obj, lv_event_t event)  { (void)obj; if (event == LV_EVENT_CLICKED) open_proxy(); }
static void home_llm_cb(lv_obj_t *obj, lv_event_t event)    { (void)obj; if (event == LV_EVENT_CLICKED) open_llm(); }
static void home_feishu_cb(lv_obj_t *obj, lv_event_t event) { (void)obj; if (event == LV_EVENT_CLICKED) open_feishu(); }
static void home_search_cb(lv_obj_t *obj, lv_event_t event) { (void)obj; if (event == LV_EVENT_CLICKED) open_search(); }
static void home_sys_cb(lv_obj_t *obj, lv_event_t event)    { (void)obj; if (event == LV_EVENT_CLICKED) open_system(); }
static void home_audio_cb(lv_obj_t *obj, lv_event_t event)  { (void)obj; if (event == LV_EVENT_CLICKED) open_audio(); }

static void create_home_screen(void)
{
    s_scr_home = lv_obj_create(NULL, NULL);

    lv_obj_t *title = lv_label_create(s_scr_home, NULL);
    lv_label_set_text(title, "MimiClaw Home");
    lv_obj_align(title, NULL, LV_ALIGN_IN_TOP_MID, 0, HOME_TOP_Y);

    lv_obj_t *grid = lv_cont_create(s_scr_home, NULL);
    lv_obj_set_size(grid, HOME_GRID_W, HOME_GRID_H);
    lv_obj_align(grid, NULL, LV_ALIGN_IN_TOP_MID, 0, HOME_GRID_Y);
    lv_cont_set_layout(grid, LV_LAYOUT_OFF);
    lv_cont_set_fit(grid, LV_FIT_NONE);

    create_home_card(grid, 0, 0, LV_SYMBOL_WIFI, "WiFi", home_wifi_cb);
    create_home_card(grid, HOME_CARD_W + HOME_CARD_GAP_X, 0, LV_SYMBOL_UPLOAD, "Proxy", home_proxy_cb);
    create_home_card(grid, 2 * (HOME_CARD_W + HOME_CARD_GAP_X), 0, LV_SYMBOL_SETTINGS, "LLM", home_llm_cb);
    create_home_card(grid, 0, HOME_CARD_H + HOME_CARD_GAP_Y, LV_SYMBOL_BELL, "Feishu", home_feishu_cb);
    create_home_card(grid, HOME_CARD_W + HOME_CARD_GAP_X, HOME_CARD_H + HOME_CARD_GAP_Y, LV_SYMBOL_GPS, "Search", home_search_cb);
    create_home_card(grid, 2 * (HOME_CARD_W + HOME_CARD_GAP_X), HOME_CARD_H + HOME_CARD_GAP_Y, LV_SYMBOL_POWER, "System", home_sys_cb);

    s_home_ip_label = lv_label_create(s_scr_home, NULL);
    lv_label_set_text(s_home_ip_label, "IP: 0.0.0.0");
    lv_obj_align(s_home_ip_label, NULL, LV_ALIGN_IN_BOTTOM_LEFT, UI_MARGIN, -20);

    s_home_machine_label = lv_label_create(s_scr_home, NULL);
    lv_label_set_text(s_home_machine_label, "Machine: RUNNING");
    lv_obj_align(s_home_machine_label, NULL, LV_ALIGN_IN_BOTTOM_LEFT, UI_MARGIN, -4);

    lv_obj_t *btn_audio = lv_btn_create(s_scr_home, NULL);
    lv_obj_set_size(btn_audio, 72, 24);
    lv_obj_align(btn_audio, NULL, LV_ALIGN_IN_TOP_RIGHT, -UI_MARGIN, 4);
    lv_obj_set_event_cb(btn_audio, home_audio_cb);

    lv_obj_t *lbl_audio = lv_label_create(btn_audio, NULL);
    lv_label_set_text(lbl_audio, "Audio");
}

static void create_wifi_screen(void)
{
    s_scr_wifi = create_subscreen(&s_status_wifi, &s_form_wifi);
    int y = UI_MARGIN;
    y = create_labeled_ta(s_form_wifi, &s_ta_wifi_ssid, "WiFi SSID", y, false);
    y = create_labeled_ta(s_form_wifi, &s_ta_wifi_pass, "WiFi Password", y, true);
    create_action_btn(s_form_wifi, "Save WiFi", UI_MARGIN, y, 100, wifi_save_cb);
}

static void create_proxy_screen(void)
{
    s_scr_proxy = create_subscreen(&s_status_proxy, &s_form_proxy);
    int y = UI_MARGIN;
    y = create_labeled_ta(s_form_proxy, &s_ta_proxy_host, "Proxy Host", y, false);
    y = create_labeled_ta(s_form_proxy, &s_ta_proxy_port, "Proxy Port", y, false);
    create_action_btn(s_form_proxy, "Save Proxy", UI_MARGIN, y, 110, proxy_save_cb);
}

static void create_llm_screen(void)
{
    s_scr_llm = create_subscreen(&s_status_llm, &s_form_llm);
    int y = UI_MARGIN;
    y = create_labeled_ta(s_form_llm, &s_ta_api_key, "LLM API Key", y, true);
    y = create_labeled_ta(s_form_llm, &s_ta_model, "Model", y, false);
    create_action_btn(s_form_llm, "Save LLM", UI_MARGIN, y, 100, llm_save_cb);
}

static void create_feishu_screen(void)
{
    s_scr_feishu = create_subscreen(&s_status_feishu, &s_form_feishu);
    int y = UI_MARGIN;
    y = create_labeled_ta(s_form_feishu, &s_ta_feishu_app_id, "Feishu App ID", y, false);
    y = create_labeled_ta(s_form_feishu, &s_ta_feishu_app_secret, "Feishu App Secret", y, true);
    y = create_labeled_ta(s_form_feishu, &s_ta_feishu_chat_id, "Default Chat ID", y, false);
    create_action_btn(s_form_feishu, "Save Feishu", UI_MARGIN, y, 110, feishu_save_cb);
    create_action_btn(s_form_feishu, "Test Send", UI_MARGIN + 120, y, 100, feishu_test_cb);
}

static void create_search_screen(void)
{
    s_scr_search = create_subscreen(&s_status_search, &s_form_search);
    int y = UI_MARGIN;
    y = create_labeled_ta(s_form_search, &s_ta_search_key, "Search API Key", y, true);
    create_action_btn(s_form_search, "Save Search", UI_MARGIN, y, 110, search_save_cb);
}

static void create_system_screen(void)
{
    s_scr_system = create_subscreen(&s_status_system, &s_form_system);

    lv_obj_t *tip = lv_label_create(s_form_system, NULL);
    lv_label_set_long_mode(tip, LV_LABEL_LONG_BREAK);
    lv_label_set_text(tip, "Changes are stored in NVS.\nRestart to apply all services.");
    lv_obj_set_width(tip, MIMI_LCD_HOR_RES - 2 * UI_MARGIN - 8);
    lv_obj_align(tip, NULL, LV_ALIGN_IN_TOP_LEFT, UI_MARGIN, UI_MARGIN + 6);

    create_action_btn(s_form_system, "Restart Device", UI_MARGIN, 78, 130, restart_cb);
    create_action_btn(s_form_system, "Audio Panel", UI_MARGIN + 140, 78, 120, system_audio_cb);
}

static void create_audio_screen(void)
{
    s_scr_audio = create_subscreen(&s_status_audio, &s_form_audio);

    lv_obj_t *tip = lv_label_create(s_form_audio, NULL);
    lv_label_set_text(tip, "Record to SD and tap file to play");
    lv_obj_align(tip, NULL, LV_ALIGN_IN_TOP_LEFT, UI_MARGIN, UI_MARGIN);

    s_audio_vol_label = lv_label_create(s_form_audio, NULL);
    lv_obj_align(s_audio_vol_label, NULL, LV_ALIGN_IN_TOP_LEFT, UI_MARGIN, UI_MARGIN + 18);

    s_audio_vol_slider = lv_slider_create(s_form_audio, NULL);
    lv_obj_set_width(s_audio_vol_slider, SUB_FIELD_W);
    lv_obj_align(s_audio_vol_slider, NULL, LV_ALIGN_IN_TOP_LEFT, UI_MARGIN, UI_MARGIN + 34);
    lv_slider_set_range(s_audio_vol_slider, 5, 100);
    lv_obj_set_event_cb(s_audio_vol_slider, audio_vol_slider_cb);
    int vol = audio_service_get_playback_volume_percent();
    if (vol < 5) vol = 5;
    if (vol > 100) vol = 100;
    lv_slider_set_value(s_audio_vol_slider, vol, LV_ANIM_OFF);
    audio_update_volume_label(vol);

    int y = UI_MARGIN + 56;
    create_action_btn(s_form_audio, "Start Rec", UI_MARGIN, y, 92, audio_start_rec_cb);
    create_action_btn(s_form_audio, "Stop Rec", UI_MARGIN + 98, y, 92, audio_stop_rec_cb);
    create_action_btn(s_form_audio, "Refresh", UI_MARGIN + 196, y, 92, audio_refresh_cb);

    s_audio_list = lv_list_create(s_form_audio, NULL);
    lv_obj_set_size(s_audio_list, SUB_FIELD_W, 116);
    lv_obj_align(s_audio_list, NULL, LV_ALIGN_IN_TOP_LEFT, UI_MARGIN, y + 36);
    lv_list_set_layout(s_audio_list, LV_LAYOUT_COLUMN_LEFT);

    audio_refresh_file_list();
}

static void create_ui(void)
{
    create_home_screen();
    create_wifi_screen();
    create_proxy_screen();
    create_llm_screen();
    create_feishu_screen();
    create_search_screen();
    create_system_screen();
    create_audio_screen();
    load_values_to_widgets();
}

static void open_home(void)
{
    show_screen(s_scr_home, NULL, NULL);
    update_home_status_labels();
}

static void open_wifi(void)   { show_screen(s_scr_wifi, s_status_wifi, s_form_wifi); }
static void open_proxy(void)  { show_screen(s_scr_proxy, s_status_proxy, s_form_proxy); }
static void open_llm(void)    { show_screen(s_scr_llm, s_status_llm, s_form_llm); }
static void open_feishu(void) { show_screen(s_scr_feishu, s_status_feishu, s_form_feishu); }
static void open_search(void) { show_screen(s_scr_search, s_status_search, s_form_search); }
static void open_system(void) { show_screen(s_scr_system, s_status_system, s_form_system); }
static void open_audio(void)
{
    show_screen(s_scr_audio, s_status_audio, s_form_audio);
    if (s_audio_vol_slider) {
        int vol = audio_service_get_playback_volume_percent();
        if (vol < 5) vol = 5;
        if (vol > 100) vol = 100;
        lv_slider_set_value(s_audio_vol_slider, vol, LV_ANIM_OFF);
        audio_update_volume_label(vol);
    }
    audio_refresh_file_list();
}

static void config_ui_task(void *arg)
{
    (void)arg;

    if (display_port_init() != ESP_OK) {
        ESP_LOGE(TAG, "display init failed");
        s_ui_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    create_ui();
    open_home();

    uint32_t loop = 0;
    while (1) {
        lv_task_handler();
        if ((loop++ % 100) == 0) {
            update_home_status_labels();
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

esp_err_t config_ui_start(void)
{
    if (s_ui_task) {
        return ESP_OK;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(
        config_ui_task,
        "config_ui",
        8192,
        NULL,
        4,
        &s_ui_task,
        1);
    return ok == pdPASS ? ESP_OK : ESP_FAIL;
}
