# simPeerESP — ESP32-S3 + A7670E 短信转发器

把插入设备的 SIM 卡收到的短信，通过 WiFi 实时转发到**飞书群机器人**，并支持 **OTA 远程固件升级**。

## 功能

- 📩 **短信接收转发**：A7670E 4G 模组接收短信（支持中文 UCS2 解码），自动推送到飞书群
- 🕐 **北京时间显示**：GSM 时间戳自动换算为北京时间（正确处理时区偏移），SNTP 同步兜底
- 📶 **SmartConfig 一键配网**：手机 ESPTouch App 配网，凭据持久化，断电不丢失
- 🔄 **OTA 远程升级**：每 24 小时检查 GitHub Release，有新版本自动升级（双分区，失败不影响运行）
- 🛡️ **稳定性设计**：短信接收与 HTTPS 转发通过队列解耦（独立任务），长期运行稳定
- 🔐 **密钥安全**：Webhook 等敏感配置放在 `main/secrets.h`（不入库）

飞书消息示例：

```
📩 收到新短信
━━━━━━━━━━━━
发件人: 10010
接收卡: [REDACTED]
时间: 2026-09-06 23:18:06
━━━━━━━━━━━━
验证码：239542，短信验证码2分钟内有效，请勿泄漏给他人。【中国联通】
```

## 硬件

| 外设 | 引脚 | 说明 |
|---|---|---|
| A7670E UART | TX=GPIO18, RX=GPIO17 | 115200 波特率，与模组 AT 通信 |
| A7670E PWRKEY | GPIO4 | 开机控制 |
| A7670E RESET | GPIO5 | 复位控制 |

烧录/日志走板载 USB 转 UART 芯片（CH9102），支持 460800 波特率烧录。

## 目录结构

```
├── main/
│   ├── main.c               # 主流程：任务编排、时区换算、SNTP
│   ├── sms.c / sms.h        # A7670E 短信接收（AT 指令、UCS2 解码）
│   ├── feishu_api.c / .h    # 飞书 Webhook 发送（手写 JSON，无外部依赖）
│   ├── ota_update.c / .h    # GitHub Release OTA 升级
│   ├── wifi_smartconfig.c / .h  # SmartConfig 配网
│   ├── secrets.h            # 敏感配置（不入库）
│   └── secrets.h.example    # 敏感配置模板
├── scripts/release.sh       # 固件发布脚本（GitHub Release + OTA）
└── partitions.csv           # 双 OTA 分区表
```

## 快速开始

### 1. 环境

- ESP-IDF **v6.x**（本项目在 v6.1 开发验证）
- VSCode ESP-IDF 扩展或命令行均可

命令行构建（EIM 安装方式）：

```bash
. ~/.espressif/tools/activate_idf_v6.1.sh
```

### 2. 配置敏感信息

```bash
cp main/secrets.h.example main/secrets.h
# 编辑 main/secrets.h，填入：
#   FEISHU_WEBHOOK_URL   飞书群机器人 Webhook 地址
#   OTA_RELEASE_API_URL  GitHub Release API 地址（用于 OTA）
```

### 3. 编译、烧录、看日志

```bash
idf.py build
idf.py -p /dev/cu.wchusbserialXXXX flash
idf.py -p /dev/cu.wchusbserialXXXX monitor   # 退出: Ctrl + ]
```

### 4. 配网

首次启动用手机 **ESPTouch App**（iOS/Android 应用商店搜"乐鑫 EspTouch"）配置 WiFi，之后自动连接。

## 飞书机器人配置

1. 飞书群 → 设置 → 群机器人 → 添加**自定义机器人**
2. 复制 Webhook 地址填入 `main/secrets.h` 的 `FEISHU_WEBHOOK_URL`
3. 重新编译烧录即可

## 固件发布与 OTA 升级

设备每 24 小时（启动 2 分钟后首查）访问 GitHub Release 检查更新，版本号不同即自动升级。

发布新版本：

```bash
./scripts/release.sh 1.0.1     # 依赖 gh CLI 已登录
```

脚本会：更新版本号并提交 → 构建 → 创建 Release（tag `v1.0.1`）并上传 `sms.bin`。

已部署的设备在下次检查时自动下载升级，无需接线。

## 常见问题

**烧录报 `Failed to write to target RAM (Checksum error)`**
macOS 内置 CDC 驱动对 CH9102 大块传输丢包。安装 WCH 官方驱动：
`brew install --cask wch-ch34x-usb-serial-driver`，然后在
系统设置 → 通用 → 登录项与扩展 → 驱动程序扩展 中启用，重插 USB。
装好后设备名从 `cu.usbmodem*` 变为 `cu.wchusbserial*`。

**烧录报串口 `Resource temporarily unavailable`**
monitor 占着串口，先 `Ctrl + ]` 退出 monitor 再烧录。

**`Failed to resolve component 'json'`**
ESP-IDF v6.x 移除了 json 组件，本项目已手写 JSON 无需该依赖，请确认使用 v6.x 构建。

**设备不转发短信，日志无 `+CMTI`**
多为蜂窝未注册：看日志中 `网络未注册 (stat=...)`。`stat=0 未搜索` 时固件会自动执行 `AT+CFUN=1` 重开射频；若持续失败检查天线和 SIM 卡。

## 许可

私人项目，未配置开源许可。
