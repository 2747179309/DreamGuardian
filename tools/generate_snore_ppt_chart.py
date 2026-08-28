"""Generate a PPT-ready, evidence-labelled snore pulse visualization.

The waveform is read from the actual playback source.  The device did not
store timestamped microphone PCM during the live test, so the two serial
detections are reported as a count and are deliberately not assigned to
specific source pulses.
"""

from __future__ import annotations

import array
import html
import math
import os
import wave
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


ROOT = Path(__file__).resolve().parents[1]
WAV_PATH = ROOT / "demo_audio" / "snore" / "snore-demo-loud-recommended.wav"
OUT_DIR = ROOT / "docs" / "ppt_assets"
PNG_PATH = OUT_DIR / "snore_pulse_curve_live_test.png"
SVG_PATH = OUT_DIR / "snore_pulse_curve_live_test.svg"

WIDTH, HEIGHT = 2400, 1350
BG = "#071526"
PANEL = "#0D2238"
GRID = "#274158"
TEXT = "#F4F8FC"
MUTED = "#9BB0C3"
CYAN = "#34D6E8"
BLUE = "#4F8CFF"
ORANGE = "#FFAD3D"
GREEN = "#51D69A"


def load_audio() -> tuple[int, list[int]]:
    with wave.open(str(WAV_PATH), "rb") as wav:
        if wav.getnchannels() != 1 or wav.getsampwidth() != 2:
            raise ValueError("Expected mono 16-bit PCM WAV")
        rate = wav.getframerate()
        pcm = array.array("h")
        pcm.frombytes(wav.readframes(wav.getnframes()))
    return rate, list(pcm)


