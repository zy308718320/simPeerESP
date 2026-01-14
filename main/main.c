/**
 * @file main.c
 * @brief ESP32-S3 + A7670E 短信发送和接收示例
 * @note 集成 SmartConfig 配网功能，配网成功后初始化短信模块
 *       收到短信后自动上传到 SimPeer 服务器
 *       本机号码从 SIM 卡自动获取
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"
#include "sms.h"
#include "wifi_smartconfig.h"
#include "server_api.h"

static const char *TAG = "MAIN";

/* 本机号码缓冲区（自动从SIM卡获取，或使用备用号码） */
static char s_device_phone_number[32] = {0};

/* 备用号码：如果SIM卡未存储号码，则使用此备用号码（可为空） */
#define FALLBACK_PHONE_NUMBER "[REDACTED]"

/* WiFi 连接状态标志（避免在事件回调中执行耗时操作） */
static volatile bool s_wifi_just_connected = false;
static volatile bool s_server_initialized = false;

/**
 * @brief 短信接收回调函数
 */
static void on_sms_received(const char *phone_number, const char *message, const char *timestamp)
{
    ESP_LOGI(TAG, "========== 收到新短信 ==========");
    ESP_LOGI(TAG, "发送方: %s", phone_number);
    ESP_LOGI(TAG, "时间: %s", timestamp ? timestamp : "未知");
    ESP_LOGI(TAG, "内容: %s", message);
    ESP_LOGI(TAG, "=================================");
    
    // 上传短信到服务器
    if (wifi_smartconfig_is_connected()) {
        if (strlen(s_device_phone_number) == 0) {
            ESP_LOGW(TAG, "本机号码未知，使用发送方号码作为接收方");
        }
        
        ESP_LOGI(TAG, "正在上传短信到服务器...");
        const char *to_number = strlen(s_device_phone_number) > 0 ? s_device_phone_number : NULL;
        if (server_api_upload_sms(phone_number, to_number, message, timestamp)) {
            ESP_LOGI(TAG, "短信上传成功");
        } else {
            ESP_LOGE(TAG, "短信上传失败");
        }
    } else {
        ESP_LOGW(TAG, "WiFi 未连接，无法上传短信");
    }
}

/**
 * @brief 初始化短信模块并启动相关功能
 * @return true: 初始化成功, false: 失败
 */
static bool init_sms_module(void)
{
    // 初始化短信模块
    if (!sms_init()) {
        ESP_LOGE(TAG, "短信模块初始化失败，请检查硬件连接");
        return false;
    }
    
    ESP_LOGI(TAG, "短信模块初始化成功");
    
    // 尝试从SIM卡获取本机号码
    ESP_LOGI(TAG, "正在获取本机号码...");
    if (sms_get_own_number(s_device_phone_number, sizeof(s_device_phone_number))) {
        ESP_LOGI(TAG, "本机号码: %s", s_device_phone_number);
    } else {
        // 使用备用号码
        if (strlen(FALLBACK_PHONE_NUMBER) > 0) {
            strncpy(s_device_phone_number, FALLBACK_PHONE_NUMBER, sizeof(s_device_phone_number) - 1);
            ESP_LOGW(TAG, "使用备用号码: %s", s_device_phone_number);
        } else {
            ESP_LOGW(TAG, "无法获取本机号码，短信上传时将不包含接收方号码");
            ESP_LOGW(TAG, "如需指定号码，请在代码中设置 FALLBACK_PHONE_NUMBER");
        }
    }
    
    // 注册短信接收回调
    sms_register_receive_callback(on_sms_received);
    
    // 清空 SIM 卡中所有旧短信，释放存储空间
    // 避免 "+SMS FULL" 错误导致无法接收新短信
    ESP_LOGI(TAG, "清空 SIM 卡中的旧短信...");
    if (sms_delete_all()) {
        ESP_LOGI(TAG, "旧短信清理完成");
    } else {
        ESP_LOGW(TAG, "旧短信清理失败，继续运行");
    }
    
    // 启动短信接收监听任务
    sms_start_receive_task();
    ESP_LOGI(TAG, "短信接收监听已启动，等待新短信...");
    
    return true;
}

/**
 * @brief 测试 DNS 解析
 * @return true: DNS 解析成功, false: 失败
 */
static bool test_dns_resolution(const char *hostname)
{
    struct hostent *host = gethostbyname(hostname);
    if (host != NULL) {
        char ip_str[16];
        inet_ntop(AF_INET, host->h_addr_list[0], ip_str, sizeof(ip_str));
        ESP_LOGI(TAG, "DNS 解析成功: %s -> %s", hostname, ip_str);
        return true;
    } else {
        ESP_LOGE(TAG, "DNS 解析失败: %s", hostname);
        return false;
    }
}

