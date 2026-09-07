/**
 * @file ota_update.c
 * @brief GitHub Release OTA 固件升级实现
 *
 * 流程：
 *   1. GET GitHub API 查询仓库最新 Release
 *   2. 解析 tag_name（版本号）与 sms.bin 资产的下载地址
 *   3. 与当前固件版本（esp_app_desc）比较，不同则触发升级
 *   4. esp_https_ota 下载写入备用分区，校验通过后重启切换
 *
 * 安全性：全程 HTTPS（内置证书包验证）；升级写入的是未运行的
 * 备用分区，任何失败都不影响当前固件运行。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_crt_bundle.h"
#include "esp_app_desc.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "ota_update.h"
#include "wifi_smartconfig.h"
#include "feishu_api.h"
#include "secrets.h"

static const char *TAG = "OTA";

/* NVS 标记：升级重启前写入，新固件启动后读取并清除 */
#define OTA_NVS_NAMESPACE "ota"
#define OTA_NVS_KEY_PENDING "pending"

/* 任务参数 */
#define OTA_TASK_STACK_SIZE       12288   /* HTTPS/TLS 需要较大栈 */
#define OTA_TASK_PRIORITY         3
#define OTA_FIRST_CHECK_DELAY_MS  (2 * 60 * 1000)        /* 启动 2 分钟后首查 */
#define OTA_CHECK_INTERVAL_MS     (24 * 60 * 60 * 1000)  /* 之后每 24 小时 */

/* Release 信息缓冲（GitHub API 响应较大） */
#define MANIFEST_BUF_SIZE 8192

static char s_manifest[MANIFEST_BUF_SIZE];
static int s_manifest_len = 0;

/**
 * @brief HTTP 事件处理（收集 Release 信息响应）
 */
static esp_err_t ota_http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        if (s_manifest_len + evt->data_len < MANIFEST_BUF_SIZE - 1) {
            memcpy(s_manifest + s_manifest_len, evt->data, evt->data_len);
            s_manifest_len += evt->data_len;
            s_manifest[s_manifest_len] = '\0';
        }
    }
    return ESP_OK;
}

/**
 * @brief 从 JSON 中提取字符串字段（如 "tag_name":"v1.0.1"）
 */
static bool json_get_string(const char *body, const char *key, char *out, size_t out_size)
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
    if (*p != '"') {
        return false;
    }
    p++;

    const char *e = strchr(p, '"');
    if (!e) {
        return false;
    }

    size_t len = e - p;
    if (len == 0 || len >= out_size) {
        return false;
    }
    memcpy(out, p, len);
    out[len] = '\0';
    return true;
}

/**
 * @brief 在 Release JSON 中查找 sms.bin 资产的下载地址
 *
 * 遍历所有 "browser_download_url" 字段，取以 /sms.bin 结尾的那个
 */
static bool find_firmware_url(const char *body, char *url, size_t url_size)
{
    const char *key = "\"browser_download_url\"";
    const char *p = body;

    while ((p = strstr(p, key)) != NULL) {
        const char *v = strchr(p + strlen(key), ':');
        if (!v) {
            return false;
        }
        v++;
        while (*v == ' ') {
            v++;
        }
        if (*v != '"') {
            p += strlen(key);
            continue;
        }
        v++;

        const char *e = strchr(v, '"');
        if (!e) {
            return false;
        }

        size_t len = e - v;
        if (len > strlen("/sms.bin") && strcmp(v + len - strlen("/sms.bin"), "/sms.bin") == 0) {
            if (len >= url_size) {
                return false;
            }
            memcpy(url, v, len);
            url[len] = '\0';
            return true;
        }
        p = e;
    }
    return false;
}

/**
 * @brief 升级流程结束、重启前写入标记
 */
static void mark_ota_reboot_pending(void);

/**
 * @brief 获取最新 Release 的版本号和固件下载地址
 */
static bool fetch_latest_release(char *tag, size_t tag_size, char *url, size_t url_size)
{
    esp_http_client_config_t config = {
        .url = OTA_RELEASE_API_URL,
        .method = HTTP_METHOD_GET,
        .event_handler = ota_http_event_handler,
        .timeout_ms = 15000,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = false,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "创建 HTTP 客户端失败");
        return false;
    }

    s_manifest_len = 0;
    s_manifest[0] = '\0';
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200) {
        ESP_LOGE(TAG, "查询 Release 失败: err=%s, status=%d", esp_err_to_name(err), status);
        return false;
    }

    if (!json_get_string(s_manifest, "tag_name", tag, tag_size)) {
        ESP_LOGE(TAG, "Release 信息中未找到 tag_name");
        return false;
    }
    if (!find_firmware_url(s_manifest, url, url_size)) {
        ESP_LOGE(TAG, "Release 信息中未找到 sms.bin 下载地址");
        return false;
    }
    return true;
}

/**
 * @brief 检查更新并执行 OTA
 * @return true 已升级完成并即将重启
 */
