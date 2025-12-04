/**
 * @file main.c
 * @brief ESP32-S3 + A7670E 短信发送和接收示例
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "sms.h"

static const char *TAG = "MAIN";

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
    
    // 这里可以添加自定义的短信处理逻辑
    // 例如：根据短信内容执行特定操作
    if (strstr(message, "状态") || strstr(message, "status")) {
        ESP_LOGI(TAG, "收到状态查询请求");
        // 可以回复状态信息
        // sms_send(phone_number, "设备运行正常");
    }
}

/**
 * @brief 主任务
 */
static void main_task(void *arg)
{
    // 初始化短信模块
    if (!sms_init()) {
        ESP_LOGE(TAG, "短信模块初始化失败，请检查硬件连接");
        vTaskDelete(NULL);
        return;
    }
    
    ESP_LOGI(TAG, "短信模块初始化成功");
    
    // 注册短信接收回调
    sms_register_receive_callback(on_sms_received);
    
    // 发送测试短信（请替换为实际手机号）
    const char *phone_number = "[REDACTED]";
    
    // 发送中文短信测试
    ESP_LOGI(TAG, "发送测试短信...");
    if (sms_send(phone_number, "你好，ESP32短信模块已启动！")) {
        ESP_LOGI(TAG, "测试短信发送成功");
    } else {
        ESP_LOGE(TAG, "测试短信发送失败");
    }
    
    // 读取所有未读短信
    ESP_LOGI(TAG, "检查未读短信...");
    int unread_count = sms_read_all_unread();
    ESP_LOGI(TAG, "未读短信数量: %d", unread_count);
    
    // 启动短信接收监听任务
    sms_start_receive_task();
    ESP_LOGI(TAG, "短信接收监听已启动，等待新短信...");
    
    ESP_LOGI(TAG, "系统运行中，可随时接收短信...");
    
    // 保持任务运行
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        ESP_LOGI(TAG, "系统运行中...");
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "ESP32-S3 + A7670E 短信收发示例");
    ESP_LOGI(TAG, "=====================================");
    
    // 创建主任务
    xTaskCreate(main_task, "main_task", 8192, NULL, 5, NULL);
}
