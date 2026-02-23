idf_component_register(
    SRCS
        "mimi.c"
        "bus/message_bus.c"
        "wifi/wifi_manager.c"
        "telegram/telegram_bot.c"
        "feishu/feishu_bot.c"
        "llm/llm_proxy.c"
        "agent/agent_loop.c"
        "agent/context_builder.c"
        "memory/memory_store.c"
        "memory/session_mgr.c"
        "gateway/ws_server.c"
        "cli/serial_cli.c"
        "ota/ota_manager.c"
        "proxy/http_proxy.c"
        "tools/tool_registry.c"
        "tools/tool_web_search.c"
        "tools/tool_web_fetch.c"
        "tools/tool_get_time.c"
        "tools/tool_files.c"
        "ui/display_port.c"
        "ui/xpt2046.c"
        "ui/config_ui.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        nvs_flash esp_wifi esp_netif esp_http_client esp_http_server
        esp_https_ota esp_event json spiffs console vfs app_update esp-tls
        esp_timer esp_driver_gpio esp_driver_spi lvgl_component tcp_transport
)
