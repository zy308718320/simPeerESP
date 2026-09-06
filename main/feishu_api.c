/**
 * @file feishu_api.c
 * @brief 飞书自定义机器人 Webhook 实现
 *
 * @note 不依赖 cJSON：请求 payload 为固定格式，手工构建并转义；
 *       响应只需提取 "code"/"StatusCode" 数值字段
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "feishu_api.h"
#include "secrets.h"

static const char *TAG = "FEISHU";

/* payload 缓冲：短信内容最大 512 字节（中文转义后约 2 倍），加消息头与前后缀 */
#define FEISHU_PAYLOAD_MAX  2048

/* HTTP 响应缓冲区 */
#define HTTP_RESPONSE_BUFFER_SIZE 1024

static char s_payload[FEISHU_PAYLOAD_MAX];
static char s_http_response[HTTP_RESPONSE_BUFFER_SIZE];
static int s_http_response_len = 0;

/**
 * @brief HTTP 事件处理函数
 */
static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (s_http_response_len + evt->data_len < HTTP_RESPONSE_BUFFER_SIZE - 1) {
                memcpy(s_http_response + s_http_response_len, evt->data, evt->data_len);
                s_http_response_len += evt->data_len;
                s_http_response[s_http_response_len] = '\0';
            }
            break;
        case HTTP_EVENT_ON_CONNECTED:
        case HTTP_EVENT_HEADERS_SENT:
        case HTTP_EVENT_ON_HEADER:
        case HTTP_EVENT_ON_FINISH:
        case HTTP_EVENT_DISCONNECTED:
        case HTTP_EVENT_REDIRECT:
        default:
            break;
    }
    return ESP_OK;
}

/**
 * @brief 将字符串按 JSON 规则转义后追加到缓冲区
 * @return 写入后的目标指针
 */
static char *append_escaped(char *dst, char *dst_end, const char *src)
{
    /* 预留 7 字节：转义序列 "\u00XX" 最长 6 字节 + 结尾 '\0' */
    for (; *src && dst < dst_end - 7; src++) {
        unsigned char c = (unsigned char)*src;
        if (c == '"' || c == '\\') {
            *dst++ = '\\';
            *dst++ = (char)c;
        } else if (c == '\n') {
            *dst++ = '\\'; *dst++ = 'n';
        } else if (c == '\r') {
            *dst++ = '\\'; *dst++ = 'r';
        } else if (c == '\t') {
            *dst++ = '\\'; *dst++ = 't';
        } else if (c < 0x20) {
            dst += snprintf(dst, (size_t)(dst_end - dst), "\\u%04X", c);
        } else {
            *dst++ = (char)c;   /* UTF-8 中文等多字节字符原样透传 */
        }
    }
    return dst;
}

/**
 * @brief 构建飞书文本消息 payload
 *        {"msg_type":"text","content":{"text":"..."}}
 * @return true 构建成功
 */
static bool build_payload(const char *text)
{
    char *p = s_payload;
    p += snprintf(p, FEISHU_PAYLOAD_MAX, "{\"msg_type\":\"text\",\"content\":{\"text\":\"");

    /* 转义并写入正文，通过写入位置精确判断是否被截断
     * （UTF-8 中文等多字节字符原样透传，实际膨胀很小） */
    char *end = append_escaped(p, s_payload + FEISHU_PAYLOAD_MAX, text);
    if (end >= s_payload + FEISHU_PAYLOAD_MAX - 4) {
        ESP_LOGE(TAG, "消息过长，无法构建 payload (text_len=%d)", (int)strlen(text));
        return false;
    }

    memcpy(end, "\"}}", 3);
    end += 3;
    *end = '\0';
    return true;
}

/**
 * @brief 从 JSON body 中提取数值字段（如 "code":0）
 * @return true 找到字段并写出数值
 */
static bool json_find_number(const char *body, const char *key, int *out_value)
{
    char pattern[24];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);

    const char *p = strstr(body, pattern);
    if (!p) {
        return false;
    }

    p = strchr(p + strlen(pattern), ':');
    if (!p) {
        return false;
    }
    p++;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p < '0' || *p > '9') {
        return false;
    }

    *out_value = (int)strtol(p, NULL, 10);
    return true;
}

/**
 * @brief 解析飞书响应，判断是否成功
 *
 * 飞书自定义机器人成功时返回 HTTP 200，body 中
 * 新版为 {"code":0,...}，旧版为 {"StatusCode":0,...}
 *
 * @return true 发送成功
 */
static bool parse_feishu_response(void)
{
    int code = -1;
    if (json_find_number(s_http_response, "code", &code) ||
        json_find_number(s_http_response, "StatusCode", &code)) {
        if (code != 0) {
            ESP_LOGE(TAG, "飞书返回错误码: %d, body: %s", code, s_http_response);
            return false;
        }
    }
    /* 找不到错误码字段时，HTTP 200 即视为成功 */
    return true;
}

bool feishu_send_text(const char *text)
{
    if (text == NULL || strlen(text) == 0) {
        ESP_LOGE(TAG, "参数无效: text 为空");
        return false;
    }

    if (!build_payload(text)) {
        return false;
    }

    /* 配置 HTTP 客户端（HTTPS，使用内置证书包验证） */
    esp_http_client_config_t config = {
        .url = FEISHU_WEBHOOK_URL,
        .method = HTTP_METHOD_POST,
        .event_handler = http_event_handler,
        .timeout_ms = 15000,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = false,
        .is_async = false,
        .keep_alive_enable = false,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "创建 HTTP 客户端失败（堆内存不足？）");
        return false;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, s_payload, strlen(s_payload));

    /* 清空响应缓冲区 */
    s_http_response_len = 0;
    s_http_response[0] = '\0';

    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);

    bool success = false;
    if (err == ESP_OK && status_code == 200) {
        success = parse_feishu_response();
        if (success) {
            ESP_LOGI(TAG, "飞书消息发送成功");
        }
    } else {
        ESP_LOGE(TAG, "HTTP 请求失败: err=%s, status=%d, body=%s",
                 esp_err_to_name(err), status_code, s_http_response);
    }

    esp_http_client_cleanup(client);
    return success;
}
