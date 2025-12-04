/**
 * @file sms.c
 * @brief A7670E 短信模块实现
 * 
 * 实现短信发送和接收功能
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
#define SMS_SEND_TIMEOUT_MS     30000

// 短信接收任务句柄
static TaskHandle_t s_sms_receive_task_handle = NULL;

// 短信接收回调函数
static sms_receive_callback_t s_sms_receive_callback = NULL;

// 任务运行标志
static volatile bool s_receive_task_running = false;

// 发送中标志（暂停接收任务读取）
static volatile bool s_sending_sms = false;

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
 * @brief 发送短信内容
 */
static bool a7670e_send_sms_content(const char *content, uint32_t timeout_ms)
{
    char response[256];
    
    a7670e_uart_write_slow(content, strlen(content));
    
    char ctrl_z = 0x1A;
    uart_write_bytes(A7670E_UART_NUM, &ctrl_z, 1);
    
    ESP_LOGI(TAG, "发送短信内容: %s", content);
    
    memset(response, 0, sizeof(response));
    int total_len = 0;
    uint32_t start_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
    
    while ((xTaskGetTickCount() * portTICK_PERIOD_MS - start_time) < timeout_ms) {
        int len = uart_read_bytes(A7670E_UART_NUM, (uint8_t*)(response + total_len), 
                                   sizeof(response) - total_len - 1, pdMS_TO_TICKS(100));
        if (len > 0) {
            total_len += len;
            response[total_len] = '\0';
            
            if (strstr(response, "OK") || strstr(response, "+CMGS:")) {
                ESP_LOGI(TAG, "短信发送成功: %s", response);
                return true;
            }
            if (strstr(response, "ERROR")) {
                ESP_LOGE(TAG, "短信发送失败: %s", response);
                return false;
            }
        }
    }
    
    ESP_LOGW(TAG, "短信发送超时");
    return false;
}

/**
 * @brief UTF-8转UCS2编码
 */
