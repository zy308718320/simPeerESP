idf_component_register(SRCS "server_api.c" "wifi_smartconfig.c" "main.c" "sms.c"
                       PRIV_REQUIRES driver nvs_flash esp_wifi esp_netif esp_http_client json esp_event mbedtls
                       INCLUDE_DIRS ".")
