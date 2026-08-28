# DreamGuardian Sleep 模式完整流程说明

本文档描述当前固件中，点击屏幕 `SLEEP`、语音/飞书发送睡眠命令后，设备进入 sleep 模式的完整工作流程、状态含义、声光行为、退出条件，以及 LCD/串口中常见状态字段的解释。

对应代码位置：

- 助眠流程状态机：`components/sleep_assist/sleep_assist.c`
- 助眠状态定义：`components/sleep_assist/include/sleep_assist.h`
- 主循环命令处理：`main/app_main.c`
- 睡眠质量评估：`components/algorithm/sleep_score.c`
- LCD 状态显示：`components/ui/sleep_ui.c`
- 睡眠报告统计：`components/storage/sleep_log.c`

## 1. 进入 Sleep 的触发方式

以下方式都会进入 sleep 模式：

- 屏幕点击 `SLEEP`
- 飞书/HTTP/ESP-Claw 发送 `sleep` 或 `sleep_assist.start`
- 唤醒后识别到语音睡眠命令，例如“睡觉了”“开始睡觉”等
- 串口控制台发送 `sleep`

进入 sleep 后，主程序会执行：

1. 清除手动灯光、手动屏幕、手动音频状态。
2. 清除中途睡眠干预状态。
3. 重置本次语音提示状态。
4. 调用 `sleep_assist_start()` 启动助眠状态机。
5. 如果当前没有睡眠会话，则启动一段新的睡眠统计会话。
6. LCD 切换到 sleep assist 视觉界面。
7. 语音助手进入抑制状态：不再响应唤醒词和普通语音命令，mic 继续用于睡眠监测路径。

串口通常会看到：

```text
DG Sleep: start source=... session=new
DG Voice: sleep mode active, wake/commands ignored; mic remains in monitor path
```

## 2. Sleep 后的总体阶段

当前默认流程如下：

| 阶段 | 默认时长 | 主要目的 | 声光行为 |
|---|---:|---|---|
| `baseline` | 60 秒 | 采集个人基线：呼吸、心率、体动 | 轻微声光，建立初始状态 |
| `settle` | 4 分钟 | 安定阶段，让用户放松下来 | 温暖呼吸灯、轻柔音频 |
| `entrain` | 12 分钟 | 主助眠阶段，引导呼吸逐步靠近目标节奏 | 声音/灯光按呼吸节奏缓慢变化 |
| `fade` | 4 分钟 | 淡出阶段 | 音量、灯光、屏幕逐步减弱 |
| `sleep_lock` | 持续监测 | 认为用户已稳定入睡 | 主动声光关闭，只保留监测 |

完整默认节奏约 `1 + 4 + 12 + 4 = 21 分钟`。如果系统更早判断用户稳定入睡，会提前进入 `sleep_lock`。如果始终无法判断稳定入睡，最长 `30 分钟` 兜底进入 `sleep_lock`。

## 3. 助眠流程状态 `sleep_assist.state`

这是“设备正在做什么”的状态，来自 `sleep_assist_state_t`。

| 状态 | 含义 | 典型表现 |
|---|---|---|
| `off` | 未处于助眠模式 | 声光助眠关闭，主屏幕正常显示 |
| `baseline` | 基线采集阶段 | 采集人体存在、呼吸、心率、体动数据 |
| `settle` | 安定阶段 | 声光逐步引导用户放松 |
| `entrain` | 呼吸引导阶段 | 根据目标呼吸节奏控制灯光和音频 |
| `fade` | 淡出阶段 | 声音、灯光、屏幕逐步减弱 |
| `sleep_lock` | 稳定睡眠锁定 | 声光关闭，进入睡眠监测和弱干预准备状态 |
| `abort` | 助眠中止 | 通常由用户交互、离床或异常条件触发，随后会 stop |

状态转移主线：

```text
off
  -> baseline
  -> settle
  -> entrain
  -> fade
  -> sleep_lock
```

异常或用户干预时：

```text
baseline/settle/entrain/fade
  -> abort
  -> off
```

## 4. 睡眠评估状态 `sleep_state`

这是“系统判断用户现在睡眠状态如何”的状态，来自 `sleep_state_t`。它和 `sleep_assist.state` 不是一回事。

| 状态 | 含义 | 判断依据概念 |
|---|---|---|
| `unknown` | 数据质量不够，无法判断 | 雷达/生物信号不稳定或缺失 |
| `awake` | 清醒或早期放松 | 体动、呼吸、心率还不像睡着 |
| `transition` | 入睡过渡 | 比清醒稳定，但还没有明显浅睡/深睡趋势 |
| `light_trend` | 浅睡趋势 | 体动降低，呼吸/心率较稳定 |
| `deep_trend` | 深睡趋势 | 体动很低，呼吸/心率稳定性更高 |
| `out_of_bed` | 离床或无人 | 雷达判断无人或人体目标丢失 |

