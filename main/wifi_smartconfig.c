/**
 * @file wifi_smartconfig.c
 * @brief ESP32 SmartConfig WiFi 配网模块实现
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_smartconfig.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "wifi_smartconfig.h"

static const char *TAG = "SMARTCONFIG";

/* 事件组标志位 */
#define WIFI_CONNECTED_BIT    BIT0  // WiFi 已连接
#define WIFI_FAIL_BIT         BIT1  // WiFi 连接失败
#define SMARTCONFIG_DONE_BIT  BIT2  // SmartConfig 完成

/* 静态变量 */
static EventGroupHandle_t s_wifi_event_group = NULL;
static wifi_connected_callback_t s_connected_callback = NULL;
static bool s_is_connected = false;
static int s_retry_num = 0;
static const int MAX_RETRY = 3;  // WiFi 连接最大重试次数

/**
 * @brief WiFi 和 SmartConfig 事件处理函数
 */
static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG, "WiFi STA 启动");
                break;
            case WIFI_EVENT_STA_DISCONNECTED:
                s_is_connected = false;
                if (s_retry_num < MAX_RETRY) {
                    esp_wifi_connect();
                    s_retry_num++;
                    ESP_LOGI(TAG, "重试连接 WiFi... (%d/%d)", s_retry_num, MAX_RETRY);
                } else {
                    ESP_LOGW(TAG, "WiFi 连接失败，启动 SmartConfig 配网");
                    xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
                }
                break;
            case WIFI_EVENT_STA_CONNECTED:
                ESP_LOGI(TAG, "WiFi 已连接到 AP");
                break;
            default:
                break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "获取到 IP 地址: " IPSTR, IP2STR(&event->ip_info.ip));
        s_is_connected = true;
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        
        // 调用连接成功回调
        if (s_connected_callback) {
            s_connected_callback();
        }
    } else if (event_base == SC_EVENT) {
        switch (event_id) {
            case SC_EVENT_SCAN_DONE:
                ESP_LOGI(TAG, "SmartConfig 扫描完成");
                break;
            case SC_EVENT_FOUND_CHANNEL:
                ESP_LOGI(TAG, "SmartConfig 找到信道");
                break;
            case SC_EVENT_GOT_SSID_PSWD: {
                ESP_LOGI(TAG, "SmartConfig 获取到 SSID 和密码");
                smartconfig_event_got_ssid_pswd_t *evt = (smartconfig_event_got_ssid_pswd_t *)event_data;
                
                wifi_config_t wifi_config;
                memset(&wifi_config, 0, sizeof(wifi_config_t));
                memcpy(wifi_config.sta.ssid, evt->ssid, sizeof(wifi_config.sta.ssid));
                memcpy(wifi_config.sta.password, evt->password, sizeof(wifi_config.sta.password));
                wifi_config.sta.bssid_set = evt->bssid_set;
                if (wifi_config.sta.bssid_set) {
                    memcpy(wifi_config.sta.bssid, evt->bssid, sizeof(wifi_config.sta.bssid));
                }
                
                ESP_LOGI(TAG, "SSID: %s", (char *)evt->ssid);
                ESP_LOGI(TAG, "PASSWORD: %s", (char *)evt->password);
                
                // 保存 WiFi 配置到 NVS
                ESP_ERROR_CHECK(esp_wifi_disconnect());
                ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
                
                s_retry_num = 0;  // 重置重试计数
                esp_wifi_connect();
                break;
            }
            case SC_EVENT_SEND_ACK_DONE:
                ESP_LOGI(TAG, "SmartConfig ACK 已发送");
                xEventGroupSetBits(s_wifi_event_group, SMARTCONFIG_DONE_BIT);
                break;
            default:
                break;
        }
    }
}

/**
 * @brief SmartConfig 配网任务
 */
static void smartconfig_task(void *parm)
{
    EventBits_t uxBits;
    
    ESP_LOGI(TAG, "开始 SmartConfig 配网...");
    ESP_LOGI(TAG, "请使用 ESPTouch App 进行配网");
    
    // 设置 SmartConfig 类型（支持 ESPTouch 和 AirKiss）
    ESP_ERROR_CHECK(esp_smartconfig_set_type(SC_TYPE_ESPTOUCH_AIRKISS));
    
    // 配置 SmartConfig
    smartconfig_start_config_t cfg = SMARTCONFIG_START_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_smartconfig_start(&cfg));
    
    while (1) {
        uxBits = xEventGroupWaitBits(s_wifi_event_group,
                                     WIFI_CONNECTED_BIT | SMARTCONFIG_DONE_BIT,
                                     pdTRUE, pdFALSE, portMAX_DELAY);
        
        if (uxBits & WIFI_CONNECTED_BIT) {
            ESP_LOGI(TAG, "WiFi 连接成功");
        }
        if (uxBits & SMARTCONFIG_DONE_BIT) {
            ESP_LOGI(TAG, "SmartConfig 配网完成");
            esp_smartconfig_stop();
            vTaskDelete(NULL);
            return;
        }
    }
}