def rms_dbfs(samples: list[int], rate: int, frame_ms: int = 20) -> list[float]:
    frame = max(1, rate * frame_ms // 1000)
    result: list[float] = []
    for start in range(0, len(samples) - frame + 1, frame):
        square_sum = sum(value * value for value in samples[start : start + frame])
        rms = math.sqrt(square_sum / frame)
        result.append(20.0 * math.log10(max(rms, 1.0) / 32768.0))
    return result


def font(size: int, bold: bool = False) -> ImageFont.FreeTypeFont:
    candidates = [
        Path("C:/Windows/Fonts/msyhbd.ttc" if bold else "C:/Windows/Fonts/msyh.ttc"),
        Path("C:/Windows/Fonts/simhei.ttf"),
        Path("C:/Windows/Fonts/arialbd.ttf" if bold else "C:/Windows/Fonts/arial.ttf"),
    ]
    for candidate in candidates:
        if candidate.exists():
            return ImageFont.truetype(str(candidate), size)
    return ImageFont.load_default()


def text(draw: ImageDraw.ImageDraw, xy: tuple[float, float], value: str,
         size: int, fill: str = TEXT, bold: bool = False,
         anchor: str | None = None) -> None:
    draw.text(xy, value, font=font(size, bold), fill=fill, anchor=anchor)


def svg_text(parts: list[str], x: float, y: float, value: str, size: int,
             fill: str = TEXT, weight: int = 400, anchor: str = "start") -> None:
    parts.append(
        f'<text x="{x:.1f}" y="{y:.1f}" fill="{fill}" font-size="{size}" '
        f'font-family="Microsoft YaHei, sans-serif" font-weight="{weight}" '
        f'text-anchor="{anchor}">{html.escape(value)}</text>'
    )


def generate() -> None:
    rate, samples = load_audio()
    duration = len(samples) / rate
    envelope = rms_dbfs(samples, rate)
    OUT_DIR.mkdir(parents=True, exist_ok=True)

    image = Image.new("RGB", (WIDTH, HEIGHT), BG)
    draw = ImageDraw.Draw(image)
    svg: list[str] = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{WIDTH}" height="{HEIGHT}" viewBox="0 0 {WIDTH} {HEIGHT}">',
        f'<rect width="{WIDTH}" height="{HEIGHT}" fill="{BG}"/>',
    ]

    text(draw, (120, 74), "鼾声脉冲曲线与设备现场检测结果", 60, bold=True)
    text(draw, (120, 150),
         f"真实播放源波形｜{duration:.1f} s｜16 kHz 单声道｜6 s静音校准 + 10个重复鼾声周期",
         29, MUTED)
    svg_text(svg, 120, 120, "鼾声脉冲曲线与设备现场检测结果", 60, weight=700)
    svg_text(svg, 120, 180,
             f"真实播放源波形｜{duration:.1f} s｜16 kHz 单声道｜6 s静音校准 + 10个重复鼾声周期",
             29, MUTED)

    left, right = 150, 2280
    top1, bottom1 = 255, 625
    top2, bottom2 = 710, 1015
    chart_w = right - left

    for top, bottom in ((top1, bottom1), (top2, bottom2)):
        draw.rounded_rectangle((105, top - 55, 2325, bottom + 35), 26, fill=PANEL)
        svg.append(
            f'<rect x="105" y="{top-55}" width="2220" height="{bottom-top+90}" '
            f'rx="26" fill="{PANEL}"/>'
        )

    text(draw, (140, top1 - 30), "A  播放源时域波形（归一化振幅）", 28, bold=True)
    text(draw, (140, top2 - 30), "B  20 ms短时能量包络（dBFS）", 28, bold=True)
    svg_text(svg, 140, top1 - 18, "A  播放源时域波形（归一化振幅）", 28, weight=700)
    svg_text(svg, 140, top2 - 18, "B  20 ms短时能量包络（dBFS）", 28, weight=700)

    def x_for_time(seconds: float) -> float:
        return left + seconds / duration * chart_w

    # Calibration area and the ten known repeated source periods.
    cal_right = x_for_time(6.0)
    for top, bottom in ((top1, bottom1), (top2, bottom2)):
        draw.rectangle((left, top, cal_right, bottom), fill="#123047")
        svg.append(
            f'<rect x="{left}" y="{top}" width="{cal_right-left:.1f}" '
            f'height="{bottom-top}" fill="#123047"/>'
        )
    text(draw, ((left + cal_right) / 2, top1 + 32), "环境静音校准", 25, MUTED, anchor="mm")
    svg_text(svg, (left + cal_right) / 2, top1 + 39, "环境静音校准", 25, MUTED, anchor="middle")

    clip_seconds = 15654 / 8000.0
    repeat_seconds = clip_seconds + 0.85
    for index in range(10):
        start = 6.0 + index * repeat_seconds
        end = min(duration, start + clip_seconds)
        x0, x1 = x_for_time(start), x_for_time(end)
        shade = "#102D43" if index % 2 == 0 else "#0E283E"
        for top, bottom in ((top1, bottom1), (top2, bottom2)):
            draw.rectangle((x0, top, x1, bottom), fill=shade)
            svg.append(
                f'<rect x="{x0:.1f}" y="{top}" width="{x1-x0:.1f}" '
                f'height="{bottom-top}" fill="{shade}"/>'
            )
        cx = (x0 + x1) / 2
        draw.ellipse((cx - 18, top1 + 53, cx + 18, top1 + 89), fill=BLUE)
        text(draw, (cx, top1 + 71), str(index + 1), 18, bold=True, anchor="mm")
        svg.append(f'<circle cx="{cx:.1f}" cy="{top1+71}" r="18" fill="{BLUE}"/>')
        svg_text(svg, cx, top1 + 78, str(index + 1), 18, weight=700, anchor="middle")

    # Time grid shared by both panels.
    for second in range(0, 35, 2):
        x = x_for_time(min(second, duration))
        for top, bottom in ((top1, bottom1), (top2, bottom2)):
            draw.line((x, top, x, bottom), fill=GRID, width=1)
            svg.append(f'<line x1="{x:.1f}" y1="{top}" x2="{x:.1f}" y2="{bottom}" stroke="{GRID}"/>')
        text(draw, (x, bottom2 + 18), str(second), 19, MUTED, anchor="ma")
        svg_text(svg, x, bottom2 + 28, str(second), 19, MUTED, anchor="middle")
    text(draw, ((left + right) / 2, bottom2 + 58), "时间 / s", 22, MUTED, anchor="ma")
    svg_text(svg, (left + right) / 2, bottom2 + 68, "时间 / s", 22, MUTED, anchor="middle")

    # Actual waveform: min/max bucket polygon preserves visible transients.
    bucket_count = int(chart_w)
    upper: list[tuple[float, float]] = []
    lower: list[tuple[float, float]] = []
    center = (top1 + bottom1) / 2
    half_height = (bottom1 - top1) * 0.40
    for column in range(bucket_count):
        a = column * len(samples) // bucket_count
        b = max(a + 1, (column + 1) * len(samples) // bucket_count)
        block = samples[a:b]
        high, low = max(block) / 32768.0, min(block) / 32768.0
        x = left + column
        upper.append((x, center - high * half_height))
        lower.append((x, center - low * half_height))
    polygon = upper + list(reversed(lower))
    draw.polygon(polygon, fill=CYAN)
    svg_points = " ".join(f"{x:.1f},{y:.1f}" for x, y in polygon)
    svg.append(f'<polygon points="{svg_points}" fill="{CYAN}" opacity="0.92"/>')
    draw.line((left, center, right, center), fill="#B8F5FA", width=2)
    svg.append(f'<line x1="{left}" y1="{center:.1f}" x2="{right}" y2="{center:.1f}" stroke="#B8F5FA" stroke-width="2"/>')

    # Actual source RMS envelope.
    db_min, db_max = -60.0, 0.0
    env_points: list[tuple[float, float]] = []
    for index, value in enumerate(envelope):
        seconds = index * 0.020
        clipped = max(db_min, min(db_max, value))
        x = x_for_time(seconds)
        y = bottom2 - (clipped - db_min) / (db_max - db_min) * (bottom2 - top2)
        env_points.append((x, y))
    draw.line(env_points, fill=ORANGE, width=5, joint="curve")
    env_path = "M " + " L ".join(f"{x:.1f} {y:.1f}" for x, y in env_points)
    svg.append(f'<path d="{env_path}" fill="none" stroke="{ORANGE}" stroke-width="5" stroke-linejoin="round"/>')
    for db in (-60, -40, -20, 0):
        y = bottom2 - (db - db_min) / (db_max - db_min) * (bottom2 - top2)
        draw.line((left, y, right, y), fill=GRID, width=1)
        text(draw, (left - 18, y), str(db), 19, MUTED, anchor="rm")
        svg.append(f'<line x1="{left}" y1="{y:.1f}" x2="{right}" y2="{y:.1f}" stroke="{GRID}"/>')
        svg_text(svg, left - 18, y + 7, str(db), 19, MUTED, anchor="end")

    # Evidence card: count only, no fabricated time alignment.
    card_y0, card_y1 = 1120, 1290
    draw.rounded_rectangle((105, card_y0, 2325, card_y1), 30, fill="#10283D")
    svg.append(f'<rect x="105" y="{card_y0}" width="2220" height="{card_y1-card_y0}" rx="30" fill="#10283D"/>')
    text(draw, (155, 1152), "输入", 24, MUTED)
    text(draw, (155, 1190), "10 个鼾声周期", 40, BLUE, bold=True)
    text(draw, (650, 1194), "→", 52, MUTED, bold=True, anchor="mm")
    text(draw, (795, 1152), "设备本轮实际新增识别", 24, MUTED)
    text(draw, (795, 1190), "2 次", 42, ORANGE, bold=True)
    text(draw, (1120, 1148), "串口证据", 24, MUTED)
    text(draw, (1120, 1185), "−30.3 dBFS / ZCR 0.088    ·    −34.6 dBFS / ZCR 0.072", 27, TEXT)
    text(draw, (1120, 1237), "说明：未保存带时间戳的设备PCM，故不虚构2次事件对应的具体脉冲位置。", 22, GREEN)
    svg_text(svg, 155, 1178, "输入", 24, MUTED)
    svg_text(svg, 155, 1230, "10 个鼾声周期", 40, BLUE, weight=700)
    svg_text(svg, 650, 1225, "→", 52, MUTED, weight=700, anchor="middle")
    svg_text(svg, 795, 1178, "设备本轮实际新增识别", 24, MUTED)
    svg_text(svg, 795, 1230, "2 次", 42, ORANGE, weight=700)
    svg_text(svg, 1120, 1178, "串口证据", 24, MUTED)
    svg_text(svg, 1120, 1222, "−30.3 dBFS / ZCR 0.088    ·    −34.6 dBFS / ZCR 0.072", 27)
    svg_text(svg, 1120, 1265, "说明：未保存带时间戳的设备PCM，故不虚构2次事件对应的具体脉冲位置。", 22, GREEN)

    image.save(PNG_PATH, quality=95)
    svg.append("</svg>")
    SVG_PATH.write_text("\n".join(svg), encoding="utf-8")
    print(PNG_PATH)
    print(SVG_PATH)


if __name__ == "__main__":
    generate()
