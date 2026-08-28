# DreamGuardian 鼾声现场演示音频

现场优先播放 `snore-demo-loud-recommended.wav`。它使用真实鼾声素材，
只做了响度压缩以提高手机扬声器远场回放的稳定性；
`snore-demo-recommended.wav` 是动态范围更自然的备用版本。
`snore-demo-quick-test.wav` 是约 9 秒的快速联调版本，适合比赛前确认一次
“检测—计数—飞书发送”链路。

- 格式：16 kHz、16-bit、单声道 PCM WAV
- 时长：约 34 秒
- 结构：前 6 秒静音校准，之后重复 10 次真实鼾声，每次之间保留约 0.85 秒间隔
- 来源素材：MediaCollege `snore-02.wav`
- 来源页面：https://www.mediacollege.com/downloads/sound-effects/people/snoring/
- 授权：Public Domain（来源页面标注为 PD）

## 使用方法

1. 设备进入助眠模式并点击“深睡眠”。
2. 等待串口出现 `DG Snore: analysis=1 playback_gate=0`。
3. 手机媒体音量调到 90%～100%，距离设备麦克风 5～15 cm。
4. 从头播放本文件，不要跳过前 6 秒校准段。
5. 串口应出现 `DG Snore: candidate event=...`，随后出现飞书入队与 HTTP 200 日志。

若没有触发，先确认 `playback_gate=0`，再缩短手机和麦克风之间的距离；不要通过降低检测阈值来解决现场音量不足。
