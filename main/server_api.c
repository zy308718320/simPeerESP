/**
 * @file server_api.c
 * @brief SimPeer 服务器 API 通信模块实现
 */

#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "server_api.h"

static const char *TAG = "SERVER_API";

/* NVS 存储键 */
#define NVS_NAMESPACE       "server_api"
#define NVS_KEY_TOKEN       "device_token"
#define NVS_KEY_PHONE       "phone_number"

/* HTTP 响应缓冲区大小 */
#define HTTP_RESPONSE_BUFFER_SIZE   2048

/* 静态变量 */
static char s_phone_number[32] = {0};       // 绑定的手机号
static char s_http_response[HTTP_RESPONSE_BUFFER_SIZE];  // HTTP 响应缓冲区
static int s_http_response_len = 0;

/**
 * @brief HTTP 事件处理函数
 */
static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGD(TAG, "HTTP 已连接");
            break;
        case HTTP_EVENT_HEADERS_SENT:
            ESP_LOGD(TAG, "HTTP 请求头已发送");
            break;
        case HTTP_EVENT_ON_HEADER:
            ESP_LOGD(TAG, "收到响应头: %s: %s", evt->header_key, evt->header_value);
            break;
        case HTTP_EVENT_ON_DATA:
            // 无论是否是 chunked 响应都读取数据
            ESP_LOGD(TAG, "收到数据: %d 字节, chunked=%d", 
                     evt->data_len, esp_http_client_is_chunked_response(evt->client));
            if (s_http_response_len + evt->data_len < HTTP_RESPONSE_BUFFER_SIZE - 1) {
                memcpy(s_http_response + s_http_response_len, evt->data, evt->data_len);
                s_http_response_len += evt->data_len;
                s_http_response[s_http_response_len] = '\0';
            }
            break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGD(TAG, "HTTP 请求完成");
            break;
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGD(TAG, "HTTP 连接断开");
            break;
        case HTTP_EVENT_REDIRECT:
            ESP_LOGD(TAG, "HTTP 重定向");
            break;
        default:
            break;
    }
    return ESP_OK;
}

/**
 * @brief 从 NVS 加载手机号
 */
static bool load_phone_number(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return false;
    }
    
    size_t len = sizeof(s_phone_number);
    err = nvs_get_str(handle, NVS_KEY_PHONE, s_phone_number, &len);
    nvs_close(handle);
    
    return (err == ESP_OK && strlen(s_phone_number) > 0);
}

/**
 * @brief 保存手机号到 NVS
 */
static bool save_phone_number(const char *phone)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return false;
    }
    
    err = nvs_set_str(handle, NVS_KEY_PHONE, phone);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    
    return (err == ESP_OK);
}

bool server_api_get_mac_address(char *mac_str)
{
    if (mac_str == NULL) {
        return false;
    }
    
    uint8_t mac[6];
    esp_err_t err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "获取 MAC 地址失败: %s", esp_err_to_name(err));
        return false;
    }
    
    sprintf(mac_str, "%02X:%02X:%02X:%02X:%02X:%02X",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return true;
}

bool server_api_init(void)
{
    ESP_LOGI(TAG, "初始化服务器 API 模块");
    
    // 尝试加载已保存的手机号
    load_phone_number();
    
    return true;
}

void server_api_set_phone_number(const char *phone_number)
{
    if (phone_number == NULL) {
        return;
    }
    
    strncpy(s_phone_number, phone_number, sizeof(s_phone_number) - 1);
    save_phone_number(phone_number);
    ESP_LOGI(TAG, "已设置绑定手机号: %s", s_phone_number);
}

bool server_api_is_bound(void)
{
    // 使用 MAC 地址认证，总是返回 true
    return true;
}