static bool ota_check_and_update(void)
{
    char tag[32];
    char url[256];

    if (!fetch_latest_release(tag, sizeof(tag), url, sizeof(url))) {
        return false;
    }

    const char *current = esp_app_get_description()->version;
    const char *latest = (tag[0] == 'v') ? tag + 1 : tag;

    ESP_LOGI(TAG, "当前版本: %s, 最新版本: %s", current, latest);
    if (strcmp(current, latest) == 0) {
        ESP_LOGI(TAG, "已是最新版本");
        return false;
    }

    ESP_LOGI(TAG, "发现新版本，开始 OTA 升级...");
    ESP_LOGI(TAG, "下载地址: %s", url);

    esp_http_client_config_t http_config = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 30000,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
        .keep_alive_enable = false,
    };
    esp_https_ota_config_t ota_config = {
        .http_config = &http_config,
    };

    esp_https_ota_handle_t ota_handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_config, &ota_handle);
    if (err != ESP_OK || ota_handle == NULL) {
        ESP_LOGE(TAG, "OTA 开始失败: %s", esp_err_to_name(err));
        return false;
    }

    while ((err = esp_https_ota_perform(ota_handle)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        int total = esp_https_ota_get_image_size(ota_handle);
        int read = esp_https_ota_get_image_len_read(ota_handle);
        ESP_LOGI(TAG, "OTA 进度: %d/%d 字节 (%d%%)", read, total, total > 0 ? read * 100 / total : 0);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    if (esp_https_ota_is_complete_data_received(ota_handle)) {
        err = esp_https_ota_finish(ota_handle);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "OTA 升级完成，2 秒后重启切换到新版本...");
            mark_ota_reboot_pending();   /* 新固件启动后据此发送飞书通知 */
            vTaskDelay(pdMS_TO_TICKS(2000));
            esp_restart();
        }
        ESP_LOGE(TAG, "OTA 校验失败: %s", esp_err_to_name(err));
    } else {
        esp_https_ota_abort(ota_handle);
        ESP_LOGE(TAG, "OTA 下载数据不完整");
    }
    return false;
}

/**
 * @brief 一次性飞书通知任务（大栈承载 HTTPS/TLS，发送后自删除）
 */
static void ota_notify_task(void *arg)
{
    char *text = (char *)arg;
    if (feishu_send_text(text)) {
        ESP_LOGI(TAG, "升级通知已发送到飞书");
    } else {
        ESP_LOGE(TAG, "升级通知发送失败");
    }
    free(text);
    vTaskDelete(NULL);
}

/**
 * @brief 若设备刚通过 OTA 升级重启，发送飞书通知
 *
 * 由主任务在 WiFi 连接成功后调用：读取 NVS 中的升级标记，
 * 存在则通知"新版本已正常运行"并清除标记
 */
void ota_notify_if_just_upgraded(void)
{
    nvs_handle_t handle;
    if (nvs_open(OTA_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }

    uint8_t pending = 0;
    if (nvs_get_u8(handle, OTA_NVS_KEY_PENDING, &pending) == ESP_OK && pending) {
        /* 清除标记（先清后通知，即使通知失败也不会重复打扰） */
        nvs_set_u8(handle, OTA_NVS_KEY_PENDING, 0);
        nvs_commit(handle);
        nvs_close(handle);

        char text[96];
        snprintf(text, sizeof(text),
                 "🔄 固件已通过 OTA 升级到 v%s 并正常运行",
                 esp_app_get_description()->version);
        ESP_LOGI(TAG, "%s", text);

        char *task_text = strdup(text);
        if (task_text != NULL) {
            xTaskCreate(ota_notify_task, "ota_notify", 12288, task_text, 3, NULL);
        }
        return;
    }

    nvs_close(handle);
}

/**
 * @brief 升级流程结束、重启前写入标记
 */
static void mark_ota_reboot_pending(void)
{
    nvs_handle_t handle;
    if (nvs_open(OTA_NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_u8(handle, OTA_NVS_KEY_PENDING, 1);
        nvs_commit(handle);
        nvs_close(handle);
    }
}

/**
 * @brief OTA 检查任务：周期性检查 GitHub Release 更新
 */
static void ota_task(void *arg)
{
    /* 推迟首次检查，避开启动关键期（模块初始化/WiFi 连接） */
    vTaskDelay(pdMS_TO_TICKS(OTA_FIRST_CHECK_DELAY_MS));

    while (1) {
        if (wifi_smartconfig_is_connected()) {
            ESP_LOGI(TAG, "检查固件更新...");
            ota_check_and_update();
        } else {
            ESP_LOGW(TAG, "WiFi 未连接，跳过本次更新检查");
        }
        vTaskDelay(pdMS_TO_TICKS(OTA_CHECK_INTERVAL_MS));
    }
}

void ota_start_task(void)
{
    xTaskCreate(ota_task, "ota_task", OTA_TASK_STACK_SIZE, NULL, OTA_TASK_PRIORITY, NULL);
    ESP_LOGI(TAG, "OTA 更新任务已启动（启动 %d 分钟后首次检查，每 24 小时检查一次）",
             OTA_FIRST_CHECK_DELAY_MS / 60000);
}
