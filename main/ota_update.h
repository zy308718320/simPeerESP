/**
 * @file ota_update.h
 * @brief GitHub Release OTA 固件升级接口
 */

#ifndef OTA_UPDATE_H
#define OTA_UPDATE_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动 OTA 检查任务
 *
 * 独立任务运行：启动 2 分钟后首次检查，之后每 24 小时检查一次。
 * 通过 GitHub API 查询最新 Release，版本号与当前固件不同则自动
 * 下载升级并重启。
 */
void ota_start_task(void);

/**
 * @brief 若设备刚通过 OTA 升级重启，发送飞书通知
 *
 * 在 WiFi 连接成功后调用一次。检测到升级标记时，向飞书群发送
 * "固件已通过 OTA 升级到 vX.Y.Z 并正常运行" 并清除标记。
 */
void ota_notify_if_just_upgraded(void);

#ifdef __cplusplus
}
#endif

#endif /* OTA_UPDATE_H */