bool server_api_upload_sms(const char *from_number, const char *to_number,
                           const char *content, const char *received_at)
{
    // 参数检查，content 不能为空
    if (content == NULL || strlen(content) == 0) {
        ESP_LOGE(TAG, "参数无效: content 为空");
        return false;
    }
    
    // fromNumber 允许为空
    const char *actual_from = (from_number != NULL) ? from_number : "";
    
    ESP_LOGI(TAG, "上传参数 - fromNumber: [%s], toNumber: [%s], content长度: %d", 
             actual_from, to_number ? to_number : "NULL", strlen(content));
    
    // 获取 MAC 地址用于认证
    char mac_str[18];
    if (!server_api_get_mac_address(mac_str)) {
        ESP_LOGE(TAG, "获取 MAC 地址失败");
        return false;
    }
    
    // 使用本机号码作为 to_number（如果未指定）
    const char *actual_to = (to_number != NULL && strlen(to_number) > 0) ? to_number : s_phone_number;
    
    ESP_LOGI(TAG, "上传短信到服务器...");
    ESP_LOGI(TAG, "发送方: %s", actual_from);
    ESP_LOGI(TAG, "接收方: %s", actual_to);
    ESP_LOGI(TAG, "设备 MAC: %s", mac_str);
    
    // 构建请求 JSON
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "fromNumber", actual_from);
    if (actual_to != NULL && strlen(actual_to) > 0) {
        cJSON_AddStringToObject(root, "toNumber", actual_to);
    }
    cJSON_AddStringToObject(root, "content", content);
    if (received_at != NULL && strlen(received_at) > 0) {
        cJSON_AddStringToObject(root, "receivedAt", received_at);
    }
    char *post_data = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    
    if (post_data == NULL) {
        ESP_LOGE(TAG, "创建 JSON 失败");
        return false;
    }
    
    // 构建完整 URL
    char url[256];
    snprintf(url, sizeof(url), "%s%s", SERVER_URL, API_SMS_WRITE);
    
    ESP_LOGI(TAG, "请求 URL: %s", url);
    ESP_LOGI(TAG, "请求数据: %s", post_data);
    
    // 配置 HTTP 客户端（使用 ESP-IDF 内置证书包进行 HTTPS 验证）
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .event_handler = http_event_handler,
        .timeout_ms = 60000,            // 总超时 60 秒
        .buffer_size = 4096,            // 增大接收缓冲区
        .buffer_size_tx = 2048,         // 增大发送缓冲区
        .crt_bundle_attach = esp_crt_bundle_attach,  // 使用内置证书包
        .disable_auto_redirect = false, // 允许重定向
        .is_async = false,              // 同步模式
        .keep_alive_enable = false,     // 禁用 keep-alive
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "创建 HTTP 客户端失败");
        free(post_data);
        return false;
    }
    
    // 设置请求头和数据
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "X-Device-MAC", mac_str);  // 使用 MAC 地址认证
    esp_http_client_set_post_field(client, post_data, strlen(post_data));
    
    // 清空响应缓冲区
    s_http_response_len = 0;
    memset(s_http_response, 0, sizeof(s_http_response));
    
    // 执行请求
    esp_err_t err = esp_http_client_perform(client);
    bool success = false;
    
    // 获取状态码（即使 perform 返回错误也尝试获取）
    int status_code = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "HTTP 状态码: %d", status_code);
    ESP_LOGI(TAG, "HTTP 响应: %s", s_http_response);
    
    // 对于 4xx 错误，perform 可能返回 ESP_ERR_NOT_SUPPORTED，但我们仍然处理响应
    if (err == ESP_OK || status_code >= 200) {
        if (status_code == 200 || status_code == 201) {
            // 解析响应 JSON
            cJSON *response = cJSON_Parse(s_http_response);
            if (response != NULL) {
                cJSON *success_obj = cJSON_GetObjectItem(response, "success");
                if (success_obj && cJSON_IsTrue(success_obj)) {
                    ESP_LOGI(TAG, "短信上传成功！");
                    success = true;
                } else {
                    cJSON *message = cJSON_GetObjectItem(response, "message");
                    if (message && cJSON_IsString(message)) {
                        ESP_LOGE(TAG, "上传失败: %s", message->valuestring);
                    }
                }
                cJSON_Delete(response);
            }
        } else {
            ESP_LOGE(TAG, "上传请求失败，状态码: %d", status_code);
            ESP_LOGE(TAG, "服务器响应: %s", s_http_response);
        }
    } else {
        ESP_LOGE(TAG, "HTTP 请求失败: %s", esp_err_to_name(err));
        if (s_http_response_len > 0) {
            ESP_LOGE(TAG, "服务器响应: %s", s_http_response);
        }
    }
    
    esp_http_client_cleanup(client);
    free(post_data);
    
    return success;
}

bool server_api_clear_binding(void)
{
    // 使用 MAC 地址认证，无需清除 Token
    ESP_LOGI(TAG, "使用 MAC 地址认证，无需清除绑定信息");
    return true;
}
