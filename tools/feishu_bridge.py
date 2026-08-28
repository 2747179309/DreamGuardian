# -*- coding: utf-8 -*-
"""Feishu-to-DreamGuardian bridge.

Run this on a PC. It receives Feishu bot messages through Feishu WebSocket
callback and forwards recognized commands to the ESP32 local HTTP API.
"""

from __future__ import annotations

import argparse
import json
import os
import unicodedata
import sys
import time
from dataclasses import dataclass, field
from typing import Any

import requests

try:
    import websocket
except ImportError:
    websocket = None


FEISHU_API_BASE = "https://open.feishu.cn/open-apis"
FEISHU_WS_CONFIG_URL = "https://open.feishu.cn/callback/ws/endpoint"
FEISHU_TOKEN_URL = FEISHU_API_BASE + "/auth/v3/tenant_access_token/internal"
FEISHU_SEND_MSG_URL = FEISHU_API_BASE + "/im/v1/messages"
BRIDGE_VERSION = "2026-07-31-local-http-qwen-v1"

COMMAND_ALIASES = {
    "sleep": "sleep",
    "sleep_assist.start": "sleep",
    "sleep_assist_start": "sleep",
    "sleep.start": "sleep",
    "sleep_start": "sleep",
    "start_sleep": "sleep",
    "stop": "stop",
    "sleep_assist.stop": "stop",
    "sleep_assist_stop": "stop",
    "sleep.stop": "stop",
    "sleep_stop": "stop",
    "stop_sleep": "stop",
    "wake": "stop",
    "wake_up": "stop",
    "wakeup": "stop",
    "selftest": "selftest",
    "self_test": "selftest",
    "light.red": "light_red",
    "light_red": "light_red",
    "light.green": "light_green",
    "light_green": "light_green",
    "light.blue": "light_blue",
    "light_blue": "light_blue",
    "light.off": "light_off",
    "light_off": "light_off",
    "screen.red": "screen_red",
    "screen_red": "screen_red",
    "screen.green": "screen_green",
    "screen_green": "screen_green",
    "screen.blue": "screen_blue",
    "screen_blue": "screen_blue",
    "screen.off": "screen_off",
    "screen_off": "screen_off",
    "audio.music": "audio_music",
    "audio_music": "audio_music",
    "music": "audio_music",
    "audio.noise": "audio_noise",
    "audio_noise": "audio_noise",
    "noise": "audio_noise",
    "audio.breathing": "audio_breathing",
    "audio_breathing": "audio_breathing",
    "breathing": "audio_breathing",
    "audio.volume_max": "audio_volume_max",
    "audio_volume_max": "audio_volume_max",
    "volume.max": "audio_volume_max",
    "volume_max": "audio_volume_max",
    "audio.volume_up": "audio_volume_up",
    "audio_volume_up": "audio_volume_up",
    "volume.up": "audio_volume_up",
    "volume_up": "audio_volume_up",
    "audio.volume_down": "audio_volume_down",
    "audio_volume_down": "audio_volume_down",
    "volume.down": "audio_volume_down",
    "volume_down": "audio_volume_down",
    "audio.mute": "audio_mute",
    "audio_mute": "audio_mute",
    "mute": "audio_mute",
    "story": "story",
    "story.play": "story",
    "scene.ppm": "scene_ppm",
    "scene_ppm": "scene_ppm",
    "scene.ocean": "scene_ocean",
    "scene_ocean": "scene_ocean",
    "scene.forest": "scene_forest",
    "scene_forest": "scene_forest",
    "scene.rain": "scene_rain",
    "scene_rain": "scene_rain",
    "scene.zen": "scene_zen",
    "scene_zen": "scene_zen",
    "scene.empty": "scene_empty",
    "scene_empty": "scene_empty",
    "breath.relax": "breath_relax",
    "breath_relax": "breath_relax",
    "breath.box": "breath_box",
    "breath_box": "breath_box",
    "breath.deep": "breath_deep",
    "breath_deep": "breath_deep",
    "mid_sleep.test_minor": "mid_sleep_test_minor",
    "mid_sleep_test_minor": "mid_sleep_test_minor",
    "mid_sleep.test_restless": "mid_sleep_test_restless",
    "mid_sleep_test_restless": "mid_sleep_test_restless",
    "mid_sleep.test_arousal": "mid_sleep_test_arousal",
    "mid_sleep_test_arousal": "mid_sleep_test_arousal",
    "mid_sleep.test_out_of_bed": "mid_sleep_test_out_of_bed",
    "mid_sleep_test_out_of_bed": "mid_sleep_test_out_of_bed",
    "status": "status",
    "none": "none",
}