简单区分：

- `sleep_assist.state`：设备助眠流程处于哪个阶段。
- `sleep_state`：用户当前睡眠质量/人体状态判断。

## 5. 声音播放逻辑

进入 sleep 后，不是简单播放一首固定音乐，而是由 `sleep_assist` 状态机按需打开当前助眠场景音频。

当前默认配置：

- 助眠场景音乐最长播放参数：`1800 秒`，即 `30 分钟`
- 播放方式：循环播放当前 sleep scene
- 实际停止时间：由 `sleep_assist.state` 决定

实际行为：

1. `baseline/settle/entrain/fade` 阶段，音频可能开启。
2. `fade` 阶段音量逐步降低。
3. 进入 `sleep_lock` 后，主动音频关闭。
4. 收到 `stop`、`停止睡眠`、`该睡醒了` 后立即静音。
5. 如果中途发生自动干预，例如苏醒风险、离床回床，可能再次短时间播放弱音频。

当前助眠音频强度会被状态机限制，最大助眠音量上限是内部浮点 `0.12`，不是系统全局音量的 100%。所以“音量调最大”提高的是用户音量系数，但助眠阶段仍会受助眠安全上限约束。

## 6. 灯光和屏幕逻辑

### 灯带

灯带在助眠阶段使用暖色呼吸节奏：

- 初期偏暖橙红。
- 随流程推进逐步变暗。
- 亮度不是跳变，而是按呼吸曲线缓慢亮起、缓慢暗下去。
- `fade` 阶段进一步淡出。
- `sleep_lock` 后灯带关闭。
- `stop` 后强制熄灭灯带。

### LCD 屏幕

LCD 在助眠阶段显示 sleep assist 视觉界面：

- 前 30 秒显示辅助文字。
- 之后主要保留呼吸视觉图案。
- 亮度随助眠阶段变化。
- `fade` 阶段逐步变暗。
- `sleep_lock` 后关闭主动助眠屏幕。
- `stop` 后短暂黑屏，然后回到主屏幕/时钟状态。

## 7. 提前进入 `sleep_lock` 的条件

系统会持续累计 `sleep_candidate_sec`。当用户状态满足“可能已经稳定入睡”并持续一段时间后，会提前进入 `sleep_lock`。

当前主要判断概念：

- 有人体存在。
- 雷达/生物数据有效。
- 体动较低。
- 呼吸趋势稳定。
- 心率趋势稳定或低于基线。
- 稳定性评分达到要求。
- 没有用户交互。

默认需要累计约 `4 分钟` 的稳定入睡候选时间。配置会限制在 `3-5 分钟` 范围。

## 8. 退出 Sleep 的方式

以下方式会退出 sleep 模式：

- 屏幕触摸 stop 或 sleep 状态下点击退出。
- 飞书/HTTP/ESP-Claw 发送 `stop` 或 `sleep_assist.stop`。
- 语义命令：“停止睡眠”“该睡醒了”“睡醒了”“起床了”等。
- 串口发送 `stop`。
- 助眠过程中发生用户交互或异常时，可能进入 `abort` 后自动 stop。

退出时会执行：

1. 生成本次睡眠报告。
2. 推送报告到飞书 webhook。
3. 清除中途干预状态。
4. 停止助眠状态机。
5. 恢复语音助手。
6. 静音音频。
7. 强制熄灭灯带。
8. LCD 短暂黑屏后回到主界面。

串口通常会看到：

```text
DG Sleep: stop begin
DG Voice: assistant wake/commands resumed
DG Sleep: stop force light off err=0
DG Sleep: stopped, outputs muted
DG SleepSession: finished ...
DG SleepSession: feishu webhook push err=0
```

## 9. Sleep 后的监测和干预阶段

进入 `sleep_lock` 后，设备不再持续播放催眠声光，而是进入睡眠质量监测阶段：

1. 雷达继续检测人体存在、呼吸、心率、距离、体动。
2. CSI 继续提供 Wi-Fi 环境稳定性、运动 proxy、呼吸 proxy 辅助信息。
3. mic 在 sleep 中不再作为语音命令入口，主要用于睡眠监测路径，例如打呼噜/环境声等。
4. 系统持续计算睡眠评分、苏醒风险、稳定睡眠指数。
5. 如果检测到中途苏醒风险、体动明显、起夜回床等情况，会触发弱干预。

当前弱干预示例：

| 场景 | 行为 |
|---|---|
| 小幅波动 | 非常弱的声光提示，避免过度打扰 |
| 不安/翻身 | 低音量遮蔽音或暖色弱光 |
| 苏醒风险 | 短时间助眠音频或弱灯光 |
| 离床/起夜 | 夜灯/路径灯 |
| 回床稳定一段时间 | 进入非常弱的返睡干预 |

