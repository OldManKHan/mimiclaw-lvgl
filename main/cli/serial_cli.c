#include "serial_cli.h"
#include "mimi_config.h"
#include "bus/message_bus.h"
#include "wifi/wifi_manager.h"
#include "telegram/telegram_bot.h"
#include "feishu/feishu_bot.h"
#include "llm/llm_proxy.h"
#include "memory/memory_store.h"
#include "memory/session_mgr.h"
#include "proxy/http_proxy.h"
#include "tools/tool_web_search.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_console.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "argtable3/argtable3.h"

static const char *TAG = "cli";

/* --- wifi_set command --- */
static struct {
    struct arg_str *ssid;
    struct arg_str *password;
    struct arg_end *end;
} wifi_set_args;

static int cmd_wifi_set(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&wifi_set_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, wifi_set_args.end, argv[0]);
        return 1;
    }
    wifi_manager_set_credentials(wifi_set_args.ssid->sval[0],
                                  wifi_set_args.password->sval[0]);
    printf("WiFi credentials saved. Restart to apply.\n");
    return 0;
}

/* --- wifi_status command --- */
static int cmd_wifi_status(int argc, char **argv)
{
    printf("WiFi connected: %s\n", wifi_manager_is_connected() ? "yes" : "no");
    printf("IP: %s\n", wifi_manager_get_ip());
    return 0;
}

/* --- set_tg_token command --- */
static struct {
    struct arg_str *token;
    struct arg_end *end;
} tg_token_args;

static int cmd_set_tg_token(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&tg_token_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, tg_token_args.end, argv[0]);
        return 1;
    }
    telegram_set_token(tg_token_args.token->sval[0]);
    printf("Telegram bot token saved.\n");
    return 0;
}

/* --- set_feishu_webhook command --- */
static struct {
    struct arg_str *url;
    struct arg_end *end;
} feishu_webhook_args;

static int cmd_set_feishu_webhook(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&feishu_webhook_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, feishu_webhook_args.end, argv[0]);
        return 1;
    }
    feishu_bot_set_webhook(feishu_webhook_args.url->sval[0]);
    printf("Feishu webhook saved.\n");
    return 0;
}

/* --- set_feishu_app command --- */
static struct {
    struct arg_str *app_id;
    struct arg_str *app_secret;
    struct arg_end *end;
} feishu_app_args;

static int cmd_set_feishu_app(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&feishu_app_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, feishu_app_args.end, argv[0]);
        return 1;
    }
    esp_err_t ret = feishu_bot_set_app_credentials(feishu_app_args.app_id->sval[0],
                                                    feishu_app_args.app_secret->sval[0]);
    if (ret == ESP_OK) {
        feishu_bot_start();
    }
    printf("Feishu app config: %s\n", ret == ESP_OK ? "saved" : "failed");
    return ret == ESP_OK ? 0 : 1;
}

/* --- clear_feishu_app command --- */
static int cmd_clear_feishu_app(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    esp_err_t ret = feishu_bot_clear_app_credentials();
    printf("Feishu app config: %s\n", ret == ESP_OK ? "cleared" : "failed");
    return ret == ESP_OK ? 0 : 1;
}

/* --- set_feishu_chat command --- */
static struct {
    struct arg_str *chat_id;
    struct arg_end *end;
} feishu_chat_id_args;

static int cmd_set_feishu_chat(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&feishu_chat_id_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, feishu_chat_id_args.end, argv[0]);
        return 1;
    }
    esp_err_t ret = feishu_bot_set_default_chat_id(feishu_chat_id_args.chat_id->sval[0]);
    printf("Feishu default chat_id: %s\n", ret == ESP_OK ? "saved" : "failed");
    return ret == ESP_OK ? 0 : 1;
}

/* --- clear_feishu_chat command --- */
static int cmd_clear_feishu_chat(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    esp_err_t ret = feishu_bot_clear_default_chat_id();
    printf("Feishu default chat_id: %s\n", ret == ESP_OK ? "cleared" : "failed");
    return ret == ESP_OK ? 0 : 1;
}