CLAW_TOOL_BY_COMMAND = {
    "sleep": "sleep_assist.start",
    "stop": "sleep_assist.stop",
    "light_red": "light.red",
    "light_green": "light.green",
    "light_blue": "light.blue",
    "light_off": "light.off",
    "screen_red": "screen.red",
    "screen_green": "screen.green",
    "screen_blue": "screen.blue",
    "screen_off": "screen.off",
    "audio_music": "audio.music",
    "audio_noise": "audio.noise",
    "audio_breathing": "audio.breathing",
    "audio_volume_max": "audio.volume_max",
    "audio_volume_up": "audio.volume_up",
    "audio_volume_down": "audio.volume_down",
    "audio_mute": "audio.mute",
    "story": "story.play",
    "scene_ppm": "scene.ppm",
    "scene_ocean": "scene.ocean",
    "scene_forest": "scene.forest",
    "scene_rain": "scene.rain",
    "scene_zen": "scene.zen",
    "scene_empty": "scene.empty",
    "breath_relax": "breath.relax",
    "breath_box": "breath.box",
    "breath_deep": "breath.deep",
    "mid_sleep_test_minor": "mid_sleep.test_minor",
    "mid_sleep_test_restless": "mid_sleep.test_restless",
    "mid_sleep_test_arousal": "mid_sleep.test_arousal",
    "mid_sleep_test_out_of_bed": "mid_sleep.test_out_of_bed",
}

LLM_ROUTER_PROMPT = (
    "You are a command router for a DreamGuardian sleep device. "
    "Read the user's Chinese or English message and return ONLY compact JSON: "
    "{\"command\":\"...\",\"reply\":\"...\"}. "
    "Allowed command values are: sleep, stop, selftest, status, light.red, light.green, "
    "light.blue, light.off, screen.red, screen.green, screen.blue, screen.off, audio.music, audio.noise, "
    "audio.breathing, audio.volume_max, audio.volume_up, audio.volume_down, audio.mute, story, scene.ppm, scene.ocean, scene.forest, scene.rain, "
    "scene.zen, scene.empty, breath.relax, breath.box, breath.deep, "
    "mid_sleep.test_minor, mid_sleep.test_restless, mid_sleep.test_arousal, "
    "mid_sleep.test_out_of_bed, none. "
    "Map bedtime, sleep aid, hypnotic, sleep intervention, good night, or start sleeping "
    "to sleep. Map stop, cancel, wake up, waking up, time to wake up, get up, "
    "already awake, exit sleep, Chinese 醒了/睡醒了/该睡醒了/该起床了/起床了 to stop. "
    "Map LED, light strip, lamp, light color, red/green/blue light, or turning the "
    "light red/green/blue/off to light.red, light.green, light.blue, or light.off. "
    "Map LCD, display, screen color, red/green/blue screen, or screen off to "
    "screen.red, screen.green, screen.blue, or screen.off. "
    "Map volume maximum/louder/turn up volume/quieter/turn down volume/mute/silent "
    "to audio.volume_max, audio.volume_up, audio.volume_down, or audio.mute. "
    "Map asking if the device is alive or sensor status to status. "
    "For sleep advice or casual chat without a device action, use none. "
    "Never invent commands."
)


def zh(text: str) -> str:
    return text.encode("utf-8").decode("unicode_escape")


@dataclass
class BridgeConfig:
    feishu_app_id: str
    feishu_app_secret: str
    device_base_url: str = "http://192.0.2.1"
    reply_enabled: bool = True
    reconnect_delay_sec: int = 10
    llm_enabled: bool = False
    llm_base_url: str = "https://dashscope.aliyuncs.com/compatible-mode/v1"
    llm_api_key: str = ""
    llm_model: str = "qwen3.5-omni-flash"
    llm_timeout_sec: int = 20
    use_claw_tools: bool = True

    @classmethod
    def load(cls, path: str, args: argparse.Namespace | None = None) -> "BridgeConfig":
        data: dict[str, Any] = {}
        if os.path.exists(path):
            with open(path, "r", encoding="utf-8") as f:
                data = json.load(f)
        app_id = (args.app_id if args and args.app_id else None) or os.getenv("FEISHU_APP_ID") or data.get("feishu_app_id", "")
        app_secret = (args.app_secret if args and args.app_secret else None) or os.getenv("FEISHU_APP_SECRET") or data.get("feishu_app_secret", "")
        device_url = (args.device_url if args and args.device_url else None) or os.getenv("DG_DEVICE_URL") or data.get("device_base_url", "http://192.0.2.1")
        llm_api_key = os.getenv("DG_LLM_API_KEY") or data.get("llm_api_key", "")
        if not app_id or not app_secret:
            raise SystemExit("Missing Feishu app credentials. Fill tools/feishu_bridge_config.json.")
        return cls(
            feishu_app_id=app_id,
            feishu_app_secret=app_secret,
            device_base_url=device_url.rstrip("/"),
            reply_enabled=bool(data.get("reply_enabled", True)),
            reconnect_delay_sec=int(data.get("reconnect_delay_sec", 10)),
            llm_enabled=bool(data.get("llm_enabled", False)),
            llm_base_url=str(data.get("llm_base_url", "https://dashscope.aliyuncs.com/compatible-mode/v1")).rstrip("/"),
            llm_api_key=llm_api_key,
            llm_model=str(data.get("llm_model", "qwen3.5-omni-flash")),
            llm_timeout_sec=int(data.get("llm_timeout_sec", 20)),
            use_claw_tools=bool(data.get("use_claw_tools", True)),
        )


