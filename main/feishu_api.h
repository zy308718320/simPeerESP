/**
 * @file feishu_api.h
 * @brief 飞书自定义机器人 Webhook 接口
 */

#ifndef FEISHU_API_H
#define FEISHU_API_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 发送文本消息到飞书群机器人
 *
 * @note 本函数包含 HTTPS 请求，栈消耗较大（约 6KB+），
 *       不要在小栈任务或事件回调中调用
 *
 * @param text 消息内容（UTF-8，支持中文）
 * @return true 发送成功
 * @return false 发送失败
 */
bool feishu_send_text(const char *text);

#ifdef __cplusplus
}
#endif

#endif /* FEISHU_API_H */
