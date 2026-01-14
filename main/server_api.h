/**
 * @file server_api.h
 * @brief SimPeer 服务器 API 通信模块
 */

#ifndef SERVER_API_H
#define SERVER_API_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 服务器配置 */
#define SERVER_URL          "https://simpeer.dpdns.org"
#define API_SMS_WRITE       "/api/sms/device/write"
#define API_SMS_WRITE_BATCH "/api/sms/device/write-batch"

/**
 * @brief 初始化服务器 API 模块
 * 
 * 从 NVS 中加载设备 Token（如果存在）
 * 
 * @return true 初始化成功
 * @return false 初始化失败
 */
bool server_api_init(void);

/**
 * @brief 设置设备绑定的手机号
 * 
 * @param phone_number 绑定的手机号
 */
void server_api_set_phone_number(const char *phone_number);

/**
 * @brief 检查设备是否已绑定
 * 
 * @return true 已绑定（有有效的 Device Token）
 * @return false 未绑定
 */
bool server_api_is_bound(void);

/**
 * @brief 上传单条短信到服务器
 * 
 * @param from_number 发送方号码
 * @param to_number 接收方号码（本机号码）
 * @param content 短信内容
 * @param received_at 接收时间（ISO 8601 格式，可为 NULL 使用当前时间）
 * @return true 上传成功
 * @return false 上传失败
 */
bool server_api_upload_sms(const char *from_number, const char *to_number,
                           const char *content, const char *received_at);

/**
 * @brief 获取设备 MAC 地址
 * 
 * @param mac_str 存储 MAC 地址的缓冲区（至少 18 字节）
 * @return true 获取成功
 * @return false 获取失败
 */
bool server_api_get_mac_address(char *mac_str);

/**
 * @brief 清除设备绑定信息
 * 
 * 清除 NVS 中保存的 Device Token
 * 
 * @return true 清除成功
 * @return false 清除失败
 */
bool server_api_clear_binding(void);

#ifdef __cplusplus
}
#endif

#endif /* SERVER_API_H */