## 10. LCD 状态字段含义

主屏幕或状态区域常见字段：

| 显示 | 含义 |
|---|---|
| `B xxx` | 雷达呼吸率，单位 bpm |
| `H xxx` | 雷达心率，单位 bpm |
| `R xxx` | 雷达距离，单位 cm |
| `STATE xxx` | 当前 `sleep_state`，即睡眠评估状态 |
| `SCORE xxx` | 睡眠评分，0-100 |
| `RISK xxx` | 苏醒风险，0-100，越高越容易醒 |
| `SSI xxx` | 稳定睡眠指数，0-100 |
| `RXxxxxx` | 雷达串口收到的字节数 |
| `RAWxxxxx` | 雷达解析到的原始帧数 |
| `OKxxxxx` | 雷达成功解析的有效帧数 |
| `SKILL xxx` | 当前干预/决策动作 |
| `MODE xxx` | 语音控制/系统模式 |
| `MOT x.xx` | 体动指标 |

状态条会在雷达和 CSI 之间轮换显示：

| 显示 | 含义 |
|---|---|
| `RAD WAIT` | 还没有收到雷达串口数据 |
| `RAD RX ...` | 收到雷达字节，但还未形成有效帧 |
| `RAD RAW ...` | 有原始帧，但有效解析还不足 |
| `RAD OK P1 B12 H70` | 雷达有效，`P1` 表示有人，`B` 呼吸，`H` 心率 |
| `CSI WAIT` | CSI 已开启但暂未收到包 |
| `CSI ON Pxxx Mxx` | CSI 工作中，`P` 包数，`M` 运动指标百分比 |
| `CSI OFF` | CSI 未开启 |

## 11. 串口关键日志

### 启动 sleep

```text
DG Command: queued sleep(...)
DG Sleep: start source=queued session=new
DG Voice: sleep mode active, wake/commands ignored; mic remains in monitor path
```

### 雷达状态

```text
DG Radar: rx=687710 last=0x01 raw=40379 ok=23256 unk=17123 err=0/0 type=0x0a13 len=12
```

字段含义：

| 字段 | 含义 |
|---|---|
| `rx` | 雷达 UART 总接收字节数 |
| `last` | 最近收到的一个字节 |
| `raw` | 解析到的雷达原始帧数 |
| `ok` | 成功识别的有效帧数 |
| `unk` | 未知类型帧数 |
| `err` | 校验错误/解析错误 |
| `type` | 最近帧类型 |
| `len` | 最近帧长度 |

### 停止 sleep

```text
DG Sleep: stop begin
DG Sleep: stopped, outputs muted
DG SleepSession: finished quality=...
```

## 12. 飞书睡眠报告内容

退出 sleep 时会生成报告并尝试推送到飞书。报告包括：

- 结束原因
- 总时长
- 睡眠估计时长
- 入睡用时
- 深睡趋势时长
- 浅睡趋势时长
- 清醒/过渡时长
- 离床时长
- 睡眠质量分
- 平均分
- 最高苏醒风险
- 平均呼吸
- 平均心率
- 平均体动
- 离床次数
- 回床次数
- 干预次数
- 夜灯次数
- 睡眠建议

## 13. 当前需要重点观察的状态

调试时建议重点看这几个字段：

| 目标 | 看什么 |
|---|---|
| 雷达是否接通 | `RX` 是否增长 |
| 雷达协议是否解析正常 | `RAW`、`OK` 是否增长 |
| 是否检测到人 | `P1` 或 `human_present=1` |
| 呼吸心率是否可用 | `B`、`H` 是否有合理数值 |
| 是否进入助眠 | `sleep_assist.state` 是否从 `baseline` 开始 |
| 是否稳定入睡 | `sleep_state` 是否从 `awake/transition` 走向 `light_trend/deep_trend` |
| 是否进入睡眠锁定 | `sleep_assist.state=sleep_lock` |
| 是否有苏醒风险 | `RISK` 是否升高 |
| 是否触发干预 | `SKILL` 是否从 `idle.clock` 变为干预动作 |

## 14. 推荐理解方式

可以把当前 sleep 流程理解为四层：

1. 用户入口层：触摸、语音、飞书、HTTP、串口。
2. 助眠执行层：`sleep_assist.state` 控制声光节奏和阶段。
3. 睡眠判断层：`sleep_state`、`SCORE`、`RISK`、`SSI` 判断人的睡眠状态。
4. 统计与上报层：整晚累计分段时间，退出时生成报告并推送飞书。

最关键的一点：

`sleep_lock` 不是关机，也不是停止监测，而是“用户已稳定入睡，主动声光关闭，系统进入低干预监测阶段”。