/* --- clear_feishu_webhook command --- */
static int cmd_clear_feishu_webhook(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    feishu_bot_clear_webhook();
    printf("Feishu webhook cleared.\n");
    return 0;
}

/* --- feishu_send command --- */
static struct {
    struct arg_str *chat_id;
    struct arg_str *text;
    struct arg_end *end;
} feishu_send_args;

static int cmd_feishu_send(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&feishu_send_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, feishu_send_args.end, argv[0]);
        return 1;
    }
    esp_err_t ret = feishu_bot_send_message_to(feishu_send_args.chat_id->sval[0],
                                               feishu_send_args.text->sval[0]);
    printf("Feishu send: %s\n", ret == ESP_OK ? "ok" : "failed");
    return ret == ESP_OK ? 0 : 1;
}

/* --- feishu_chat command --- */
static struct {
    struct arg_str *text;
    struct arg_end *end;
} feishu_chat_args;

static int cmd_feishu_chat(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&feishu_chat_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, feishu_chat_args.end, argv[0]);
        return 1;
    }

    mimi_msg_t msg = {0};
    const char *chat_id = feishu_bot_get_default_chat_id();
    if (!chat_id || !chat_id[0]) {
        chat_id = "feishu_default";
    }
    strncpy(msg.channel, MIMI_CHAN_FEISHU, sizeof(msg.channel) - 1);
    strncpy(msg.chat_id, chat_id, sizeof(msg.chat_id) - 1);
    msg.content = strdup(feishu_chat_args.text->sval[0]);
    if (!msg.content) {
        printf("Out of memory.\n");
        return 1;
    }

    esp_err_t ret = message_bus_push_inbound(&msg);
    if (ret != ESP_OK) {
        free(msg.content);
        printf("Queue full.\n");
        return 1;
    }

    printf("Queued message to agent (channel=feishu).\n");
    return 0;
}

/* --- set_api_key command --- */
static struct {
    struct arg_str *key;
    struct arg_end *end;
} api_key_args;

static int cmd_set_api_key(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&api_key_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, api_key_args.end, argv[0]);
        return 1;
    }
    llm_set_api_key(api_key_args.key->sval[0]);
    printf("API key saved.\n");
    return 0;
}

/* --- set_model command --- */
static struct {
    struct arg_str *model;
    struct arg_end *end;
} model_args;

static int cmd_set_model(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&model_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, model_args.end, argv[0]);
        return 1;
    }
    llm_set_model(model_args.model->sval[0]);
    printf("Model set.\n");
    return 0;
}

/* --- memory_read command --- */
static int cmd_memory_read(int argc, char **argv)
{
    char *buf = malloc(4096);
    if (!buf) {
        printf("Out of memory.\n");
        return 1;
    }
    if (memory_read_long_term(buf, 4096) == ESP_OK && buf[0]) {
        printf("=== MEMORY.md ===\n%s\n=================\n", buf);
    } else {
        printf("MEMORY.md is empty or not found.\n");
    }
    free(buf);
    return 0;
}

/* --- memory_write command --- */
static struct {
    struct arg_str *content;
    struct arg_end *end;
} memory_write_args;

static int cmd_memory_write(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&memory_write_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, memory_write_args.end, argv[0]);
        return 1;
    }
    memory_write_long_term(memory_write_args.content->sval[0]);
    printf("MEMORY.md updated.\n");
    return 0;
}

/* --- session_list command --- */
static int cmd_session_list(int argc, char **argv)
{
    printf("Sessions:\n");
    session_list();
    return 0;
}

/* --- session_clear command --- */
static struct {
    struct arg_str *chat_id;
    struct arg_end *end;
} session_clear_args;

static int cmd_session_clear(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&session_clear_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, session_clear_args.end, argv[0]);
        return 1;
    }
    if (session_clear(session_clear_args.chat_id->sval[0]) == ESP_OK) {
        printf("Session cleared.\n");
    } else {
        printf("Session not found.\n");
    }
    return 0;
}

