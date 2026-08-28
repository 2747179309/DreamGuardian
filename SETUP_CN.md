# DreamGuardian 2 开发环境与项目导览

## 项目定位

DreamGuardian 2 是面向 ESP32-S3-BOX-3 的床旁睡眠守护固件。主控通过 HLK-LD6002 毫米波雷达感知存在、距离、呼吸、心率和体动，并结合麦克风、扬声器、LCD、触摸屏和灯带完成睡眠监测、声光助眠、夜间干预、语音交互与联网控制。

固件入口为 `main/app_main.c`，主要模块位于 `components/`：

- `sensors`、`bio_filter`：雷达采集与生命体征滤波；
- `algorithm`、`intervention`、`sleep_assist`：睡眠状态、评分和干预策略；
- `audio`、`audio_monitor`、`voice_sr`、`voice_prompt`：播放、采音、WakeNet 唤醒和提示音；
- `ui`、`lighting`：LCD/触摸屏和灯带；
- `cloud`、`feishu_agent`、`ai_bridge`：Web、飞书和外部智能体控制；
- `online_asr`、`online_chat`、`llm_intent`：在线语音识别、对话和意图路由；
- `storage`、`time_sync`、`csi`：日志、时间同步和 Wi-Fi CSI。

`spiffs/` 保存随固件打包的网页、唤醒模板和音频资源。`dependencies.lock` 将目标锁定为 `esp32s3`、ESP-IDF 5.5.0 兼容依赖；本机使用 ESP-IDF 5.5.4。

## 已配置的开发环境

- ESP-IDF 5.5.4：`C:\Espressif\esp-idf-v5.5.4`
- ESP-IDF 工具目录：`C:\Espressif\tools`
- Python 3.11（ESP-IDF 专用环境）
- CMake 3.30.2、Ninja 1.12.1、Xtensa GCC 14.2.0
- VS Code 与 Espressif IDF、C/C++、串口监视器扩展
- 项目辅助脚本虚拟环境：`.venv`

## 构建

在 `dreamguardian2` 目录运行：

```bat
build_ascii.cmd
```

脚本会优先使用已经激活的 `IDF_PATH`，否则自动查找常见的 ESP-IDF 5.5 安装位置。构建输出位于 `build_local/`。

PowerShell 也可以使用：

```powershell
.\tools\build_ascii.ps1
```

连接开发板后，根据设备管理器中的端口选择正确串口（下面的 `COM9` 仅为示例）：

```powershell
.\tools\build_ascii.ps1 -Flash -Monitor -Port COM9
```

如只刷写应用分区、希望保留设备已有的 NVS、SPIFFS 和唤醒模板，应使用已有说明中的 `esptool write_flash 0x10000 build_local\dreamguardian2.bin` 方式；执行前必须再次确认芯片、串口和分区表。

## 电脑端辅助脚本

首次运行飞书桥或音频分析工具前：

```bat
tools\install_feishu_bridge_deps.bat
```

依赖会安装到项目自己的 `.venv`。启动飞书桥前，将 `tools/feishu_bridge_config.example.json` 复制为 `tools/feishu_bridge_config.json` 并填写本机配置；真实密钥文件已被 `.gitignore` 排除。

## 硬件与运行注意事项

- 默认目标是 ESP32-S3-BOX-3；已移除旧电脑写死的 `COM12`，首次刷机前必须确认当前开发板串口。
- 完整功能需要 LD6002 雷达、板载 ES8311 音频、LCD/触摸屏和灯带按 `HARDWARE_WIRING.md` 接线。
- 在线 ASR、在线对话和飞书功能需要 Wi-Fi 与相应 API 密钥；密钥应通过设备配置页、串口命令或本地配置文件写入，不要提交到源码。
- 设备端 RAM 较紧张，在线对话任务已采用延迟创建和 PSRAM 优先策略；增加常驻任务时要继续关注内部 RAM。
- `espclaw/esp-claw` 是独立的 ESP-Claw 上游工程/参考实现，不是构建 DreamGuardian 2 固件的直接依赖。