/**
 * @brief 初始化服务器连接
 */
static void init_server_connection(void)
{
    // 等待网络稳定
    ESP_LOGI(TAG, "等待网络稳定...");
    vTaskDelay(pdMS_TO_TICKS(3000));
    
    // 测试 DNS 解析
    ESP_LOGI(TAG, "测试 DNS 解析...");
    if (!test_dns_resolution("simpeer.dpdns.org")) {
        ESP_LOGE(TAG, "无法解析服务器域名，请检查网络连接");
        return;
    }
    
    // 初始化服务器 API 模块
    server_api_init();
    
    // 如果已获取到本机号码，设置为绑定手机号
    if (strlen(s_device_phone_number) > 0) {
        server_api_set_phone_number(s_device_phone_number);
    }
    
    ESP_LOGI(TAG, "服务器连接初始化完成");
}

/**
 * @brief WiFi 连接成功回调函数
 * @note 此回调运行在系统事件任务中，栈空间有限，不要在此执行耗时操作
 */
static void on_wifi_connected(void)
{
    ESP_LOGI(TAG, "========== WiFi 连接成功 ==========");
    
    // 打印连接信息（这些操作栈消耗小，可以在回调中执行）
    char ssid[33] = {0};
    if (wifi_smartconfig_get_ssid(ssid, sizeof(ssid))) {
        ESP_LOGI(TAG, "已连接到: %s", ssid);
    }
    ESP_LOGI(TAG, "信号强度: %d dBm", wifi_smartconfig_get_rssi());
    ESP_LOGI(TAG, "====================================");
    
    // 设置标志位，让主任务执行服务器连接（避免在事件回调中执行HTTP请求）
    s_wifi_just_connected = true;
}

/**
 * @brief 主任务
 */
static void main_task(void *arg)
{
    // 先初始化短信模块并获取本机号码
    ESP_LOGI(TAG, "开始初始化短信模块...");
    if (!init_sms_module()) {
        ESP_LOGE(TAG, "短信模块初始化失败");
        vTaskDelete(NULL);
        return;
    }
    
    ESP_LOGI(TAG, "启动 WiFi SmartConfig 配网...");
    ESP_LOGI(TAG, "请使用 ESPTouch App 进行配网（如未配网）");
    
    // 初始化 WiFi 并启动 SmartConfig，传入连接成功回调
    if (!wifi_smartconfig_init(on_wifi_connected)) {
        ESP_LOGE(TAG, "WiFi SmartConfig 初始化失败");
        vTaskDelete(NULL);
        return;
    }
    
    // 等待 WiFi 连接成功（无限等待）
    ESP_LOGI(TAG, "等待 WiFi 连接...");
    if (!wifi_smartconfig_wait_connected(0)) {
        ESP_LOGE(TAG, "WiFi 连接失败");
        vTaskDelete(NULL);
        return;
    }
    
    ESP_LOGI(TAG, "系统运行中，可随时接收短信...");
    
    // 保持任务运行，定期打印状态
    while (1) {
        // 检查是否需要初始化服务器连接（从事件回调移到这里执行）
        if (s_wifi_just_connected && !s_server_initialized) {
            s_wifi_just_connected = false;
            
            // 在主任务中执行服务器连接（有足够的栈空间）
            ESP_LOGI(TAG, "开始初始化服务器连接...");
            init_server_connection();
            s_server_initialized = true;
        }
        
        vTaskDelay(pdMS_TO_TICKS(10000));
        if (wifi_smartconfig_is_connected()) {
            ESP_LOGI(TAG, "系统运行中... WiFi RSSI: %d dBm, 本机号码: %s", 
                     wifi_smartconfig_get_rssi(),
                     strlen(s_device_phone_number) > 0 ? s_device_phone_number : "未知");
        } else {
            ESP_LOGW(TAG, "WiFi 断开连接，等待重连...");
            s_server_initialized = false;  // WiFi断开后需要重新初始化服务器连接
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "ESP32-S3 + A7670E 短信收发示例");
    ESP_LOGI(TAG, "集成 SmartConfig 配网 + SimPeer 服务器");
    ESP_LOGI(TAG, "服务器地址: %s", SERVER_URL);
    ESP_LOGI(TAG, "本机号码将从SIM卡自动获取");
    ESP_LOGI(TAG, "=====================================");
    
    // 创建主任务
    xTaskCreate(main_task, "main_task", 8192, NULL, 5, NULL);
}