/**
 * @brief 初始化 NVS
 */
static esp_err_t init_nvs(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    return ret;
}

/**
 * @brief 初始化 WiFi（STA 模式）
 */
static esp_err_t init_wifi(void)
{
    // 初始化 TCP/IP 栈
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    
    // 初始化 WiFi
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    
    // 注册事件处理函数
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(SC_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    
    // 设置 WiFi 模式
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    
    return ESP_OK;
}

/**
 * @brief 检查是否有保存的 WiFi 配置
 */
static bool has_saved_wifi_config(void)
{
    wifi_config_t wifi_config;
    esp_err_t err = esp_wifi_get_config(WIFI_IF_STA, &wifi_config);
    if (err != ESP_OK) {
        return false;
    }
    return strlen((char *)wifi_config.sta.ssid) > 0;
}

bool wifi_smartconfig_init(wifi_connected_callback_t on_connected)
{
    s_connected_callback = on_connected;
    
    // 创建事件组
    s_wifi_event_group = xEventGroupCreate();
    if (s_wifi_event_group == NULL) {
        ESP_LOGE(TAG, "创建事件组失败");
        return false;
    }
    
    // 初始化 NVS
    if (init_nvs() != ESP_OK) {
        ESP_LOGE(TAG, "NVS 初始化失败");
        return false;
    }
    
    // 初始化 WiFi
    if (init_wifi() != ESP_OK) {
        ESP_LOGE(TAG, "WiFi 初始化失败");
        return false;
    }
    
    // 启动 WiFi
    ESP_ERROR_CHECK(esp_wifi_start());
    
    // 检查是否有保存的 WiFi 配置
    if (has_saved_wifi_config()) {
        ESP_LOGI(TAG, "发现保存的 WiFi 配置，尝试连接...");
        esp_wifi_connect();
        
        // 等待连接结果
        EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                               WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                               pdFALSE, pdFALSE,
                                               pdMS_TO_TICKS(15000));  // 15秒超时
        
        if (bits & WIFI_CONNECTED_BIT) {
            ESP_LOGI(TAG, "使用保存的配置连接 WiFi 成功");
            return true;
        } else {
            ESP_LOGW(TAG, "使用保存的配置连接失败，启动 SmartConfig");
            xEventGroupClearBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else {
        ESP_LOGI(TAG, "未找到保存的 WiFi 配置，启动 SmartConfig");
    }
    
    // 启动 SmartConfig 任务
    s_retry_num = 0;  // 重置重试计数
    xTaskCreate(smartconfig_task, "smartconfig_task", 4096, NULL, 3, NULL);
    
    return true;
}

bool wifi_smartconfig_wait_connected(uint32_t timeout_ms)
{
    if (s_wifi_event_group == NULL) {
        return false;
    }
    
    TickType_t wait_ticks = (timeout_ms == 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT,
                                           pdFALSE, pdFALSE,
                                           wait_ticks);
    
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

bool wifi_smartconfig_is_connected(void)
{
    return s_is_connected;
}

bool wifi_smartconfig_reset(void)
{
    ESP_LOGI(TAG, "清除 WiFi 配置并重新配网");
    
    // 断开当前连接
    esp_wifi_disconnect();
    s_is_connected = false;
    
    // 清除保存的配置
    wifi_config_t wifi_config;
    memset(&wifi_config, 0, sizeof(wifi_config_t));
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    
    // 清除事件组标志
    if (s_wifi_event_group) {
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT | SMARTCONFIG_DONE_BIT);
    }
    
    // 重新启动 SmartConfig
    s_retry_num = 0;
    xTaskCreate(smartconfig_task, "smartconfig_task", 4096, NULL, 3, NULL);
    
    return true;
}

bool wifi_smartconfig_get_ssid(char *ssid, size_t len)
{
    if (ssid == NULL || len == 0) {
        return false;
    }
    
    wifi_config_t wifi_config;
    if (esp_wifi_get_config(WIFI_IF_STA, &wifi_config) != ESP_OK) {
        return false;
    }
    
    strncpy(ssid, (char *)wifi_config.sta.ssid, len - 1);
    ssid[len - 1] = '\0';
    return true;
}

int wifi_smartconfig_get_rssi(void)
{
    if (!s_is_connected) {
        return 0;
    }
    
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) {
        return 0;
    }
    
    return ap_info.rssi;
}