static int utf8_to_ucs2_hex(const char *utf8_str, char *ucs2_buf, size_t buf_size)
{
    int out_len = 0;
    const unsigned char *p = (const unsigned char *)utf8_str;
    
    while (*p && out_len < (int)(buf_size - 5)) {
        uint32_t unicode = 0;
        
        if ((*p & 0x80) == 0) {
            unicode = *p;
            p++;
        } else if ((*p & 0xE0) == 0xC0) {
            unicode = (*p & 0x1F) << 6;
            p++;
            unicode |= (*p & 0x3F);
            p++;
        } else if ((*p & 0xF0) == 0xE0) {
            unicode = (*p & 0x0F) << 12;
            p++;
            unicode |= (*p & 0x3F) << 6;
            p++;
            unicode |= (*p & 0x3F);
            p++;
        } else {
            p++;
            continue;
        }
        
        out_len += snprintf(ucs2_buf + out_len, buf_size - out_len, "%04X", (unsigned int)unicode);
    }
    
    return out_len;
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
 * @brief 检测是否只包含ASCII字符
 */
static bool is_ascii_only(const char *str)
{
    const unsigned char *p = (const unsigned char *)str;
    while (*p) {
        if (*p & 0x80) {
            return false;
        }
        p++;
    }
    return true;
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
    
    // 查询网络注册状态
    ESP_LOGI(TAG, "检查网络注册状态...");
    for (int i = 0; i < 30; i++) {
        if (a7670e_send_at_cmd("AT+CREG?", "+CREG: 0,1", response, sizeof(response), AT_CMD_TIMEOUT_MS) ||
            a7670e_send_at_cmd("AT+CREG?", "+CREG: 0,5", response, sizeof(response), AT_CMD_TIMEOUT_MS)) {
            ESP_LOGI(TAG, "网络注册成功");
            break;
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
 * @brief 解析+CMGR响应，提取短信内容
 * @param cmgr_response CMGR响应字符串
 * @param phone_number 输出：发送方号码
 * @param message 输出：短信内容
 * @param timestamp 输出：时间戳
 * @param is_ucs2 是否为UCS2编码（保留参数，当前不使用）
 */
static void parse_cmgr_response(const char *cmgr_response, char *phone_number, 
                                 char *message, char *timestamp, bool is_ucs2)
{
    (void)is_ucs2;  // 暂不使用
    
    // 格式: +CMGR: "REC UNREAD","+8613800138000","","25/01/01,12:00:00+32"\r\n短信内容\r\n\r\nOK
    const char *p = strstr(cmgr_response, "+CMGR:");
    if (!p) return;
    
    ESP_LOGI(TAG, "解析CMGR响应: %s", cmgr_response);
    
    // 提取电话号码（第二个引号对之间的内容）
    const char *num_start = strchr(p, ',');
    if (num_start) {
        num_start = strchr(num_start, '"');
        if (num_start) {
            num_start++;
            const char *num_end = strchr(num_start, '"');
            if (num_end && (num_end - num_start) < 64) {
                strncpy(phone_number, num_start, num_end - num_start);
            }
        }
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
    
    // 提取短信内容（在第一个\r\n之后，到\r\n\r\nOK或\r\nOK之前）
    const char *content_start = strchr(p, '\n');
    if (content_start) {
        content_start++;
        
        // 跳过可能的\r
        if (*content_start == '\r') content_start++;
        
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

/**
 * @brief 读取指定索引的短信
 * @param index 短信索引
 * @return true: 读取成功, false: 失败
 */
static bool read_sms_by_index(int index)
{
    char cmd[32];
    char response[1024];  // 增大缓冲区以容纳中文
    char phone_number[64] = {0};
    char message[512] = {0};
    char timestamp[32] = {0};
    
    // 确保使用文本模式
    a7670e_send_at_cmd("AT+CMGF=1", "OK", response, sizeof(response), AT_CMD_TIMEOUT_MS);
    
    // 先尝试使用UCS2编码读取（可以正确显示中文）
    a7670e_send_at_cmd("AT+CSCS=\"UCS2\"", "OK", response, sizeof(response), AT_CMD_TIMEOUT_MS);
    
    snprintf(cmd, sizeof(cmd), "AT+CMGR=%d", index);
    
    if (a7670e_send_at_cmd(cmd, "OK", response, sizeof(response), AT_CMD_TIMEOUT_MS)) {
        // 解析响应
        parse_cmgr_response(response, phone_number, message, timestamp, true);
        
        // 如果电话号码是UCS2编码，转换为UTF-8
        if (is_ucs2_encoded(phone_number)) {
            char phone_utf8[64] = {0};
            ucs2_hex_to_utf8(phone_number, phone_utf8, sizeof(phone_utf8));
            strncpy(phone_number, phone_utf8, sizeof(phone_number) - 1);
        }
        
        // 如果消息是UCS2编码，转换为UTF-8
        if (is_ucs2_encoded(message)) {
            char message_utf8[512] = {0};
            ucs2_hex_to_utf8(message, message_utf8, sizeof(message_utf8));
            strncpy(message, message_utf8, sizeof(message) - 1);
        }
        
        // 调用回调函数
        if (s_sms_receive_callback && strlen(message) > 0) {
            ESP_LOGI(TAG, "收到短信 - 发送方: %s, 时间: %s", phone_number, timestamp);
            ESP_LOGI(TAG, "短信内容: %s", message);
            s_sms_receive_callback(phone_number, message, timestamp);
        }
        
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
    char buffer[256];
    int buf_index = 0;
    
    ESP_LOGI(TAG, "短信接收监听任务启动");
    
    while (s_receive_task_running) {
        // 如果正在发送短信，暂停读取
        if (s_sending_sms) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        
        // 读取UART数据
        int len = uart_read_bytes(A7670E_UART_NUM, (uint8_t*)(buffer + buf_index), 
                                   sizeof(buffer) - buf_index - 1, pdMS_TO_TICKS(100));
        
        if (len > 0) {
            buf_index += len;
            buffer[buf_index] = '\0';
            
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
                        
                        // 稍等一下让模块准备好
                        vTaskDelay(pdMS_TO_TICKS(500));
                        
                        // 读取短信
                        read_sms_by_index(sms_index);
                        
                        // 删除已读短信（可选）
                        // sms_delete(sms_index);
                    }
                    
                    // 清空缓冲区
                    buf_index = 0;
                    memset(buffer, 0, sizeof(buffer));
                }
            }
            
            // 防止缓冲区溢出
            if (buf_index > 200) {
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

bool sms_send(const char *phone_number, const char *message)
{
    char cmd[128];
    char response[256];
    bool result = false;
    
    // 设置发送标志，暂停接收任务
    s_sending_sms = true;
    vTaskDelay(pdMS_TO_TICKS(200));  // 等待接收任务暂停
    
    // 清空UART缓冲区
    uart_flush(A7670E_UART_NUM);
    
    ESP_LOGI(TAG, "准备发送短信到: %s", phone_number);
    ESP_LOGI(TAG, "短信内容: %s", message);
    
    // 检测消息是否包含非ASCII字符
    bool use_ucs2 = !is_ascii_only(message);
    
    if (use_ucs2) {
        ESP_LOGI(TAG, "检测到非ASCII字符，使用UCS2编码发送");
    } else {
        ESP_LOGI(TAG, "纯ASCII字符，使用GSM编码发送");
    }
    
    // 设置短信格式为文本模式
    if (!a7670e_send_at_cmd("AT+CMGF=1", "OK", response, sizeof(response), AT_CMD_TIMEOUT_MS)) {
        ESP_LOGE(TAG, "设置文本模式失败");
        return false;
    }
    
    if (use_ucs2) {
        if (!a7670e_send_at_cmd("AT+CSCS=\"UCS2\"", "OK", response, sizeof(response), AT_CMD_TIMEOUT_MS)) {
            ESP_LOGE(TAG, "设置UCS2字符集失败");
            s_sending_sms = false;
            return false;
        }
        
        // 设置文本模式参数：dcs=8表示UCS2编码
        a7670e_send_at_cmd("AT+CSMP=17,167,0,8", "OK", response, sizeof(response), AT_CMD_TIMEOUT_MS);
    } else {
        if (!a7670e_send_at_cmd("AT+CSCS=\"GSM\"", "OK", response, sizeof(response), AT_CMD_TIMEOUT_MS)) {
            ESP_LOGE(TAG, "设置GSM字符集失败");
            s_sending_sms = false;
            return false;
        }
    }
    
    // 准备电话号码和短信内容
    char phone_to_send[64];
    char message_to_send[512];
    
    if (use_ucs2) {
        utf8_to_ucs2_hex(phone_number, phone_to_send, sizeof(phone_to_send));
        utf8_to_ucs2_hex(message, message_to_send, sizeof(message_to_send));
    } else {
        strncpy(phone_to_send, phone_number, sizeof(phone_to_send) - 1);
        phone_to_send[sizeof(phone_to_send) - 1] = '\0';
        strncpy(message_to_send, message, sizeof(message_to_send) - 1);
        message_to_send[sizeof(message_to_send) - 1] = '\0';
    }
    
    // 发送AT+CMGS命令
    snprintf(cmd, sizeof(cmd), "AT+CMGS=\"%s\"", phone_to_send);
    
    uart_flush(A7670E_UART_NUM);
    
    ESP_LOGI(TAG, "发送: %s", cmd);
    a7670e_send_serial(cmd);
    
    // 等待">"提示符
    memset(response, 0, sizeof(response));
    int total_len = 0;
    uint32_t start_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
    
    while ((xTaskGetTickCount() * portTICK_PERIOD_MS - start_time) < AT_CMD_TIMEOUT_MS) {
        int len = uart_read_bytes(A7670E_UART_NUM, (uint8_t*)(response + total_len), 
                                   sizeof(response) - total_len - 1, pdMS_TO_TICKS(100));
        if (len > 0) {
            total_len += len;
            response[total_len] = '\0';
            
            if (strstr(response, ">")) {
                ESP_LOGI(TAG, "收到>提示符，开始输入短信内容");
                break;
            }
            if (strstr(response, "ERROR")) {
                ESP_LOGE(TAG, "发送失败: %s", response);
                return false;
            }
        }
    }
    
    if (!strstr(response, ">")) {
        ESP_LOGE(TAG, "未收到>提示符");
        s_sending_sms = false;
        return false;
    }
    
    result = a7670e_send_sms_content(message_to_send, SMS_SEND_TIMEOUT_MS);
    
    // 恢复接收任务
    s_sending_sms = false;
    
    return result;
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

int sms_read_all_unread(void)
{
    char response[1024];
    int count = 0;
    
    // 读取所有未读短信
    // AT+CMGL="REC UNREAD" 列出所有未读短信
    ESP_LOGI(TAG, "读取所有未读短信...");
    
    if (a7670e_send_at_cmd("AT+CMGL=\"REC UNREAD\"", "OK", response, sizeof(response), 10000)) {
        // 解析响应，查找所有+CMGL:开头的行
        char *p = response;
        while ((p = strstr(p, "+CMGL:")) != NULL) {
            // 提取索引
            int index = atoi(p + 7);
            if (index >= 0) {
                read_sms_by_index(index);
                count++;
            }
            p++;
        }
    }
    
    ESP_LOGI(TAG, "共读取 %d 条未读短信", count);
    return count;
}

bool sms_delete(int index)
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
    char response[128];
    
    // AT+CMGD=1,4 删除所有短信
    if (a7670e_send_at_cmd("AT+CMGD=1,4", "OK", response, sizeof(response), AT_CMD_TIMEOUT_MS)) {
        ESP_LOGI(TAG, "删除所有短信成功");
        return true;
    }
    
    ESP_LOGE(TAG, "删除所有短信失败");
    return false;
}
