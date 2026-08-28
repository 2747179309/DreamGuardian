# BLE 肌肉按摩仪控制模块

这是一个独立的 ESP-IDF/NimBLE Central 组件。协议参数已根据
`协议-提示词-电脑测试` 中的 PC Bleak 实测结果更新：设备名 `DF000001`、
目标地址 `00:00:00:00:00:00`、FFF0/FFF1/FFF2、`5A CMD P1 P2 P3 A5`
帧格式以及强度/启动/停止命令。周期状态包仍只做原始 HEX 记录，不强行解释。

组件不会修改 DreamGuardian 现有 `main.c`，也不会在连接后自动启动按摩。
`examples/massage_main.c` 是独立测试工程的参考 `app_main`，不是现有固件的入口。

推荐 ESP-IDF v5.1 或更高版本（当前工程为 v5.5.4），并在 menuconfig 中启用
Bluetooth Controller、NimBLE Host。实际按摩设备测试前必须先完成协议抓包和
人体安全验证。
