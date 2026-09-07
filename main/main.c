/**
 * @file main.c
 * @brief ESP32-S3 + A7670E 短信接收转发到飞书
 * @note 功能：SmartConfig 配网 + 短信接收 + 飞书机器人转发
 *
 * 架构说明（稳定性设计）：
 *   短信接收回调运行在 sms_receive 任务（栈较小）中，回调内不做任何
 *   网络请求，只把短信事件投递到队列；由独立的大栈转发任务
 *   sms_forward_task 负责与飞书的 HTTPS 通信（含失败重试）。
 *   避免在接收任务中执行 TLS 握手导致栈溢出/长时间阻塞 UART 接收。
 *
 * 时区说明：
 *   A7670E 短信时间戳为 GSM 格式 "YY/MM/DD,HH:MM:SS±zz"（zz 单位为
 *   15 分钟，如 +32 表示 +8 小时）。本程序将其换算为北京时间显示；
 *   同时通过 SNTP 同步系统时间作为兜底（WiFi 连接后自动同步）。
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "esp_app_desc.h"
#include "sms.h"
#include "wifi_smartconfig.h"
#include "feishu_api.h"
#include "ota_update.h"

static const char *TAG = "MAIN";

/* 备用号码：如果SIM卡未存储号码，则使用此备用号码（可为空） */
#define FALLBACK_PHONE_NUMBER "[REDACTED]"

/* 短信事件队列长度（队列满时丢弃新事件并记日志） */
#define SMS_QUEUE_LENGTH 8

/* 转发任务配置 */
#define FORWARD_TASK_STACK_SIZE 12288   /* HTTPS+TLS 需要较大栈 */
#define FEISHU_MAX_RETRY        3
#define FEISHU_RETRY_DELAY_MS   5000

/* WiFi 断开时等待恢复的最长时间（5s x 60 = 5 分钟），超时丢弃事件 */
#define WAIT_WIFI_MAX_ROUNDS    60
#define WAIT_WIFI_INTERVAL_MS   5000

/* 北京时间偏移（UTC+8，秒） */
#define BEIJING_TZ_OFFSET_SEC   (8 * 3600)

/* SNTP 同步成功前系统时间的时间戳判断阈值（2020-01-01） */
#define SNTP_MIN_VALID_EPOCH    1577836800

/* 本机号码缓冲区（自动从SIM卡获取，或使用备用号码） */
static char s_device_phone_number[32] = {0};

/* 短信事件（由接收任务投递，转发任务消费） */
typedef struct {
    char from[32];      /* 发送方号码 */
    char content[512];  /* 短信内容 */
    char time_str[40];  /* 已格式化的北京时间 */
} sms_event_t;

static QueueHandle_t s_sms_queue = NULL;

/* WiFi 连接状态标志（避免在事件回调中执行耗时操作） */
static volatile bool s_wifi_just_connected = false;
static volatile bool s_sntp_started = false;

/**
 * @brief 公历日期转自 1970-01-01 起的天数（Howard Hinnant 算法）
 */
static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    uint32_t yoe = (uint32_t)(y - era * 400);                      /* [0, 399] */
    uint32_t doy = (153u * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1; /* [0, 365] */
    uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;           /* [0, 146096] */
    return era * 146097 + (int64_t)doe - 719468;
}

/**
 * @brief 解析 GSM 短信时间戳并转换为北京时间字符串
 *
 * 输入格式: "YY/MM/DD,HH:MM:SS±zz"，zz 为相对 UTC 的偏移（单位 15 分钟）
 * 例如 "26/01/13,22:50:38+32" 中 +32 表示 +8 小时（东八区）
 *
 * @param gsm_ts GSM 时间戳字符串
 * @param out 输出缓冲区
 * @param out_size 输出缓冲区大小
 * @return true 转换成功
 */
