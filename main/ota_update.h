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

#ifdef __cplusplus
}
#endif

#endif /* OTA_UPDATE_H */