/* --- heap_info command --- */
static int cmd_heap_info(int argc, char **argv)
{
    printf("Internal free: %d bytes\n",
           (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    printf("PSRAM free:    %d bytes\n",
           (int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    printf("Total free:    %d bytes\n",
           (int)esp_get_free_heap_size());
    return 0;
}

/* --- set_proxy command --- */
static struct {
    struct arg_str *host;
    struct arg_int *port;
    struct arg_end *end;
} proxy_args;

static int cmd_set_proxy(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&proxy_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, proxy_args.end, argv[0]);
        return 1;
    }
    http_proxy_set(proxy_args.host->sval[0], (uint16_t)proxy_args.port->ival[0]);
    printf("Proxy set. Restart to apply.\n");
    return 0;
}

/* --- clear_proxy command --- */
static int cmd_clear_proxy(int argc, char **argv)
{
    http_proxy_clear();
    printf("Proxy cleared. Restart to apply.\n");
    return 0;
}

/* --- set_search_key command --- */
static struct {
    struct arg_str *key;
    struct arg_end *end;
} search_key_args;

static int cmd_set_search_key(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&search_key_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, search_key_args.end, argv[0]);
        return 1;
    }
    tool_web_search_set_key(search_key_args.key->sval[0]);
    printf("Search API key saved.\n");
    return 0;
}

