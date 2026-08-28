# DreamGuardian

DreamGuardian 是一套基于 ESP32-S3 的多模态睡眠监测与自适应干预科研原型。本仓库保存原版 DreamGuardian 原型机的软件实现，用于记录项目研发经历、嵌入式系统设计和实验验证过程。

> 本项目是科研与教学原型，不属于医疗器械，输出结果不能用于疾病诊断或替代专业睡眠监测。

## 主要功能

- 毫米波雷达心率、呼吸、在床状态与体动信息接入
- Wi-Fi CSI 辅助体动分析
- 语音唤醒、语音控制和在线语义理解
- 助眠声音、灯光与分级睡眠干预
- 鼾声候选事件检测
- 离床夜灯与回床助眠流程
- 飞书消息、周期状态和睡眠报告
- ESP32-S3-BOX-3 触摸屏交互

## 系统结构

```text
毫米波雷达 / Wi-Fi CSI / 麦克风
                 |
                 v
           ESP32-S3-BOX-3
        感知融合、状态机、干预控制
          |          |          |
          v          v          v
       声音播放     灯光控制     飞书智能体
```

## 目录说明

- `main/`：应用入口和系统任务编排
- `components/`：传感器、算法、语音、云端、界面和干预模块
- `spiffs/`：设备运行所需的文件系统资源
- `demo_audio/`：鼾声检测演示素材
- `tools/`：飞书桥接、分析和调试工具
- `sdkconfig.defaults`：ESP-IDF 默认构建配置
- `dependencies.lock`：ESP-IDF 组件依赖版本锁定

`managed_components/` 不纳入版本控制。首次配置工程时，ESP-IDF Component Manager 会根据组件清单和依赖锁文件下载第三方组件。

## 环境要求

- ESP-IDF 5.5.4
- ESP32-S3-BOX-3
- Python 3.11（辅助工具）
- 16 MB Flash 配置

## 编译

在已经导入 ESP-IDF 环境的 PowerShell 中执行：

```powershell
git clone https://github.com/2747179309/DreamGuardian.git
cd DreamGuardian
idf.py set-target esp32s3
idf.py build
```

烧录前请根据实际串口号修改 `COM9`：

```powershell
idf.py -p COM9 flash monitor
```

## 本地配置

飞书电脑桥接工具使用本地配置文件。先复制模板：

```powershell
Copy-Item tools\feishu_bridge_config.example.json tools\feishu_bridge_config.json
```

然后只在 `tools/feishu_bridge_config.json` 中填写自己的飞书凭据、设备地址和模型密钥。该文件已被 `.gitignore` 排除，不应提交到仓库。

设备热点密码、BLE 设备地址和其他部署参数在公开版本中均为占位值，使用者需要在本地按自己的设备环境配置。

## 隐私与安全

仓库不包含：

- Wi-Fi 名称和密码
- 飞书 App Secret、Webhook 和访问令牌
- 大模型与语音识别 API Key
- NVS Flash 镜像
- 串口运行日志
- 个人唤醒录音和本地训练模板
- 编译生成的固件与构建目录

提交代码前请检查 `git status`，不要绕过 `.gitignore` 强制加入本地配置文件。

## 研究说明

系统通过多传感器趋势融合估计睡眠状态和苏醒风险。心率、呼吸、体动、鼾声及睡眠阶段均属于工程估计结果，不应被表述为临床诊断结论。