@dataclass
class WsHeader:
    key: str = ""
    value: str = ""


@dataclass
class WsFrame:
    seq_id: int = 0
    log_id: int = 0
    service: int = 0
    method: int = 0
    headers: list[WsHeader] = field(default_factory=list)
    payload: bytes = b""

    def header(self, key: str) -> str | None:
        for header in self.headers:
            if header.key == key:
                return header.value
        return None


def read_varint(buf: bytes, pos: int) -> tuple[int, int]:
    value = 0
    shift = 0
    while pos < len(buf) and shift <= 63:
        byte = buf[pos]
        pos += 1
        value |= (byte & 0x7F) << shift
        if byte & 0x80 == 0:
            return value, pos
        shift += 7
    raise ValueError("bad varint")


def skip_field(buf: bytes, pos: int, wire_type: int) -> int:
    if wire_type == 0:
        _, pos = read_varint(buf, pos)
        return pos
    if wire_type == 1:
        return pos + 8
    if wire_type == 2:
        size, pos = read_varint(buf, pos)
        return pos + size
    if wire_type == 5:
        return pos + 4
    raise ValueError(f"unsupported protobuf wire type {wire_type}")


def parse_header(buf: bytes) -> WsHeader:
    pos = 0
    header = WsHeader()
    while pos < len(buf):
        tag, pos = read_varint(buf, pos)
        field_no = tag >> 3
        wire_type = tag & 0x07
        if wire_type != 2:
            pos = skip_field(buf, pos, wire_type)
            continue
        size, pos = read_varint(buf, pos)
        raw = buf[pos : pos + size]
        pos += size
        if field_no == 1:
            header.key = raw.decode("utf-8", errors="replace")
        elif field_no == 2:
            header.value = raw.decode("utf-8", errors="replace")
    return header


def parse_frame(buf: bytes) -> WsFrame:
    pos = 0
    frame = WsFrame()
    while pos < len(buf):
        tag, pos = read_varint(buf, pos)
        field_no = tag >> 3
        wire_type = tag & 0x07
        if field_no == 1 and wire_type == 0:
            frame.seq_id, pos = read_varint(buf, pos)
        elif field_no == 2 and wire_type == 0:
            frame.log_id, pos = read_varint(buf, pos)
        elif field_no == 3 and wire_type == 0:
            frame.service, pos = read_varint(buf, pos)
        elif field_no == 4 and wire_type == 0:
            frame.method, pos = read_varint(buf, pos)
        elif field_no == 5 and wire_type == 2:
            size, pos = read_varint(buf, pos)
            frame.headers.append(parse_header(buf[pos : pos + size]))
            pos += size
        elif field_no == 8 and wire_type == 2:
            size, pos = read_varint(buf, pos)
            frame.payload = buf[pos : pos + size]
            pos += size
        else:
            pos = skip_field(buf, pos, wire_type)
    return frame


def write_varint(value: int) -> bytes:
    out = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        if value:
            byte |= 0x80
        out.append(byte)
        if not value:
            return bytes(out)


def write_field_varint(field_no: int, value: int) -> bytes:
    return write_varint(field_no << 3) + write_varint(value)


def write_field_bytes(field_no: int, data: bytes) -> bytes:
    return write_varint((field_no << 3) | 2) + write_varint(len(data)) + data


def encode_header(header: WsHeader) -> bytes:
    payload = (
        write_field_bytes(1, header.key.encode("utf-8"))
        + write_field_bytes(2, header.value.encode("utf-8"))
    )
    return write_field_bytes(5, payload)


def encode_frame(frame: WsFrame, payload: bytes = b"") -> bytes:
    out = bytearray()
    out += write_field_varint(1, frame.seq_id)
    out += write_field_varint(2, frame.log_id)
    out += write_field_varint(3, frame.service)
    out += write_field_varint(4, frame.method)
    for header in frame.headers:
        out += encode_header(header)
    if payload:
        out += write_field_bytes(8, payload)
    return bytes(out)