/* --- config_show command --- */
static void print_config(const char *label, const char *ns, const char *key,
                         const char *build_val, bool mask)
{
    char nvs_val[128] = {0};
    const char *source = "not set";
    const char *display = "(empty)";

    /* NVS takes highest priority */
    nvs_handle_t nvs;
    if (nvs_open(ns, NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = sizeof(nvs_val);
        if (nvs_get_str(nvs, key, nvs_val, &len) == ESP_OK && nvs_val[0]) {
            source = "NVS";
            display = nvs_val;
        }
        nvs_close(nvs);
    }

    /* Fall back to build-time value */
    if (strcmp(source, "not set") == 0 && build_val[0] != '\0') {
        source = "build";
        display = build_val;
    }

    if (mask && strlen(display) > 6 && strcmp(display, "(empty)") != 0) {
        printf("  %-14s: %.4s****  [%s]\n", label, display, source);
    } else {
        printf("  %-14s: %s  [%s]\n", label, display, source);
    }
}

static int cmd_config_show(int argc, char **argv)
{
    printf("=== Current Configuration ===\n");
    print_config("WiFi SSID",  MIMI_NVS_WIFI,   MIMI_NVS_KEY_SSID,     MIMI_SECRET_WIFI_SSID,  false);
    print_config("WiFi Pass",  MIMI_NVS_WIFI,   MIMI_NVS_KEY_PASS,     MIMI_SECRET_WIFI_PASS,  true);
    print_config("TG Token",   MIMI_NVS_TG,     MIMI_NVS_KEY_TG_TOKEN, MIMI_SECRET_TG_TOKEN,   true);
    print_config("API Key",    MIMI_NVS_LLM,    MIMI_NVS_KEY_API_KEY,  MIMI_SECRET_API_KEY,    true);
    print_config("Model",      MIMI_NVS_LLM,    MIMI_NVS_KEY_MODEL,    MIMI_SECRET_MODEL,      false);
    print_config("Proxy Host", MIMI_NVS_PROXY,  MIMI_NVS_KEY_PROXY_HOST, MIMI_SECRET_PROXY_HOST, false);
    print_config("Proxy Port", MIMI_NVS_PROXY,  MIMI_NVS_KEY_PROXY_PORT, MIMI_SECRET_PROXY_PORT, false);
    print_config("Search Key", MIMI_NVS_SEARCH, MIMI_NVS_KEY_API_KEY,  MIMI_SECRET_SEARCH_KEY, true);
    print_config("Feishu Hook", MIMI_NVS_FEISHU, MIMI_NVS_KEY_FEISHU_WEBHOOK, MIMI_SECRET_FEISHU_WEBHOOK, true);
    print_config("Feishu AppID", MIMI_NVS_FEISHU, MIMI_NVS_KEY_FEISHU_APP_ID, MIMI_SECRET_FEISHU_APP_ID, true);
    print_config("Feishu Secret", MIMI_NVS_FEISHU, MIMI_NVS_KEY_FEISHU_APP_SECRET, MIMI_SECRET_FEISHU_APP_SECRET, true);
    print_config("Feishu ChatID", MIMI_NVS_FEISHU, MIMI_NVS_KEY_FEISHU_DEF_CHAT, MIMI_SECRET_FEISHU_DEFAULT_CHAT_ID, false);
    printf("=============================\n");
    return 0;
}

/* --- config_reset command --- */
static int cmd_config_reset(int argc, char **argv)
{
    const char *namespaces[] = {
        MIMI_NVS_WIFI, MIMI_NVS_TG, MIMI_NVS_LLM, MIMI_NVS_PROXY, MIMI_NVS_SEARCH, MIMI_NVS_FEISHU
    };
    for (int i = 0; i < 6; i++) {
        nvs_handle_t nvs;
        if (nvs_open(namespaces[i], NVS_READWRITE, &nvs) == ESP_OK) {
            nvs_erase_all(nvs);
            nvs_commit(nvs);
            nvs_close(nvs);
        }
    }
    printf("All NVS config cleared. Build-time defaults will be used on restart.\n");
    return 0;
}

/* --- restart command --- */
static int cmd_restart(int argc, char **argv)
{
    printf("Restarting...\n");
    esp_restart();
    return 0;  /* unreachable */
}

esp_err_t serial_cli_init(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "mimi> ";
    repl_config.max_cmdline_length = 256;

    /* USB Serial JTAG */
    esp_console_dev_usb_serial_jtag_config_t hw_config =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();

    esp_err_t ret = esp_console_new_repl_usb_serial_jtag(&hw_config, &repl_config, &repl);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "USB Serial JTAG REPL init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Register commands */
    esp_console_register_help_command();

    /* wifi_set */
    wifi_set_args.ssid = arg_str1(NULL, NULL, "<ssid>", "WiFi SSID");
    wifi_set_args.password = arg_str1(NULL, NULL, "<password>", "WiFi password");
    wifi_set_args.end = arg_end(2);
    esp_console_cmd_t wifi_set_cmd = {
        .command = "wifi_set",
        .help = "Set WiFi SSID and password",
        .func = &cmd_wifi_set,
        .argtable = &wifi_set_args,
    };
    esp_console_cmd_register(&wifi_set_cmd);

    /* wifi_status */
    esp_console_cmd_t wifi_status_cmd = {
        .command = "wifi_status",
        .help = "Show WiFi connection status",
        .func = &cmd_wifi_status,
    };
    esp_console_cmd_register(&wifi_status_cmd);

    /* set_tg_token */
    tg_token_args.token = arg_str1(NULL, NULL, "<token>", "Telegram bot token");
    tg_token_args.end = arg_end(1);
    esp_console_cmd_t tg_token_cmd = {
        .command = "set_tg_token",
        .help = "Set Telegram bot token",
        .func = &cmd_set_tg_token,
        .argtable = &tg_token_args,
    };
    esp_console_cmd_register(&tg_token_cmd);

    /* set_feishu_webhook */
    feishu_webhook_args.url = arg_str1(NULL, NULL, "<url>", "Feishu custom bot webhook URL");
    feishu_webhook_args.end = arg_end(1);
    esp_console_cmd_t feishu_webhook_cmd = {
        .command = "set_feishu_webhook",
        .help = "Set Feishu custom bot webhook",
        .func = &cmd_set_feishu_webhook,
        .argtable = &feishu_webhook_args,
    };
    esp_console_cmd_register(&feishu_webhook_cmd);

    /* set_feishu_app */
    feishu_app_args.app_id = arg_str1(NULL, NULL, "<app_id>", "Feishu app id");
    feishu_app_args.app_secret = arg_str1(NULL, NULL, "<app_secret>", "Feishu app secret");
    feishu_app_args.end = arg_end(2);
    esp_console_cmd_t feishu_app_cmd = {
        .command = "set_feishu_app",
        .help = "Set Feishu app credentials for long connection",
        .func = &cmd_set_feishu_app,
        .argtable = &feishu_app_args,
    };
    esp_console_cmd_register(&feishu_app_cmd);

    /* clear_feishu_app */
    esp_console_cmd_t clear_feishu_app_cmd = {
        .command = "clear_feishu_app",
        .help = "Clear Feishu app credentials",
        .func = &cmd_clear_feishu_app,
    };
    esp_console_cmd_register(&clear_feishu_app_cmd);

    /* set_feishu_chat */
    feishu_chat_id_args.chat_id = arg_str1(NULL, NULL, "<chat_id>", "Default Feishu chat_id for test sends");
    feishu_chat_id_args.end = arg_end(1);
    esp_console_cmd_t feishu_chat_id_cmd = {
        .command = "set_feishu_chat",
        .help = "Set default Feishu chat_id",
        .func = &cmd_set_feishu_chat,
        .argtable = &feishu_chat_id_args,
    };
    esp_console_cmd_register(&feishu_chat_id_cmd);

    /* clear_feishu_chat */
    esp_console_cmd_t clear_feishu_chat_cmd = {
        .command = "clear_feishu_chat",
        .help = "Clear default Feishu chat_id",
        .func = &cmd_clear_feishu_chat,
    };
    esp_console_cmd_register(&clear_feishu_chat_cmd);

    /* clear_feishu_webhook */
    esp_console_cmd_t clear_feishu_cmd = {
        .command = "clear_feishu_webhook",
        .help = "Clear Feishu custom bot webhook",
        .func = &cmd_clear_feishu_webhook,
    };
    esp_console_cmd_register(&clear_feishu_cmd);

    /* feishu_send */
    feishu_send_args.chat_id = arg_str1(NULL, NULL, "<chat_id>", "Feishu chat_id");
    feishu_send_args.text = arg_str1(NULL, NULL, "<text>", "Text message to send to Feishu");
    feishu_send_args.end = arg_end(2);
    esp_console_cmd_t feishu_send_cmd = {
        .command = "feishu_send",
        .help = "Send a test text message via Feishu im/v1/messages",
        .func = &cmd_feishu_send,
        .argtable = &feishu_send_args,
    };
    esp_console_cmd_register(&feishu_send_cmd);

    /* feishu_chat */
    feishu_chat_args.text = arg_str1(NULL, NULL, "<text>", "Ask agent then send response to Feishu");
    feishu_chat_args.end = arg_end(1);
    esp_console_cmd_t feishu_chat_cmd = {
        .command = "feishu_chat",
        .help = "Push a feishu-channel inbound message to agent loop",
        .func = &cmd_feishu_chat,
        .argtable = &feishu_chat_args,
    };
    esp_console_cmd_register(&feishu_chat_cmd);

    /* set_api_key */
    api_key_args.key = arg_str1(NULL, NULL, "<key>", "Anthropic API key");
    api_key_args.end = arg_end(1);
    esp_console_cmd_t api_key_cmd = {
        .command = "set_api_key",
        .help = "Set Claude API key",
        .func = &cmd_set_api_key,
        .argtable = &api_key_args,
    };
    esp_console_cmd_register(&api_key_cmd);

    /* set_model */
    model_args.model = arg_str1(NULL, NULL, "<model>", "Model identifier");
    model_args.end = arg_end(1);
    esp_console_cmd_t model_cmd = {
        .command = "set_model",
        .help = "Set LLM model (default: " MIMI_LLM_DEFAULT_MODEL ")",
        .func = &cmd_set_model,
        .argtable = &model_args,
    };
    esp_console_cmd_register(&model_cmd);

    /* memory_read */
    esp_console_cmd_t mem_read_cmd = {
        .command = "memory_read",
        .help = "Read MEMORY.md",
        .func = &cmd_memory_read,
    };
    esp_console_cmd_register(&mem_read_cmd);

    /* memory_write */
    memory_write_args.content = arg_str1(NULL, NULL, "<content>", "Content to write");
    memory_write_args.end = arg_end(1);
    esp_console_cmd_t mem_write_cmd = {
        .command = "memory_write",
        .help = "Write to MEMORY.md",
        .func = &cmd_memory_write,
        .argtable = &memory_write_args,
    };
    esp_console_cmd_register(&mem_write_cmd);

    /* session_list */
    esp_console_cmd_t sess_list_cmd = {
        .command = "session_list",
        .help = "List all sessions",
        .func = &cmd_session_list,
    };
    esp_console_cmd_register(&sess_list_cmd);

    /* session_clear */
    session_clear_args.chat_id = arg_str1(NULL, NULL, "<chat_id>", "Chat ID to clear");
    session_clear_args.end = arg_end(1);
    esp_console_cmd_t sess_clear_cmd = {
        .command = "session_clear",
        .help = "Clear a session",
        .func = &cmd_session_clear,
        .argtable = &session_clear_args,
    };
    esp_console_cmd_register(&sess_clear_cmd);

    /* heap_info */
    esp_console_cmd_t heap_cmd = {
        .command = "heap_info",
        .help = "Show heap memory usage",
        .func = &cmd_heap_info,
    };
    esp_console_cmd_register(&heap_cmd);

    /* set_search_key */
    search_key_args.key = arg_str1(NULL, NULL, "<key>", "Search API key (Brave or Tavily)");
    search_key_args.end = arg_end(1);
    esp_console_cmd_t search_key_cmd = {
        .command = "set_search_key",
        .help = "Set search API key for web_search tool (Brave/Tavily)",
        .func = &cmd_set_search_key,
        .argtable = &search_key_args,
    };
    esp_console_cmd_register(&search_key_cmd);

    /* set_proxy */
    proxy_args.host = arg_str1(NULL, NULL, "<host>", "Proxy host/IP");
    proxy_args.port = arg_int1(NULL, NULL, "<port>", "Proxy port");
    proxy_args.end = arg_end(2);
    esp_console_cmd_t proxy_cmd = {
        .command = "set_proxy",
        .help = "Set HTTP proxy (e.g. set_proxy 192.168.1.83 7897)",
        .func = &cmd_set_proxy,
        .argtable = &proxy_args,
    };
    esp_console_cmd_register(&proxy_cmd);

    /* clear_proxy */
    esp_console_cmd_t clear_proxy_cmd = {
        .command = "clear_proxy",
        .help = "Remove proxy configuration",
        .func = &cmd_clear_proxy,
    };
    esp_console_cmd_register(&clear_proxy_cmd);

    /* config_show */
    esp_console_cmd_t config_show_cmd = {
        .command = "config_show",
        .help = "Show current configuration (build-time + NVS)",
        .func = &cmd_config_show,
    };
    esp_console_cmd_register(&config_show_cmd);

    /* config_reset */
    esp_console_cmd_t config_reset_cmd = {
        .command = "config_reset",
        .help = "Clear all NVS overrides, revert to build-time defaults",
        .func = &cmd_config_reset,
    };
    esp_console_cmd_register(&config_reset_cmd);

    /* restart */
    esp_console_cmd_t restart_cmd = {
        .command = "restart",
        .help = "Restart the device",
        .func = &cmd_restart,
    };
    esp_console_cmd_register(&restart_cmd);

    /* Start REPL */
    ret = esp_console_start_repl(repl);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Serial CLI start failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "Serial CLI started");

    return ESP_OK;
}