static bool format_gsm_time_to_beijing(const char *gsm_ts, char *out, size_t out_size)
{
    int year, mon, day, hour, min, sec;
    int qz = 0;
    char sign = '+';
    int fields = sscanf(gsm_ts, "%2d/%2d/%2d,%2d:%2d:%2d%c%2d",
                        &year, &mon, &day, &hour, &min, &sec, &sign, &qz);

    if (fields < 6) {
        return false;   /* 基本字段缺失 */
    }

    /* 两位年份: 00-69 视为 20xx */
    int full_year = (year < 70) ? (2000 + year) : (1900 + year);

    /* 合理性校验，防止解析出垃圾时间 */
    if (full_year < 2020 || mon < 1 || mon > 12 || day < 1 || day > 31 ||
        hour < 0 || hour > 23 || min < 0 || min > 59 || sec < 0 || sec > 62) {
        return false;
    }

    /* GSM 时间戳显示的是短信中心的本地时间，需先减去时区偏移得到 UTC */
    int64_t epoch = days_from_civil(full_year, mon, day) * 86400
                    + (int64_t)hour * 3600 + min * 60 + sec;
    if (fields >= 8) {
        int64_t offset = (int64_t)qz * 900;    /* zz 单位为 15 分钟 */
        epoch -= (sign == '-') ? -offset : offset;
    }

    /* UTC + 8 小时 = 北京时间，用 gmtime_r 直接得到"墙上时钟" */
    time_t beijing = (time_t)(epoch + BEIJING_TZ_OFFSET_SEC);
    struct tm tm_info;
    gmtime_r(&beijing, &tm_info);
    strftime(out, out_size, "%Y-%m-%d %H:%M:%S", &tm_info);
    return true;
}

/**
 * @brief 获取当前北京时间字符串（SNTP 同步的系统时间）
 *
 * @param out 输出缓冲区
 * @param out_size 输出缓冲区大小
 */
static void get_beijing_time_now(char *out, size_t out_size)
{
    time_t now;
    time(&now);

    if (now < SNTP_MIN_VALID_EPOCH) {
        /* SNTP 尚未同步，系统时间为编译期默认值 */
        snprintf(out, out_size, "时间未同步");
        return;
    }

    time_t beijing = now + BEIJING_TZ_OFFSET_SEC;
    struct tm tm_info;
    gmtime_r(&beijing, &tm_info);
    strftime(out, out_size, "%Y-%m-%d %H:%M:%S", &tm_info);
}

/**
 * @brief 启动 SNTP 时间同步（WiFi 连接后调用一次）
 */
static void start_sntp(void)
{
    if (s_sntp_started) {
        return;
    }
    s_sntp_started = true;

    ESP_LOGI(TAG, "启动 SNTP 时间同步...");
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_setservername(1, "pool.ntp.org");
    esp_sntp_init();
}

/**
 * @brief 短信接收回调函数
 *
 * @note 运行在 sms_receive 任务中（栈有限），只做轻量的格式化
 *       和入队操作，严禁在此执行网络请求
 */
static void on_sms_received(const char *phone_number, const char *message, const char *timestamp)
{
    ESP_LOGI(TAG, "========== 收到新短信 ==========");
    ESP_LOGI(TAG, "发送方: %s", phone_number);
    ESP_LOGI(TAG, "内容: %s", message);
    ESP_LOGI(TAG, "=================================");

    sms_event_t event = {0};
    strlcpy(event.from, phone_number, sizeof(event.from));
    strlcpy(event.content, message, sizeof(event.content));

    /* 优先使用短信自带的时间戳（换算为北京时间），失败则用系统时间 */
    if (timestamp == NULL || !format_gsm_time_to_beijing(timestamp, event.time_str, sizeof(event.time_str))) {
        get_beijing_time_now(event.time_str, sizeof(event.time_str));
    }
    ESP_LOGI(TAG, "短信时间(北京): %s", event.time_str);

    /* 非阻塞入队；队列满时丢弃并记日志，绝不阻塞接收任务 */
    if (xQueueSend(s_sms_queue, &event, 0) != pdTRUE) {
        ESP_LOGE(TAG, "转发队列已满，丢弃本条短信通知");
    }
}

/**
 * @brief 短信转发任务：从队列取出短信事件，发送到飞书
 *
 * @note 独立大栈任务，HTTPS/TLS 的栈消耗集中在这里
 */
