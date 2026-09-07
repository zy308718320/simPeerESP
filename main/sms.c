/**
 * @file sms.c
 * @brief A7670E 短信接收模块实现
 *
 * 实现短信接收监听功能
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "sms.h"

static const char *TAG = "SMS";

// UART配置
#define A7670E_UART_NUM         UART_NUM_1
#define A7670E_UART_TX_PIN      GPIO_NUM_18    // ESP32-S3 TX -> A7670E RX
#define A7670E_UART_RX_PIN      GPIO_NUM_17    // ESP32-S3 RX -> A7670E TX
#define A7670E_UART_BAUD_RATE   115200
#define A7670E_UART_BUF_SIZE    1024

// A7670E 电源控制引脚
#define A7670E_PWRKEY_PIN       GPIO_NUM_4
#define A7670E_RESET_PIN        GPIO_NUM_5

// 超时时间（毫秒）
#define AT_CMD_TIMEOUT_MS       5000

// 短信接收任务句柄
static TaskHandle_t s_sms_receive_task_handle = NULL;

// 短信接收回调函数
static sms_receive_callback_t s_sms_receive_callback = NULL;

// 任务运行标志
static volatile bool s_receive_task_running = false;

/**
 * @brief 初始化A7670E UART通信
 */
static void a7670e_uart_init(void)
{
    uart_config_t uart_config = {
        .baud_rate = A7670E_UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(A7670E_UART_NUM, A7670E_UART_BUF_SIZE * 2, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(A7670E_UART_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(A7670E_UART_NUM, A7670E_UART_TX_PIN, A7670E_UART_RX_PIN, 
                                  UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    ESP_LOGI(TAG, "UART初始化完成，TX: GPIO%d, RX: GPIO%d", A7670E_UART_TX_PIN, A7670E_UART_RX_PIN);
}

/**
 * @brief 初始化A7670E电源控制引脚
 */
static void a7670e_power_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << A7670E_PWRKEY_PIN) | (1ULL << A7670E_RESET_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    
    gpio_set_level(A7670E_PWRKEY_PIN, 0);
    gpio_set_level(A7670E_RESET_PIN, 1);
    
    ESP_LOGI(TAG, "电源控制引脚初始化完成");
}

/**
 * @brief 开启A7670E模块
 */
static void a7670e_power_on(void)
{
    ESP_LOGI(TAG, "正在开启A7670E模块...");
    
    // 先复位模块
    ESP_LOGI(TAG, "复位模块...");
    gpio_set_level(A7670E_RESET_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(500));
    gpio_set_level(A7670E_RESET_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    // PWRKEY拉高1.5秒开机
    ESP_LOGI(TAG, "PWRKEY开机...");
    gpio_set_level(A7670E_PWRKEY_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(1500));
    gpio_set_level(A7670E_PWRKEY_PIN, 0);
    
    // 等待模块启动完成
    ESP_LOGI(TAG, "等待模块启动...");
    vTaskDelay(pdMS_TO_TICKS(8000));
    
    // 清空UART缓冲区中的启动信息
    uart_flush(A7670E_UART_NUM);
    
    ESP_LOGI(TAG, "A7670E模块开机完成");
}

/**
 * @brief 逐字节发送数据到UART
 */
static void a7670e_uart_write_slow(const char *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        uart_write_bytes(A7670E_UART_NUM, &data[i], 1);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/**
 * @brief 发送AT指令字符串
 */
static void a7670e_send_serial(const char *cmd)
{
    a7670e_uart_write_slow(cmd, strlen(cmd));
    
    char cr = '\r';
    uart_write_bytes(A7670E_UART_NUM, &cr, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    
    char lf = '\n';
    uart_write_bytes(A7670E_UART_NUM, &lf, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
}

/**
 * @brief 发送AT指令并等待响应
 */
static bool a7670e_send_at_cmd(const char *cmd, const char *expected_response, 
                                char *response_buf, size_t buf_size, uint32_t timeout_ms)
{
    uart_flush(A7670E_UART_NUM);
    
    ESP_LOGI(TAG, "发送: %s", cmd);
    a7670e_send_serial(cmd);
    
    memset(response_buf, 0, buf_size);
    int total_len = 0;
    uint32_t start_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
    
    while ((xTaskGetTickCount() * portTICK_PERIOD_MS - start_time) < timeout_ms) {
        int len = uart_read_bytes(A7670E_UART_NUM, (uint8_t*)(response_buf + total_len), 
                                   buf_size - total_len - 1, pdMS_TO_TICKS(100));
        if (len > 0) {
            total_len += len;
            response_buf[total_len] = '\0';
            
            if (expected_response && strstr(response_buf, expected_response)) {
                ESP_LOGI(TAG, "收到响应: %s", response_buf);
                return true;
            }
            if (strstr(response_buf, "ERROR")) {
                ESP_LOGE(TAG, "收到错误响应: %s", response_buf);
                return false;
            }
        }
    }
    
    ESP_LOGW(TAG, "响应超时，已收到: %s", response_buf);
    return false;
}

/**
 * @brief UCS2编码转UTF-8
 */
static int ucs2_hex_to_utf8(const char *ucs2_str, char *utf8_buf, size_t buf_size)
{
    int out_len = 0;
    const char *p = ucs2_str;
    
    while (*p && out_len < (int)(buf_size - 4)) {
        // 读取4个十六进制字符
        char hex[5] = {0};
        if (strlen(p) < 4) break;
        
        strncpy(hex, p, 4);
        uint32_t unicode = (uint32_t)strtol(hex, NULL, 16);
        p += 4;
        
        // 转换为UTF-8
        if (unicode < 0x80) {
            utf8_buf[out_len++] = (char)unicode;
        } else if (unicode < 0x800) {
            utf8_buf[out_len++] = (char)(0xC0 | (unicode >> 6));
            utf8_buf[out_len++] = (char)(0x80 | (unicode & 0x3F));
        } else {
            utf8_buf[out_len++] = (char)(0xE0 | (unicode >> 12));
            utf8_buf[out_len++] = (char)(0x80 | ((unicode >> 6) & 0x3F));
            utf8_buf[out_len++] = (char)(0x80 | (unicode & 0x3F));
        }
    }
    
    utf8_buf[out_len] = '\0';
    return out_len;
}

/**
 * @brief 初始化A7670E模块
 */
static bool a7670e_module_init(void)
{
    char response[256];
    
    // 测试AT通信，增加重试次数
    ESP_LOGI(TAG, "测试AT通信...");
    for (int i = 0; i < 10; i++) {
        // 清空缓冲区
        uart_flush(A7670E_UART_NUM);
        
        if (a7670e_send_at_cmd("AT", "OK", response, sizeof(response), AT_CMD_TIMEOUT_MS)) {
            ESP_LOGI(TAG, "AT通信正常");
            break;
        }
        if (i == 9) {
            ESP_LOGE(TAG, "AT通信失败");
            return false;
        }
        ESP_LOGI(TAG, "重试 %d/10...", i + 1);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    
    // 关闭回显
    a7670e_send_at_cmd("ATE0", "OK", response, sizeof(response), AT_CMD_TIMEOUT_MS);
    
    // 查询模块信息
    a7670e_send_at_cmd("ATI", "OK", response, sizeof(response), AT_CMD_TIMEOUT_MS);
    
    // 查询SIM卡状态
    ESP_LOGI(TAG, "检查SIM卡状态...");
    if (!a7670e_send_at_cmd("AT+CPIN?", "READY", response, sizeof(response), AT_CMD_TIMEOUT_MS)) {
        ESP_LOGE(TAG, "SIM卡未就绪");
        return false;
    }
    ESP_LOGI(TAG, "SIM卡就绪");
    
    // 查询信号强度
    a7670e_send_at_cmd("AT+CSQ", "OK", response, sizeof(response), AT_CMD_TIMEOUT_MS);

    // 强制设置全功能模式（确保射频开启，部分场景模块可能处于 CFUN=0 最小功能模式）
    ESP_LOGI(TAG, "设置全功能模式 (AT+CFUN=1)...");
    if (!a7670e_send_at_cmd("AT+CFUN=1", "OK", response, sizeof(response), 10000)) {
        ESP_LOGW(TAG, "设置 CFUN=1 失败: %s", response);
    }
    vTaskDelay(pdMS_TO_TICKS(2000));    // 等待射频电路就绪

    // 查询网络注册状态
    // 响应格式 +CREG: <n>,<stat>，stat: 0未注册未搜索 1已注册 2搜索中 3拒绝 5漫游
    ESP_LOGI(TAG, "检查网络注册状态...");
    for (int i = 0; i < 30; i++) {
        if (a7670e_send_at_cmd("AT+CREG?", "+CREG:", response, sizeof(response), AT_CMD_TIMEOUT_MS)) {
            char *p = strstr(response, "+CREG:");
            p = strchr(p, ',');
            int stat = p ? atoi(p + 1) : -1;

            if (stat == 1 || stat == 5) {
                ESP_LOGI(TAG, "网络注册成功 (stat=%d)", stat);
                break;
            }
            ESP_LOGI(TAG, "网络未注册 (stat=%d: %s)，继续等待...", stat,
                     stat == 0 ? "未搜索" : stat == 2 ? "搜索中" : stat == 3 ? "被网络拒绝" : "未知");
        }
        if (i == 29) {
            ESP_LOGW(TAG, "网络注册超时，继续尝试");
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    
    // 设置短信格式为文本模式
    ESP_LOGI(TAG, "设置短信格式...");
    if (!a7670e_send_at_cmd("AT+CMGF=1", "OK", response, sizeof(response), AT_CMD_TIMEOUT_MS)) {
        ESP_LOGE(TAG, "设置短信格式失败");
        return false;
    }
    
    // 设置短信字符集
    a7670e_send_at_cmd("AT+CSCS=\"GSM\"", "OK", response, sizeof(response), AT_CMD_TIMEOUT_MS);
    
    // 设置新短信提示模式：直接输出到终端
    // AT+CNMI=2,1,0,0,0 - 新短信到达时输出+CMTI通知
    ESP_LOGI(TAG, "设置短信提示模式...");
    a7670e_send_at_cmd("AT+CNMI=2,1,0,0,0", "OK", response, sizeof(response), AT_CMD_TIMEOUT_MS);
    
    ESP_LOGI(TAG, "A7670E初始化完成");
    return true;
}

/**
 * @brief 解析+CMTI通知，提取短信索引
 * @param cmti_str CMTI字符串，如 "+CMTI: \"SM\",1"
 * @return 短信索引，失败返回-1
 */
static int parse_cmti_index(const char *cmti_str)
{
    // 格式: +CMTI: "SM",1 或 +CMTI: "ME",1
    const char *p = strstr(cmti_str, "+CMTI:");
    if (!p) return -1;
    
    // 找到逗号后的数字
    p = strchr(p, ',');
    if (!p) return -1;
    
    return atoi(p + 1);
}

/**
 * @brief 解析 +CMGR 读短信响应
 * @param response 响应字符串
 * @param phone_number 输出：发送方号码
 * @param message 输出：短信内容
 * @param timestamp 输出：时间戳
 */
static void parse_cmgr_sms(const char *response, char *phone_number,
                            char *message, char *timestamp)
{
    // +CMGR 格式: +CMGR: "REC UNREAD","+8613800138000","","25/01/01,12:00:00+32"\r\n短信内容\r\n\r\nOK

    const char *p = strstr(response, "+CMGR:");
    if (!p) return;

    ESP_LOGI(TAG, "解析CMGR响应: %.200s...", response);

    // 提取电话号码
    // +CMGR 格式: +CMGR: "状态","电话号码","","时间戳"  - 电话号码在第2个引号对
    //
    // 策略：遍历所有引号对，找到第一个看起来像电话号码的内容（数字、+号开头）

    phone_number[0] = '\0';  // 默认为空

    const char *quote = strchr(p, '"');
    int quote_pair = 0;

    while (quote && quote_pair < 5) {
        const char *q_start = quote + 1;
        const char *q_end = strchr(q_start, '"');

        if (!q_end) break;

        size_t len = q_end - q_start;
        quote_pair++;

        ESP_LOGD(TAG, "引号对%d: [%.*s]", quote_pair, (int)len, q_start);

        // 检查是否像电话号码：
        // 1. 长度 >= 3 且 < 32
        // 2. 内容只含十六进制字符（兼容 UCS2 编码的号码），开头允许 '+'
        //    （时间戳 "26/09/07,23:06:28+32" 含 / : , 会被此规则排除，
        //     "REC UNREAD" 等状态串含非十六进制字母同样被排除）
        if (len >= 3 && len < 32) {
            bool looks_like_number = true;
            for (size_t i = 0; i < len; i++) {
                char c = q_start[i];
                if (c == '+' && i == 0) {
                    continue;
                }
                if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))) {
                    looks_like_number = false;
                    break;
                }
            }

            if (looks_like_number) {
                strncpy(phone_number, q_start, len);
                phone_number[len] = '\0';
                ESP_LOGI(TAG, "解析到电话号码(引号对%d): [%s]", quote_pair, phone_number);
                break;
            }
        }

        // 继续下一个引号对
        quote = strchr(q_end + 1, '"');
    }

    if (phone_number[0] == '\0') {
        ESP_LOGW(TAG, "未找到有效电话号码");
    }

    // 提取时间戳（最后一个引号对之间的内容）
    const char *time_end = strrchr(p, '"');
    if (time_end && time_end > p) {
        const char *time_start = time_end - 1;
        while (time_start > p && *time_start != '"') {
            time_start--;
        }
        if (*time_start == '"') {
            time_start++;
            if ((time_end - time_start) < 32 && (time_end - time_start) > 0) {
                strncpy(timestamp, time_start, time_end - time_start);
            }
        }
    }

    // 提取短信内容（在第一个\r\n之后）
    const char *content_start = strchr(p, '\n');
    if (content_start) {
        content_start++;

        // 跳过可能的\r
        if (*content_start == '\r') content_start++;

        // 查找内容结束位置
        const char *content_end = strstr(content_start, "\r\n\r\nOK");
        if (!content_end) {
            content_end = strstr(content_start, "\r\nOK");
        }
        if (!content_end) {
            content_end = strstr(content_start, "\nOK");
        }
        if (!content_end) {
            content_end = content_start + strlen(content_start);
        }

        // 去除末尾的\r\n
        while (content_end > content_start && (*(content_end-1) == '\r' || *(content_end-1) == '\n')) {
            content_end--;
        }

        if (content_end > content_start) {
            size_t len = content_end - content_start;
            if (len > 511) len = 511;
            strncpy(message, content_start, len);
            message[len] = '\0';
            ESP_LOGI(TAG, "解析到短信内容(原始): [%s]", message);
        }
    }
}

/**
 * @brief 检测字符串是否为UCS2编码（全部是十六进制字符且长度为4的倍数）
 */
static bool is_ucs2_encoded(const char *str)
{
    size_t len = strlen(str);
    if (len == 0 || len % 4 != 0) return false;
    
    for (size_t i = 0; i < len; i++) {
        char c = str[i];
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

// 前向声明（读取并处理完短信后立即删除，释放SIM卡空间）
static bool sms_delete(int index);

/**
 * @brief 读取指定索引的短信
 * @param index 短信索引
 * @return true: 读取成功, false: 失败
 */
static bool read_sms_by_index(int index)
{
    char cmd[32];
    /* 大缓冲使用 static 存储以降低任务栈压力
     * （本函数仅被短信接收任务调用，单线程访问，无并发问题） */
    static char response[1024];    // 增大缓冲区以容纳中文
    static char phone_number[64];
    static char message[512];
    static char timestamp[32];

    response[0] = phone_number[0] = message[0] = timestamp[0] = '\0';
    
    // 确保使用文本模式
    a7670e_send_at_cmd("AT+CMGF=1", "OK", response, sizeof(response), AT_CMD_TIMEOUT_MS);
    
    // 先尝试使用UCS2编码读取（可以正确显示中文）
    a7670e_send_at_cmd("AT+CSCS=\"UCS2\"", "OK", response, sizeof(response), AT_CMD_TIMEOUT_MS);
    
    snprintf(cmd, sizeof(cmd), "AT+CMGR=%d", index);
    
    if (a7670e_send_at_cmd(cmd, "OK", response, sizeof(response), AT_CMD_TIMEOUT_MS)) {
        // 解析响应
        parse_cmgr_sms(response, phone_number, message, timestamp);

        // 如果电话号码是UCS2编码，转换为UTF-8（static缓冲，降低栈压力）
        static char phone_utf8[64];
        static char message_utf8[512];
        if (is_ucs2_encoded(phone_number)) {
            memset(phone_utf8, 0, sizeof(phone_utf8));
            ucs2_hex_to_utf8(phone_number, phone_utf8, sizeof(phone_utf8));
            strncpy(phone_number, phone_utf8, sizeof(phone_number) - 1);
        }

        // 如果消息是UCS2编码，转换为UTF-8
        if (is_ucs2_encoded(message)) {
            memset(message_utf8, 0, sizeof(message_utf8));
            ucs2_hex_to_utf8(message, message_utf8, sizeof(message_utf8));
            strncpy(message, message_utf8, sizeof(message) - 1);
        }
        
        // 调用回调函数
        if (s_sms_receive_callback && strlen(message) > 0) {
            ESP_LOGI(TAG, "收到短信 [索引:%d] - 发送方: %s, 时间: %s", index, phone_number, timestamp);
            ESP_LOGI(TAG, "短信内容: %s", message);
            s_sms_receive_callback(phone_number, message, timestamp);
        }
        
        // 恢复GSM编码
        a7670e_send_at_cmd("AT+CSCS=\"GSM\"", "OK", response, sizeof(response), AT_CMD_TIMEOUT_MS);
        
        // 删除已处理的短信，释放SIM卡存储空间
        ESP_LOGI(TAG, "删除已处理的短信 [索引:%d]", index);
        sms_delete(index);
        
        return true;
    }
    
    // 恢复GSM编码
    a7670e_send_at_cmd("AT+CSCS=\"GSM\"", "OK", response, sizeof(response), AT_CMD_TIMEOUT_MS);
    
    return false;
}

/**
 * @brief 短信接收监听任务
 */
static void sms_receive_task(void *arg)
{
    // 使用静态缓冲区避免栈溢出
    static char buffer[2048];
    int buf_index = 0;

    memset(buffer, 0, sizeof(buffer));

    ESP_LOGI(TAG, "短信接收监听任务启动");

    while (s_receive_task_running) {
        // 读取UART数据
        int len = uart_read_bytes(A7670E_UART_NUM, (uint8_t*)(buffer + buf_index),
                                   sizeof(buffer) - buf_index - 1, pdMS_TO_TICKS(100));

        if (len > 0) {
            buf_index += len;
            buffer[buf_index] = '\0';

            /* 串口原始数据降为调试日志，避免频繁大块打印拖慢接收任务 */
            ESP_LOGD(TAG, "收到响应: \n%s", buffer);

            // 检查是否收到新短信通知 +CMTI
            char *cmti = strstr(buffer, "+CMTI:");
            if (cmti) {
                // 找到完整的CMTI消息（以\r\n结尾）
                char *cmti_end = strstr(cmti, "\r\n");
                if (cmti_end) {
                    *cmti_end = '\0';

                    int sms_index = parse_cmti_index(cmti);
                    if (sms_index >= 0) {
                        ESP_LOGI(TAG, "收到新短信通知，索引: %d", sms_index);

                        // 恢复字符串
                        *cmti_end = '\r';

                        // 稍等一下让模块准备好
                        vTaskDelay(pdMS_TO_TICKS(500));

                        // 读取短信
                        read_sms_by_index(sms_index);
                    }

                    // 清空缓冲区
                    buf_index = 0;
                    memset(buffer, 0, sizeof(buffer));
                    continue;
                }
            }

            // 防止缓冲区溢出
            if (buf_index > 1800) {
                ESP_LOGW(TAG, "缓冲区接近满，清空");
                buf_index = 0;
                memset(buffer, 0, sizeof(buffer));
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }

    ESP_LOGI(TAG, "短信接收监听任务退出");
    s_sms_receive_task_handle = NULL;
    vTaskDelete(NULL);
}

/*******************************************************************************
 * 公开接口实现
 ******************************************************************************/

bool sms_init(void)
{
    ESP_LOGI(TAG, "初始化短信模块...");
    
    // 初始化UART
    a7670e_uart_init();
    
    // 初始化电源控制引脚
    a7670e_power_init();
    
    // 开启A7670E模块
    a7670e_power_on();
    
    // 初始化模块
    return a7670e_module_init();
}

void sms_register_receive_callback(sms_receive_callback_t callback)
{
    s_sms_receive_callback = callback;
    ESP_LOGI(TAG, "短信接收回调已注册");
}

void sms_start_receive_task(void)
{
    if (s_sms_receive_task_handle != NULL) {
        ESP_LOGW(TAG, "短信接收任务已在运行");
        return;
    }
    
    s_receive_task_running = true;
    xTaskCreate(sms_receive_task, "sms_receive", 8192, NULL, 5, &s_sms_receive_task_handle);
}

void sms_stop_receive_task(void)
{
    if (s_sms_receive_task_handle == NULL) {
        return;
    }
    
    s_receive_task_running = false;
    // 等待任务退出
    vTaskDelay(pdMS_TO_TICKS(500));
}

/**
 * @brief 删除指定索引的短信（内部使用）
 */
static bool sms_delete(int index)
{
    char cmd[32];
    char response[128];
    
    snprintf(cmd, sizeof(cmd), "AT+CMGD=%d", index);
    
    if (a7670e_send_at_cmd(cmd, "OK", response, sizeof(response), AT_CMD_TIMEOUT_MS)) {
        ESP_LOGI(TAG, "删除短信 %d 成功", index);
        return true;
    }
    
    ESP_LOGE(TAG, "删除短信 %d 失败", index);
    return false;
}

bool sms_delete_all(void)
{
    char response[256];
    
    // AT+CMGD=1,4 删除所有短信
    // 当 SIM 卡短信存储满时，删除操作需要较长时间，使用 30 秒超时
    if (a7670e_send_at_cmd("AT+CMGD=1,4", "OK", response, sizeof(response), 30000)) {
        ESP_LOGI(TAG, "删除所有短信成功");
        return true;
    }
    
    // 如果批量删除失败，尝试逐条删除
    ESP_LOGW(TAG, "批量删除失败，尝试逐条删除...");
    bool any_deleted = false;
    for (int i = 1; i <= 50; i++) {
        char cmd[32];
        snprintf(cmd, sizeof(cmd), "AT+CMGD=%d", i);
        if (a7670e_send_at_cmd(cmd, "OK", response, sizeof(response), 5000)) {
            ESP_LOGI(TAG, "删除短信 %d 成功", i);
            any_deleted = true;
        }
    }
    
    if (any_deleted) {
        ESP_LOGI(TAG, "逐条删除短信完成");
        return true;
    }
    
    ESP_LOGE(TAG, "删除所有短信失败");
    return false;
}

bool sms_get_own_number(char *phone_number, size_t buf_size)
{
    char response[256];
    
    if (phone_number == NULL || buf_size == 0) {
        return false;
    }
    
    memset(phone_number, 0, buf_size);
    
    ESP_LOGI(TAG, "正在获取本机号码...");
    
    // 使用 AT+CNUM 命令获取本机号码
    // 响应格式: +CNUM: <alpha>,<number>,<type>
    // 例如: +CNUM: "","+8613800138000",145
    if (a7670e_send_at_cmd("AT+CNUM", "OK", response, sizeof(response), AT_CMD_TIMEOUT_MS)) {
        // 解析响应
        char *cnum = strstr(response, "+CNUM:");
        if (cnum) {
            // 找到第一个引号后的号码
            char *num_start = strchr(cnum, ',');
            if (num_start) {
                num_start = strchr(num_start, '"');
                if (num_start) {
                    num_start++;
                    char *num_end = strchr(num_start, '"');
                    if (num_end && (num_end - num_start) > 0) {
                        size_t len = num_end - num_start;
                        if (len >= buf_size) len = buf_size - 1;
                        strncpy(phone_number, num_start, len);
                        phone_number[len] = '\0';
                        
                        ESP_LOGI(TAG, "获取本机号码成功: %s", phone_number);
                        return true;
                    }
                }
            }
        }
        
        ESP_LOGW(TAG, "SIM卡未存储本机号码（+CNUM返回空）");
    }
    
    // 如果 AT+CNUM 失败，尝试从电话簿读取 "ON"（Own Number）
    ESP_LOGI(TAG, "尝试从电话簿读取本机号码...");
    
    // 选择ON电话簿（Own Number）
    if (a7670e_send_at_cmd("AT+CPBS=\"ON\"", "OK", response, sizeof(response), AT_CMD_TIMEOUT_MS)) {
        // 读取第一个条目
        if (a7670e_send_at_cmd("AT+CPBR=1", "+CPBR:", response, sizeof(response), AT_CMD_TIMEOUT_MS)) {
            // 格式: +CPBR: 1,"+8613800138000",145,"Name"
            char *num_start = strchr(response, ',');
            if (num_start) {
                num_start = strchr(num_start, '"');
                if (num_start) {
                    num_start++;
                    char *num_end = strchr(num_start, '"');
                    if (num_end && (num_end - num_start) > 0) {
                        size_t len = num_end - num_start;
                        if (len >= buf_size) len = buf_size - 1;
                        strncpy(phone_number, num_start, len);
                        phone_number[len] = '\0';
                        
                        // 恢复默认电话簿
                        a7670e_send_at_cmd("AT+CPBS=\"SM\"", "OK", response, sizeof(response), AT_CMD_TIMEOUT_MS);
                        
                        ESP_LOGI(TAG, "从电话簿获取本机号码成功: %s", phone_number);
                        return true;
                    }
                }
            }
        }
        // 恢复默认电话簿
        a7670e_send_at_cmd("AT+CPBS=\"SM\"", "OK", response, sizeof(response), AT_CMD_TIMEOUT_MS);
    }
    
    ESP_LOGW(TAG, "无法获取本机号码，SIM卡可能未存储号码");
    ESP_LOGW(TAG, "请手动在代码中配置本机号码，或联系运营商写入号码");
    
    return false;
}
