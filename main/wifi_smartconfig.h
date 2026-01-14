/**
 * @file wifi_smartconfig.h
 * @brief ESP32 SmartConfig WiFi 配网模块
 */

#ifndef WIFI_SMARTCONFIG_H
#define WIFI_SMARTCONFIG_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief WiFi 连接成功回调函数类型
 */
typedef void (*wifi_connected_callback_t)(void);

/**
 * @brief 初始化 WiFi 并启动 SmartConfig 配网
 * 
 * 该函数会初始化 WiFi 模块，首先尝试使用 NVS 中保存的凭据连接，
 * 如果没有保存的凭据或连接失败，则启动 SmartConfig 配网模式。
 * 
 * @param on_connected 连接成功后的回调函数，可以为 NULL
 * @return true 初始化成功
 * @return false 初始化失败
 */
bool wifi_smartconfig_init(wifi_connected_callback_t on_connected);

/**
 * @brief 等待 WiFi 连接完成
 * 
 * 阻塞等待直到 WiFi 连接成功或超时
 * 
 * @param timeout_ms 超时时间（毫秒），0 表示无限等待
 * @return true 连接成功
 * @return false 连接失败或超时
 */
bool wifi_smartconfig_wait_connected(uint32_t timeout_ms);

/**
 * @brief 检查 WiFi 是否已连接
 * 
 * @return true 已连接
 * @return false 未连接
 */
bool wifi_smartconfig_is_connected(void);

/**
 * @brief 清除保存的 WiFi 凭据并重新启动配网
 * 
 * @return true 操作成功
 * @return false 操作失败
 */
bool wifi_smartconfig_reset(void);

/**
 * @brief 获取当前连接的 WiFi SSID
 * 
 * @param ssid 用于存储 SSID 的缓冲区
 * @param len 缓冲区长度
 * @return true 获取成功
 * @return false 获取失败
 */
bool wifi_smartconfig_get_ssid(char *ssid, size_t len);

/**
 * @brief 获取当前 WiFi 信号强度 (RSSI)
 * 
 * @return int RSSI 值（dBm），获取失败返回 0
 */
int wifi_smartconfig_get_rssi(void);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_SMARTCONFIG_H */
