#pragma once

#include <stdbool.h>
#include "esp_err.h"

/*
 * Feishu integration:
 * - outbound via webhook or app-bot im/v1/messages
 * - inbound via long websocket connection (event subscription)
 */

esp_err_t feishu_bot_init(void);
esp_err_t feishu_bot_start(void);
esp_err_t feishu_bot_stop(void);

esp_err_t feishu_bot_set_webhook(const char *webhook_url);
esp_err_t feishu_bot_clear_webhook(void);
const char *feishu_bot_get_webhook(void);

esp_err_t feishu_bot_set_app_credentials(const char *app_id, const char *app_secret);
esp_err_t feishu_bot_clear_app_credentials(void);
bool feishu_bot_has_app_credentials(void);
const char *feishu_bot_get_app_id(void);

esp_err_t feishu_bot_set_default_chat_id(const char *chat_id);
esp_err_t feishu_bot_clear_default_chat_id(void);
const char *feishu_bot_get_default_chat_id(void);

bool feishu_bot_is_configured(void);
esp_err_t feishu_bot_send_message(const char *text);
esp_err_t feishu_bot_send_message_to(const char *chat_id, const char *text);
