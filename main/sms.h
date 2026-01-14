/**
 * @file sms.h
 * @brief A7670E 短信模块接口定义
 * 
 * 提供短信发送和接收功能的接口
 */

#ifndef __SMS_H__
#define __SMS_H__

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 短信接收回调函数类型
 * @param phone_number 发送方电话号码
 * @param message 短信内容
 * @param timestamp 短信时间戳（可能为空）
 */
typedef void (*sms_receive_callback_t)(const char *phone_number, const char *message, const char *timestamp);

/**
 * @brief 初始化短信模块（包括UART和A7670E模块）
 * @return true: 初始化成功, false: 失败
 */
bool sms_init(void);

/**
 * @brief 发送短信（自动识别中英文）
 * @param phone_number 目标电话号码
 * @param message 短信内容（支持中英文，UTF-8编码）
 * @return true: 发送成功, false: 失败
 */
bool sms_send(const char *phone_number, const char *message);

/**
 * @brief 注册短信接收回调函数
 * @param callback 回调函数，当收到新短信时调用
 */
void sms_register_receive_callback(sms_receive_callback_t callback);

/**
 * @brief 启动短信接收监听任务
 * @note 调用此函数后，模块将持续监听新短信
 */
void sms_start_receive_task(void);

/**
 * @brief 停止短信接收监听任务
 */
void sms_stop_receive_task(void);

/**
 * @brief 读取所有未读短信
 * @return 读取到的短信数量
 */
int sms_read_all_unread(void);

/**
 * @brief 删除指定索引的短信
 * @param index 短信索引
 * @return true: 删除成功, false: 失败
 */
bool sms_delete(int index);

/**
 * @brief 删除所有短信
 * @return true: 删除成功, false: 失败
 */
bool sms_delete_all(void);

/**
 * @brief 获取本机号码（从SIM卡读取）
 * @param phone_number 存储号码的缓冲区
 * @param buf_size 缓冲区大小
 * @return true: 获取成功, false: 失败（SIM卡可能未存储号码）
 */
bool sms_get_own_number(char *phone_number, size_t buf_size);

#ifdef __cplusplus
}
#endif

#endif /* __SMS_H__ */