def normalize_text(text: str) -> str:
    text = text.strip()
    if text.startswith("@_user_1 "):
        text = text[9:].strip()
    if text.startswith("<at "):
        end = text.rfind(">")
        if end >= 0:
            text = text[end + 1 :].strip()
    return text


def text_has(text: str, *needles: str) -> bool:
    low = text.lower()
    compact = compact_match_text(text)
    for needle in needles:
        n = str(needle)
        if n in text or n.lower() in low:
            return True
        n_compact = compact_match_text(n)
        if n_compact and n_compact in compact:
            return True
    return False


def compact_match_text(text: str) -> str:
    out: list[str] = []
    for ch in text.lower():
        category = unicodedata.category(ch)
        if ch.isspace() or category[0] in ("C", "P", "S"):
            continue
        out.append(ch)
    return "".join(out)


def local_route(text: str) -> tuple[str | None, str]:
    if text_has(text, "stop", "cancel", "wake", "wake up", "wakeup", "awake", "get up",
                zh("\\u505c\\u6b62"), zh("\\u505c\\u4e0b"), zh("\\u7ed3\\u675f"),
                zh("\\u9000\\u51fa"), zh("\\u53d6\\u6d88"), zh("\\u8d77\\u5e8a"),
                zh("\\u8d77\\u5e8a\\u4e86"), zh("\\u8be5\\u8d77\\u5e8a\\u4e86"),
                zh("\\u9192\\u4e86"), zh("\\u7761\\u9192"), zh("\\u7761\\u9192\\u4e86"),
                zh("\\u8be5\\u7761\\u9192\\u4e86"), zh("\\u8be5\\u9192\\u4e86"),
                zh("\\u9192\\u9192"), zh("\\u9192\\u6765"), zh("\\u7761\\u9192\\u5566"),
                zh("\\u8be5\\u7761\\u9192\\u5566"), zh("\\u8d77\\u6765\\u4e86"),
                zh("\\u8be5\\u8d77\\u4e86"), zh("\\u4e0d\\u7761")):
        return "command:stop", zh("\\u5df2\\u53d1\\u9001\\u505c\\u6b62\\u7761\\u7720\\u547d\\u4ee4\\u3002")
    if text_has(text, "sleep", "bedtime", zh("\\u7761\\u7720"), zh("\\u52a9\\u7720"), zh("\\u7761\\u89c9"), zh("\\u665a\\u5b89"), zh("\\u5165\\u7761"), zh("\\u50ac\\u7720")):
        return "command:sleep", zh("\\u5df2\\u53d1\\u9001\\u8fdb\\u5165\\u7761\\u7720\\u547d\\u4ee4\\u3002")
    if text_has(text, "selftest", "self test", zh("\\u81ea\\u68c0")):
        return "command:selftest", zh("\\u5df2\\u53d1\\u9001\\u81ea\\u68c0\\u547d\\u4ee4\\u3002")
    if text_has(text, "status", zh("\\u72b6\\u6001"), zh("\\u72c0\\u614b")):
        return "status", zh("\\u6b63\\u5728\\u8bfb\\u53d6\\u8bbe\\u5907\\u72b6\\u6001\\u3002")

    about_light = text_has(text, "led", "light", zh("\\u706f\\u5e26"), zh("\\u706f"))
    if about_light:
        if text_has(text, "red", zh("\\u7ea2"), zh("\\u7ea2\\u8272"), zh("\\u7ea2\\u706f"), zh("\\u53d8\\u7ea2"), zh("\\u70b9\\u4eae")):
            return "command:light_red", zh("\\u5df2\\u53d1\\u9001\\u706f\\u5e26\\u53d8\\u7ea2\\u547d\\u4ee4\\u3002")
        if text_has(text, "green", zh("\\u7eff"), zh("\\u7eff\\u8272"), zh("\\u7eff\\u706f"), zh("\\u53d8\\u7eff")):
            return "command:light_green", zh("\\u5df2\\u53d1\\u9001\\u706f\\u5e26\\u53d8\\u7eff\\u547d\\u4ee4\\u3002")
        if text_has(text, "blue", zh("\\u84dd"), zh("\\u84dd\\u8272"), zh("\\u84dd\\u706f"), zh("\\u53d8\\u84dd")):
            return "command:light_blue", zh("\\u5df2\\u53d1\\u9001\\u706f\\u5e26\\u53d8\\u84dd\\u547d\\u4ee4\\u3002")
        if text_has(text, "off", zh("\\u5173"), zh("\\u5173\\u95ed"), zh("\\u5173\\u6389"), zh("\\u706d")):
            return "command:light_off", zh("\\u5df2\\u53d1\\u9001\\u5173\\u95ed\\u706f\\u5e26\\u547d\\u4ee4\\u3002")

    about_screen = text_has(text, "lcd", "screen", "display", zh("\\u5c4f\\u5e55"), zh("\\u663e\\u793a"))
    if about_screen:
        if text_has(text, "red", zh("\\u7ea2"), zh("\\u7ea2\\u8272"), zh("\\u53d8\\u7ea2")):
            return "command:screen_red", zh("\\u5df2\\u53d1\\u9001\\u5c4f\\u5e55\\u53d8\\u7ea2\\u547d\\u4ee4\\u3002")
        if text_has(text, "green", zh("\\u7eff"), zh("\\u7eff\\u8272"), zh("\\u53d8\\u7eff")):
            return "command:screen_green", zh("\\u5df2\\u53d1\\u9001\\u5c4f\\u5e55\\u53d8\\u7eff\\u547d\\u4ee4\\u3002")
        if text_has(text, "blue", zh("\\u84dd"), zh("\\u84dd\\u8272"), zh("\\u53d8\\u84dd")):
            return "command:screen_blue", zh("\\u5df2\\u53d1\\u9001\\u5c4f\\u5e55\\u53d8\\u84dd\\u547d\\u4ee4\\u3002")
        if text_has(text, "off", zh("\\u5173"), zh("\\u5173\\u95ed"), zh("\\u5173\\u6389"), zh("\\u6062\\u590d")):
            return "command:screen_off", zh("\\u5df2\\u53d1\\u9001\\u5c4f\\u5e55\\u6062\\u590d\\u547d\\u4ee4\\u3002")

    if text_has(text, "light off", "led off", zh("\\u5173\\u706f"), zh("\\u5173\\u706f\\u5e26"), zh("\\u5173\\u95ed\\u706f"), zh("\\u706f\\u5e26\\u5173\\u95ed")):
        return "command:light_off", zh("\\u5df2\\u53d1\\u9001\\u5173\\u95ed\\u706f\\u5e26\\u547d\\u4ee4\\u3002")
    if text_has(text, "screen off", "display off", zh("\\u5c4f\\u5e55\\u5173\\u95ed"), zh("\\u5173\\u95ed\\u5c4f\\u5e55")):
        return "command:screen_off", zh("\\u5df2\\u53d1\\u9001\\u5c4f\\u5e55\\u5173\\u95ed\\u547d\\u4ee4\\u3002")
    if text_has(text, "noise", zh("\\u767d\\u566a\\u58f0"), zh("\\u566a\\u58f0")):
        return "command:audio_noise", zh("\\u5df2\\u53d1\\u9001\\u64ad\\u653e\\u767d\\u566a\\u58f0\\u547d\\u4ee4\\u3002")
    if text_has(text, "music", zh("\\u97f3\\u4e50")):
        return "command:audio_music", zh("\\u5df2\\u53d1\\u9001\\u64ad\\u653e\\u97f3\\u4e50\\u547d\\u4ee4\\u3002")
    if text_has(text, "mute", "silent", zh("\\u9759\\u97f3"), zh("\\u6ca1\\u58f0"), zh("\\u6ca1\\u58f0\\u97f3"), zh("\\u5173\\u58f0\\u97f3")):
        return "command:audio_mute", zh("\\u5df2\\u53d1\\u9001\\u9759\\u97f3\\u547d\\u4ee4\\u3002")
    about_volume = text_has(text, "volume", "sound", zh("\\u97f3\\u91cf"), zh("\\u58f0\\u97f3"))
    if about_volume:
        if text_has(text, "max", "maximum", "loudest", zh("\\u6700\\u5927"), zh("\\u6700\\u9ad8"), zh("\\u62c9\\u6ee1")):
            return "command:audio_volume_max", zh("\\u5df2\\u53d1\\u9001\\u97f3\\u91cf\\u6700\\u5927\\u547d\\u4ee4\\u3002")
        if text_has(text, "up", "louder", "increase", zh("\\u5927\\u4e00\\u70b9"), zh("\\u8c03\\u9ad8"), zh("\\u52a0\\u5927"), zh("\\u589e\\u5927")):
            return "command:audio_volume_up", zh("\\u5df2\\u53d1\\u9001\\u97f3\\u91cf\\u8c03\\u9ad8\\u547d\\u4ee4\\u3002")
        if text_has(text, "down", "quieter", "decrease", zh("\\u5c0f\\u4e00\\u70b9"), zh("\\u8c03\\u4f4e"), zh("\\u51cf\\u5c0f"), zh("\\u964d\\u4f4e")):
            return "command:audio_volume_down", zh("\\u5df2\\u53d1\\u9001\\u97f3\\u91cf\\u8c03\\u4f4e\\u547d\\u4ee4\\u3002")
        if text_has(text, "mute", "silent", zh("\\u9759\\u97f3"), zh("\\u6ca1\\u58f0"), zh("\\u5173\\u58f0\\u97f3")):
            return "command:audio_mute", zh("\\u5df2\\u53d1\\u9001\\u9759\\u97f3\\u547d\\u4ee4\\u3002")
    if text_has(text, "story", zh("\\u6545\\u4e8b")):
        return "command:story", zh("\\u5df2\\u53d1\\u9001\\u7761\\u524d\\u6545\\u4e8b\\u547d\\u4ee4\\u3002")
    return None, zh("\\u6ca1\\u6709\\u8bc6\\u522b\\u5230\\u8bbe\\u5907\\u547d\\u4ee4\\u3002")


