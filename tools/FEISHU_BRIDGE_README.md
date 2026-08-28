# Feishu Bridge

This bridge runs on the Windows PC and forwards Feishu assistant messages to the
DreamGuardian device over local HTTP.

## Setup

1. Fill `tools/feishu_bridge_config.json` with the Feishu app ID and secret.
2. Confirm `device_base_url`, for example `http://192.0.2.1`.
3. Run `tools\install_feishu_bridge_deps.bat` once.
4. Run `tools\start_feishu_bridge.bat`.

## ESP-Claw Tool Mode

When `use_claw_tools` is `true`, the bridge calls the device ESP-Claw endpoint
`/api/claw_tool` instead of the simple `/api/command` endpoint.

Examples:

- `sleep` -> `tool=sleep_assist.start`
- `stop` -> `tool=sleep_assist.stop`
- `music` -> `tool=audio.music`
- `noise` -> `tool=audio.noise`

The bridge reply includes the actual endpoint and tool result, so test output
should show `endpoint=/api/claw_tool tool=sleep_assist.start command=1 result=0`
when sleep starts correctly.

## Optional LLM Routing

The bridge first uses local rules for fast commands. If no local rule matches,
it can call an OpenAI-compatible LLM router.

Set these fields:

```json
"llm_enabled": true,
"llm_base_url": "https://dashscope.aliyuncs.com/compatible-mode/v1",
"llm_api_key": "your_api_key",
"llm_model": "qwen3.5-omni-flash",
"use_claw_tools": true
```

The LLM can only return whitelisted device commands; it cannot make arbitrary
HTTP requests.

## Supported Messages

- `睡觉`, `睡眠`, `助眠`, `晚安`, `sleep`: enter sleep mode.
- `停止`, `退出`, `取消`, `起床`, `stop`: stop sleep mode.
- `状态`, `status`: read device status.
- `自检`, `selftest`: run self test.
- `关灯`, `light off`: turn off the light strip.
- `播放音乐`, `music`: play music.
- `白噪声`, `noise`: play noise.
- `故事`, `story`: play bedtime story.

The ESP32 firmware can keep `DG_FEISHU_AGENT_ENABLED 0`; this bridge replaces the
ESP32-side Feishu WebSocket connection.
