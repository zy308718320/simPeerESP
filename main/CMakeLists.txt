idf_component_register(SRCS "feishu_api.c" "wifi_smartconfig.c" "main.c" "sms.c"
                       PRIV_REQUIRES esp_driver_uart esp_driver_gpio nvs_flash esp_wifi esp_netif esp_http_client esp_event mbedtls lwip
                       INCLUDE_DIRS ".")

# 敏感配置检查：secrets.h 不入库，缺少时给出明确提示
if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/secrets.h")
    message(FATAL_ERROR
        "缺少 main/secrets.h —— 请执行: cp main/secrets.h.example main/secrets.h "
        "并填入真实的飞书 Webhook 地址（该文件已被 .gitignore 忽略）")
endif()