def normalize_command_name(name: str) -> str | None:
    key = (name or "").strip().lower().replace("-", "_")
    return COMMAND_ALIASES.get(key)


def parse_json_object(text: str) -> dict[str, Any] | None:
    try:
        data = json.loads(text)
        return data if isinstance(data, dict) else None
    except json.JSONDecodeError:
        pass
    start = text.find("{")
    end = text.rfind("}")
    if start < 0 or end <= start:
        return None
    try:
        data = json.loads(text[start : end + 1])
        return data if isinstance(data, dict) else None
    except json.JSONDecodeError:
        return None


class Bridge:
    def __init__(self, config: BridgeConfig) -> None:
        self.config = config
        self.session = requests.Session()
        self.device_session = requests.Session()
        self.device_session.trust_env = False
        self.tenant_token = ""
        self.tenant_token_expire = 0.0
        self.seen: set[str] = set()

    def get_ws_config(self) -> dict[str, Any]:
        body = {"AppID": self.config.feishu_app_id, "AppSecret": self.config.feishu_app_secret}
        resp = self.session.post(FEISHU_WS_CONFIG_URL, json=body, timeout=15)
        resp.raise_for_status()
        data = resp.json()
        if data.get("code") != 0:
            raise RuntimeError(f"Feishu ws config failed: {data}")
        return data["data"]

    def get_tenant_token(self) -> str:
        if self.tenant_token and time.time() + 300 < self.tenant_token_expire:
            return self.tenant_token
        body = {"app_id": self.config.feishu_app_id, "app_secret": self.config.feishu_app_secret}
        resp = self.session.post(FEISHU_TOKEN_URL, json=body, timeout=15)
        resp.raise_for_status()
        data = resp.json()
        if data.get("code") != 0:
            raise RuntimeError(f"Feishu tenant token failed: {data}")
        self.tenant_token = data["tenant_access_token"]
        self.tenant_token_expire = time.time() + int(data.get("expire", 7200))
        return self.tenant_token

    def send_reply(self, route_id: str, text: str) -> None:
        if not self.config.reply_enabled or not route_id:
            return
        token = self.get_tenant_token()
        receive_id_type = "open_id" if route_id.startswith("ou_") else "chat_id"
        body = {
            "receive_id": route_id,
            "msg_type": "text",
            "content": json.dumps({"text": text}, ensure_ascii=False),
        }
        url = FEISHU_SEND_MSG_URL + f"?receive_id_type={receive_id_type}"
        resp = self.session.post(url, headers={"Authorization": f"Bearer {token}"}, json=body, timeout=15)
        if resp.status_code < 200 or resp.status_code >= 300:
            print(f"DG Bridge: reply HTTP {resp.status_code} {resp.text[:200]}", flush=True)

    def call_device_command(self, cmd: str) -> str:
        if self.config.use_claw_tools and cmd in CLAW_TOOL_BY_COMMAND:
            return self.call_device_claw_tool(CLAW_TOOL_BY_COMMAND[cmd])
        url = self.config.device_base_url + "/api/command"
        resp = self.device_session.post(url, params={"cmd": cmd}, timeout=8)
        text = resp.text[:240]
        if resp.status_code < 200 or resp.status_code >= 300:
            raise RuntimeError(f"device HTTP {resp.status_code}: {text}")
        try:
            data = resp.json()
        except ValueError as exc:
            raise RuntimeError(f"device returned non-JSON: {text}") from exc
        device_cmd = str(data.get("command", ""))
        code = int(data.get("code", 0))
        result = int(data.get("result", -1))
        if device_cmd != cmd or code == 0 or result != 0:
            raise RuntimeError(f"device rejected cmd={cmd}: {data}")
        return f"endpoint=/api/command cmd={cmd} code={code} result={result}"

    def call_device_claw_tool(self, tool: str) -> str:
        url = self.config.device_base_url + "/api/claw_tool"
        resp = self.device_session.post(url, data={"tool": tool}, timeout=8)
        text = resp.text[:240]
        if resp.status_code < 200 or resp.status_code >= 300:
            raise RuntimeError(f"claw HTTP {resp.status_code}: {text}")
        try:
            data = resp.json()
        except ValueError as exc:
            raise RuntimeError(f"claw returned non-JSON: {text}") from exc
        returned_tool = str(data.get("tool", ""))
        command = int(data.get("command", 0))
        result = int(data.get("result", -1))
        if returned_tool != tool or command == 0 or result != 0:
            raise RuntimeError(f"claw rejected tool={tool}: {data}")
        return f"endpoint=/api/claw_tool tool={tool} command={command} result={result}"

    def call_device_status(self) -> str:
        url = self.config.device_base_url + "/api/status"
        resp = self.device_session.get(url, timeout=8)
        data = resp.json()
        wifi = data.get("wifi", {})
        radar = data.get("radar", {})
        assist = data.get("assist", {})
        return (
            zh("\\u8bbe\\u5907\\u5728\\u7ebf\\u3002")
            + f"WiFi={wifi.get('ssid', '?')} IP={wifi.get('ip', '?')}; "
            + f"radar={radar.get('frames', radar.get('raw_frames', '?'))}; "
            + f"assist={assist.get('state', '?')}"
        )

    def llm_route(self, text: str) -> tuple[str | None, str] | None:
        if not self.config.llm_enabled:
            return None
        if not self.config.llm_api_key:
            print("DG Bridge: LLM enabled but llm_api_key is empty", flush=True)
            return None
        url = self.config.llm_base_url
        if not url.endswith("/chat/completions"):
            url = url.rstrip("/") + "/v1/chat/completions"
        body = {
            "model": self.config.llm_model,
            "messages": [
                {"role": "system", "content": LLM_ROUTER_PROMPT},
                {"role": "user", "content": text},
            ],
            "temperature": 0,
            "max_tokens": 160,
        }
        headers = {"Authorization": f"Bearer {self.config.llm_api_key}", "Content-Type": "application/json"}
        try:
            resp = self.session.post(url, headers=headers, json=body, timeout=self.config.llm_timeout_sec)
            if resp.status_code < 200 or resp.status_code >= 300:
                print(f"DG Bridge: LLM HTTP {resp.status_code} {resp.text[:200]}", flush=True)
                return None
            root = resp.json()
            choices = root.get("choices") or []
            content = (((choices[0] or {}).get("message") or {}).get("content") or "") if choices else ""
            parsed = parse_json_object(content)
            if not parsed:
                print(f"DG Bridge: LLM parse failed content={content[:200]}", flush=True)
                return None
            cmd = normalize_command_name(str(parsed.get("command", "")))
            reply = str(parsed.get("reply", "") or "")
            print(f"DG Bridge: LLM raw={content[:240]} normalized={cmd}", flush=True)
            if cmd == "none":
                return None, reply or zh("\\u6211\\u6536\\u5230\\u4e86\\u4f60\\u7684\\u6d88\\u606f\\uff0c\\u4f46\\u6ca1\\u6709\\u6267\\u884c\\u8bbe\\u5907\\u547d\\u4ee4\\u3002")
            if cmd == "status":
                return "status", reply or zh("\\u6b63\\u5728\\u8bfb\\u53d6\\u8bbe\\u5907\\u72b6\\u6001\\u3002")
            if cmd:
                return f"command:{cmd}", reply or zh("\\u5df2\\u8bc6\\u522b\\u5e76\\u53d1\\u9001\\u8bbe\\u5907\\u547d\\u4ee4\\u3002")
        except Exception as exc:
            print(f"DG Bridge: LLM route failed: {exc}", flush=True)
        return None

    def handle_text(self, route_id: str, message_id: str, text: str) -> None:
        clean = normalize_text(text)
        if not clean:
            return
        if message_id:
            if message_id in self.seen:
                return
            self.seen.add(message_id)
            if len(self.seen) > 128:
                self.seen = set(list(self.seen)[-64:])

        action, reply = local_route(clean)
        source = "local"
        if not action:
            llm_result = self.llm_route(clean)
            if llm_result:
                action, reply = llm_result
                source = "llm"
        print(f"DG Bridge: message={message_id} text={clean} source={source} action={action}", flush=True)
        if not action:
            self.send_reply(route_id, reply)
            return
        try:
            if action == "status":
                reply = self.call_device_status()
            elif action.startswith("command:"):
                cmd = action.split(":", 1)[1]
                result = self.call_device_command(cmd)
                print(f"DG Bridge: {result}", flush=True)
                reply = zh("\\u8bbe\\u5907\\u5df2\\u6267\\u884c\\uff1a") + result
            else:
                reply = zh("\\u672a\\u77e5\\u547d\\u4ee4\\u3002")
        except Exception as exc:
            reply = zh("\\u8bbe\\u5907\\u8c03\\u7528\\u5931\\u8d25\\uff1a") + str(exc)
            print(f"DG Bridge: device call failed: {exc}", flush=True)
        self.send_reply(route_id, reply)

    def process_event_json(self, raw: bytes | str) -> None:
        text = raw.decode("utf-8", errors="replace") if isinstance(raw, bytes) else raw
        try:
            root = json.loads(text)
        except json.JSONDecodeError:
            print(f"DG Bridge: event json parse failed: {text[:200]}", flush=True)
            return
        event = root.get("event") or {}
        header = root.get("header") or {}
        event_type = header.get("event_type", "unknown")
        if event_type not in ("unknown", "im.message.receive_v1"):
            print(f"DG Bridge: ignore event type={event_type}", flush=True)
            return

        message = event.get("message") or {}
        sender = event.get("sender") or {}
        sender_id = ((sender.get("sender_id") or {}).get("open_id")) or ""
        message_id = message.get("message_id", "")
        chat_id = message.get("chat_id", "")
        chat_type = message.get("chat_type", "p2p")
        route_id = sender_id if chat_type == "p2p" and sender_id else chat_id
        if message.get("message_type", "text") != "text":
            self.send_reply(route_id, zh("\\u76ee\\u524d\\u53ea\\u652f\\u6301\\u6587\\u672c\\u547d\\u4ee4\\u3002"))
            return
        content = message.get("content", "{}")
        try:
            content_json = json.loads(content)
        except json.JSONDecodeError:
            content_json = {}
        self.handle_text(route_id, message_id, content_json.get("text", ""))

    def on_message(self, ws: Any, message: bytes | str) -> None:
        if isinstance(message, bytes):
            try:
                frame = parse_frame(message)
            except Exception as exc:
                print(f"DG Bridge: binary frame parse failed: {exc}", flush=True)
                return
            msg_type = frame.header("type")
            if frame.method == 0:
                return
            if msg_type != "event" or not frame.payload:
                print(f"DG Bridge: ignore frame method={frame.method} type={msg_type}", flush=True)
                return
            ack = encode_frame(frame, b'{"code":200}')
            ws.send(ack, opcode=websocket.ABNF.OPCODE_BINARY)
            self.process_event_json(frame.payload)
            return
        self.process_event_json(message)

    def on_open(self, ws: Any) -> None:
        print("DG Bridge: Feishu WebSocket connected", flush=True)

    def on_error(self, ws: Any, error: Exception) -> None:
        print(f"DG Bridge: WebSocket error: {error}", flush=True)

    def on_close(self, ws: Any, code: int, reason: str) -> None:
        print(f"DG Bridge: WebSocket closed code={code} reason={reason}", flush=True)

    def run_forever(self) -> None:
        if websocket is None:
            raise SystemExit("Missing dependency: run tools\\install_feishu_bridge_deps.bat first.")
        print(f"DG Bridge: version={BRIDGE_VERSION}", flush=True)
        if self.config.llm_enabled:
            print(f"DG Bridge: LLM enabled model={self.config.llm_model} base={self.config.llm_base_url}", flush=True)
        while True:
            try:
                ws_cfg = self.get_ws_config()
                ws_url = ws_cfg["URL"]
                print(f"DG Bridge: device={self.config.device_base_url}", flush=True)
                print("DG Bridge: connecting Feishu WebSocket...", flush=True)
                app = websocket.WebSocketApp(
                    ws_url,
                    on_open=self.on_open,
                    on_message=self.on_message,
                    on_error=self.on_error,
                    on_close=self.on_close,
                )
                app.run_forever(ping_interval=120, ping_timeout=30)
            except KeyboardInterrupt:
                print("DG Bridge: stopped", flush=True)
                return
            except Exception as exc:
                print(f"DG Bridge: failed: {exc}", flush=True)
            time.sleep(max(3, self.config.reconnect_delay_sec))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", default="tools/feishu_bridge_config.json")
    parser.add_argument("--app-id", default="")
    parser.add_argument("--app-secret", default="")
    parser.add_argument("--device-url", default="")
    args = parser.parse_args()
    config = BridgeConfig.load(args.config, args)
    Bridge(config).run_forever()
    return 0


if __name__ == "__main__":
    sys.exit(main())