static void sms_forward_task(void *arg)
{
    static sms_event_t event;
    static char text[1024];

    ESP_LOGI(TAG, "飞书转发任务启动");

    while (1) {
        if (xQueueReceive(s_sms_queue, &event, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        /* WiFi 断开时短暂等待恢复（断线期间的短信不立即丢弃） */
        int wait_rounds = 0;
        while (!wifi_smartconfig_is_connected() && wait_rounds < WAIT_WIFI_MAX_ROUNDS) {
            if (wait_rounds == 0) {
                ESP_LOGW(TAG, "WiFi 未连接，等待网络恢复后转发...");
            }
            vTaskDelay(pdMS_TO_TICKS(WAIT_WIFI_INTERVAL_MS));
            wait_rounds++;
        }
        if (!wifi_smartconfig_is_connected()) {
            ESP_LOGE(TAG, "WiFi 长时间未恢复，丢弃短信通知 (来自 %s)", event.from);
            continue;
        }

        snprintf(text, sizeof(text),
                 "📩 收到新短信\n"
                 "━━━━━━━━━━━━\n"
                 "发件人: %s\n"
                 "接收卡: %s\n"
                 "时间: %s\n"
                 "━━━━━━━━━━━━\n"
                 "%s",
                 strlen(event.from) > 0 ? event.from : "未知号码",
                 strlen(s_device_phone_number) > 0 ? s_device_phone_number : "未知",
                 event.time_str,
                 event.content);

        /* 发送飞书通知，失败重试 */
        bool ok = false;
        for (int retry = 0; retry < FEISHU_MAX_RETRY && !ok; retry++) {
            if (retry > 0) {
                ESP_LOGW(TAG, "飞书转发失败，%d 秒后重试 (%d/%d)...",
                         FEISHU_RETRY_DELAY_MS / 1000, retry + 1, FEISHU_MAX_RETRY);
                vTaskDelay(pdMS_TO_TICKS(FEISHU_RETRY_DELAY_MS));
            }
            ok = feishu_send_text(text);
        }

        if (ok) {
            ESP_LOGI(TAG, "短信已转发到飞书 (来自 %s)", event.from);
        } else {
            ESP_LOGE(TAG, "飞书转发失败，已放弃本条短信通知 (来自 %s)", event.from);
        }
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
            ESP_LOGW(TAG, "无法获取本机号码");
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
 * @brief WiFi 连接成功回调函数
 * @note 此回调运行在系统事件任务中，栈空间有限，不要在此执行耗时操作
 */
static void on_wifi_connected(void)
{
    ESP_LOGI(TAG, "========== WiFi 连接成功 ==========");

    char ssid[33] = {0};
    if (wifi_smartconfig_get_ssid(ssid, sizeof(ssid))) {
        ESP_LOGI(TAG, "已连接到: %s", ssid);
    }
    ESP_LOGI(TAG, "信号强度: %d dBm", wifi_smartconfig_get_rssi());
    ESP_LOGI(TAG, "====================================");

    // 设置标志位，让主任务执行耗时初始化
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
        // WiFi 连接成功后的初始化（SNTP 时间同步、升级通知检查）
        if (s_wifi_just_connected) {
            s_wifi_just_connected = false;
            start_sntp();
            ota_notify_if_just_upgraded();
        }

        vTaskDelay(pdMS_TO_TICKS(10000));
        if (wifi_smartconfig_is_connected()) {
            ESP_LOGI(TAG, "系统运行中... WiFi RSSI: %d dBm, 本机号码: %s",
                     wifi_smartconfig_get_rssi(),
                     strlen(s_device_phone_number) > 0 ? s_device_phone_number : "未知");
        } else {
            ESP_LOGW(TAG, "WiFi 断开连接，等待重连...");
        }
    }
}

void app_main(void)
{
    const esp_app_desc_t *app_desc = esp_app_get_description();
    ESP_LOGI(TAG, "ESP32-S3 + A7670E 短信转发器 v%s", app_desc->version);
    ESP_LOGI(TAG, "功能: SmartConfig 配网 + 短信接收 + 飞书通知 + OTA");
    ESP_LOGI(TAG, "=====================================");

    // 创建短信事件队列（接收任务 -> 转发任务）
    s_sms_queue = xQueueCreate(SMS_QUEUE_LENGTH, sizeof(sms_event_t));
    if (s_sms_queue == NULL) {
        ESP_LOGE(TAG, "创建短信队列失败");
        return;
    }

    // 创建主任务
    xTaskCreate(main_task, "main_task", 8192, NULL, 5, NULL);

    // 创建飞书转发任务（大栈，承载 HTTPS/TLS）
    xTaskCreate(sms_forward_task, "sms_forward", FORWARD_TASK_STACK_SIZE, NULL, 4, NULL);

    // 启动 OTA 更新检查任务（启动 2 分钟后首查，每 24 小时一次）
    ota_start_task();
}
