# -*- coding: utf-8 -*-
"""赛博朋克霓虹待机背景生成器：出 240x240 RGB565 资源。

    py -3 tools/gen_bg_asset.py

输出：
    main/display/bg_assets.c / bg_assets.h   固件内嵌底图（dark / light 各一张）
    _bg_out/bg_*.png                          预览图，改完先看图再刷机

风格取向（第一性原理）：屏幕中央那张脸是圆润、明亮、Q 版的蓝发角色，
背景要当「夜里的霓虹街景」——暗底、软光晕、剪影，凡是直角框、刻度尺、
军用 HUD 这类硬元素都会跟它抢戏，所以一个都不留。
硬几何全部换成光：霓虹环、斜向光带、湿地面反光、雨丝。

静态全烘进图里，运行时只 blit 一次；动态留给固件里少数小对象
（光环呼吸、冲击环、雨滴、光带流光、扫描带）。
"""
import math
import os
import random

from PIL import Image, ImageDraw, ImageFilter

W = H = 240
STATUS_H = 24
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(ROOT, "_bg_out")
C_PATH = os.path.join(ROOT, "main", "display", "bg_assets.c")
H_PATH = os.path.join(ROOT, "main", "display", "bg_assets.h")

# 表情固定贴在 content_ 顶部正中，光环圆心跟着它走（EMOJI 120x120）。
# 实测（串口日志 Chat view ... in 226x202, emoji at 53,0）：content_ 从 y=24 起，
# 加 2px border + 5px pad 后表情顶在 y=31，所以圆心 31+60 = 91。
EMOJI_CX, EMOJI_CY = W // 2, 91
HORIZON_Y = 198

DARK = dict(
    top=(12, 6, 26),
    bottom=(34, 12, 52),
    halo=(0, 226, 255),
    halo2=(255, 45, 149),
    ring_in=(150, 250, 255),
    ring_mid=(0, 214, 255),
    ring_out=(255, 62, 158),
    streak=(255, 58, 158),
    streak2=(72, 240, 255),
    city=(24, 12, 42),
    city_edge=(168, 52, 208),
    win_c=(0, 226, 255),
    win_p=(255, 96, 176),
    horizon=(255, 62, 158),
    reflect=(0, 200, 236),
    rain=(150, 226, 255),
    rain_alpha=46,
    scan_alpha=10,
    status=(10, 5, 22),
    status_line=(255, 62, 158),
    vignette=205,
    grain=4,
)

LIGHT = dict(
    top=(238, 226, 250),
    bottom=(186, 156, 216),
    halo=(58, 150, 196),
    halo2=(224, 72, 158),
    ring_in=(255, 255, 255),
    ring_mid=(200, 96, 176),
    ring_out=(96, 130, 210),
    streak=(224, 72, 158),
    streak2=(66, 158, 200),
    city=(150, 126, 186),
    city_edge=(96, 44, 140),
    win_c=(16, 120, 172),
    win_p=(206, 44, 132),
    horizon=(224, 72, 158),
    reflect=(72, 150, 196),
    rain=(120, 150, 190),
    rain_alpha=34,
    scan_alpha=7,
    status=(226, 214, 240),
    status_line=(214, 64, 148),
    vignette=140,
    grain=3,
)


def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


def rgba(color, a):
    return (color[0], color[1], color[2], int(a))


def add_layer(img, layer):
    return Image.alpha_composite(img.convert("RGBA"), layer).convert("RGB")


def radial_glow(cx, cy, radius, color, peak_alpha, power=2.2, squash=1.0):
    """径向发光：逐像素算 alpha。同一张 RGBA 层上叠圆是后画覆盖先画，叠不出
    「中心亮、边缘透明」的渐变，只有逐像素写才累积得出来。squash<1 压扁成椭圆光带。"""
    layer = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    px = layer.load()
    ry = max(1, int(radius * squash))
    for y in range(max(0, cy - ry), min(H, cy + ry)):
        for x in range(max(0, cx - radius), min(W, cx + radius)):
            d = math.hypot((x - cx) / radius, (y - cy) / ry)
            if d >= 1:
                continue
            a = int(peak_alpha * (1 - d) ** power)
            if a > 0:
                px[x, y] = (color[0], color[1], color[2], a)
    return layer


def vertical_gradient(top, bottom):
    """夜空竖向渐变：上暗下亮一档，给地面反光留出底部亮度。"""
    img = Image.new("RGB", (W, H))
    px = img.load()
    for y in range(H):
        c = lerp(top, bottom, (y / (H - 1)) ** 1.6)
        for x in range(W):
            px[x, y] = c
    return img


def add_vignette(img, strength):
    """四边压暗收拢视线。软圆孔在 240px 上会露亮斑，所以走方形幂曲线。"""
    out = img.copy()
    px = out.load()
    k_strength = strength / 255
    cx, cy = W / 2, H / 2
    for y in range(H):
        for x in range(W):
            k = max(abs(x - cx) / cx, abs(y - cy) / cy) ** 2.4 * k_strength
            r, g, b = px[x, y]
            px[x, y] = (int(r * (1 - k)), int(g * (1 - k)), int(b * (1 - k)))
    return out


def add_grain(img, amount, seed=7):
    """一点噪点，避免大面积纯色在 565 面板上出现色带。"""
    rnd = random.Random(seed)
    px = img.load()
    for y in range(H):
        for x in range(W):
            n = rnd.randint(-amount, amount)
            r, g, b = px[x, y]
            px[x, y] = (max(0, min(255, r + n)), max(0, min(255, g + n)), max(0, min(255, b + n)))
    return img


def neon_ambient(img, p):
    """两侧逆光：左下一团品红霓虹、右上一团青霓虹，像站在招牌林立的巷口。"""
    img = add_layer(img, radial_glow(16, 178, 154, p["halo2"], 112, 2.0))
    img = add_layer(img, radial_glow(226, 54, 142, p["halo"], 68, 2.4))
    img = add_layer(img, radial_glow(EMOJI_CX, EMOJI_CY + 6, 100, p["ring_mid"], 48, 2.4))
    return img


def draw_skyline(img, p):
    """两侧高楼剪影 + 稀疏霓虹窗：中间留空给角色，只做场景深度，不抢视觉。"""
    layer = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    rnd = random.Random(2024)
    # (左边界, 顶边, 宽度) —— 越靠外越高，把视线往中间压
    blocks = [
        (-6, 118, 42), (28, 142, 34), (52, 160, 30),
        (188, 150, 32), (214, 128, 40), (232, 108, 26),
    ]
    for x0, y0, w in blocks:
        d.rectangle([x0, y0, x0 + w, HORIZON_Y], fill=rgba(p["city"], 236))
        d.line([(x0, y0), (x0 + w, y0)], fill=rgba(p["city_edge"], 235), width=1)
        d.line([(x0, y0), (x0, y0 + 8)], fill=rgba(p["city_edge"], 130), width=1)
        for _ in range(rnd.randint(10, 18)):
            wx = rnd.randint(x0 + 3, x0 + w - 5)
            wy = rnd.randint(y0 + 5, HORIZON_Y - 6)
            col = p["win_c"] if rnd.random() < 0.62 else p["win_p"]
            a = rnd.choice([120, 170, 215])
            d.point((wx, wy), fill=rgba(col, a))
            if rnd.random() < 0.4:
                d.point((wx + 1, wy), fill=rgba(col, a // 2))
    return add_layer(img, layer)


def draw_horizon(img, p):
    """霓虹地平线 + 湿地面竖条反光：整张图的「地面」靠这条光带立住。"""
    glow = radial_glow(W // 2, HORIZON_Y, 190, p["horizon"], 58, 2.0, squash=0.16)
    img = add_layer(img, glow)
    layer = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    d.line([(0, HORIZON_Y), (W, HORIZON_Y)], fill=rgba(p["horizon"], 210), width=1)
    d.line([(0, HORIZON_Y + 1), (W, HORIZON_Y + 1)], fill=rgba(p["horizon"], 90), width=1)
    rnd = random.Random(99)
    for _ in range(26):
        x = rnd.randrange(W)
        h = rnd.randint(4, 22)
        a = rnd.randint(16, 46)
        col = p["reflect"] if rnd.random() < 0.55 else p["streak"]
        d.line([(x, HORIZON_Y + 2), (x, HORIZON_Y + 2 + h)], fill=rgba(col, a), width=1)
    return add_layer(img, layer)


def draw_halo(img, p):
    """角色背后的霓虹光环：三圈同心软光，中心最亮往外散，把脸托在光里。
    刻意不画硬边框——硬圆环会把 Q 版脸压成仪表盘。"""
    img = add_layer(img, radial_glow(EMOJI_CX, EMOJI_CY, 92, p["ring_mid"], 62, 1.7))
    layer = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    for r, col, a, wd in (
        (66, p["ring_mid"], 165, 2),
        (82, p["ring_out"], 96, 1),
    ):
        d.ellipse([EMOJI_CX - r, EMOJI_CY - r, EMOJI_CX + r, EMOJI_CY + r],
                  outline=rgba(col, a), width=wd)
    # 环上开四道缺口，像没接完的霓虹灯管，比整圈更活
    for ang in (18, 104, 197, 286):
        a0 = math.radians(ang)
        for rr in (66, 82):
            cx = EMOJI_CX + rr * math.cos(a0)
            cy = EMOJI_CY + rr * math.sin(a0)
            d.ellipse([cx - 3.2, cy - 3.2, cx + 3.2, cy + 3.2], fill=(0, 0, 0, 0))
    layer = layer.filter(ImageFilter.GaussianBlur(0.8))
    return add_layer(img, layer)


def draw_stars(img, p):
    """夜空星点：上半个屏幕撒一点冷星，给天空一层「远处有东西」的深度。"""
    layer = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    rnd = random.Random(31)
    for _ in range(46):
        x = rnd.randrange(W)
        y = rnd.randint(STATUS_H + 2, 150)
        a = rnd.choice([40, 70, 110, 150])
        d.point((x, y), fill=rgba(p["ring_in"], a))
        if rnd.random() < 0.16:
            d.line([(x - 1, y), (x + 1, y)], fill=rgba(p["ring_in"], max(0, a - 50)), width=1)
    return add_layer(img, layer)


def draw_bokeh(img, p):
    """散景光斑：大小不一的柔圆光点，像镜头外失焦的霓虹。
    代替之前那排横向亮条——规整的短横条读起来像仪表刻度，
    跟萌系脸放一起就是「乱入」；失焦光斑没有方向感，只给氛围。"""
    layer = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    rnd = random.Random(77)
    for _ in range(18):
        cx = rnd.choice([rnd.randint(0, 52), rnd.randint(188, W)])
        cy = rnd.randint(STATUS_H + 4, 186)
        r = rnd.randint(3, 9)
        col = p["streak"] if rnd.random() < 0.5 else p["streak2"]
        a = rnd.randint(26, 62)
        d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=rgba(col, a))
        d.ellipse([cx - r * 0.45, cy - r * 0.45, cx + r * 0.45, cy + r * 0.45], fill=rgba(col, a * 2))
    # 贴近角色背后的两团大光斑：给脸添一层环境光，不是硬框
    d.ellipse([EMOJI_CX - 104, EMOJI_CY - 52, EMOJI_CX - 40, EMOJI_CY + 12], fill=rgba(p["streak"], 22))
    d.ellipse([EMOJI_CX + 34, EMOJI_CY - 40, EMOJI_CX + 106, EMOJI_CY + 34], fill=rgba(p["streak2"], 20))
    return add_layer(img, layer.filter(ImageFilter.GaussianBlur(2.4)))


def _tube(dst, x0, y0, x1, y1, color, core, peak, blur_r, half_w):
    """单根霓虹灯管：模糊层给环境光晕，清晰层给灯管本身。
    分段必须首尾相接（每段从上一段的落点画起），否则逐点竖线会变成一排骨排。"""
    halo = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    dh = ImageDraw.Draw(halo)
    sharp = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    ds = ImageDraw.Draw(sharp)
    n = 150
    px_, py_ = x0, y0
    for s in range(1, n + 1):
        t = s / n
        x = x0 + (x1 - x0) * t
        y = y0 + (y1 - y0) * t
        fade = math.sin(math.pi * (t - 0.5 / n)) ** 0.5
        a = int(peak * fade)
        if a > 0:
            dh.line([(px_, py_), (x, y)], fill=rgba(color, a // 3), width=half_w * 9)
            ds.line([(px_, py_), (x, y)], fill=rgba(color, a), width=half_w * 2)
        px_, py_ = x, y
    # 白芯只占中段，一条连贯的 line；端点不画芯，收尾才干净
    t0, t1 = 0.18, 0.82
    ds.line([(x0 + (x1 - x0) * t0, y0 + (y1 - y0) * t0),
             (x0 + (x1 - x0) * t1, y0 + (y1 - y0) * t1)],
            fill=rgba(core, 170), width=1)
    out = add_layer(dst, halo.filter(ImageFilter.GaussianBlur(blur_r)))
    return add_layer(out, sharp)


def draw_streaks(img, p):
    """角色下方的交叉灯管：一品格红主光、一条青弱光。
    刻意压到 y>=150——光带从脸上横穿会把 Q 版脸切成两半，
    放在下巴以下就变成「擂台的斜切灯光」，动势还在，脸不被切。"""
    img = _tube(img, 6, 158, 234, 196, p["streak"], (255, 240, 252), 205, 2.6, 1)
    img = _tube(img, 234, 150, 10, 190, p["streak2"], (222, 252, 255), 165, 2.0, 1)
    return img


def draw_rain(img, p):
    """斜雨丝：赛博朋克夜景的标配，也让画面有「正在下雨」的运动暗示。"""
    layer = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    rnd = random.Random(512)
    for _ in range(58):
        x = rnd.randrange(W + 30)
        y = rnd.randint(STATUS_H, H)
        ln = rnd.randint(5, 13)
        a = rnd.randint(18, p["rain_alpha"] + 20)
        d.line([(x, y), (x - ln * 0.32, y + ln)], fill=rgba(p["rain"], a), width=1)
    return add_layer(img, layer.filter(ImageFilter.GaussianBlur(0.4)))


def draw_scanlines(img, p):
    """CRT 扫描线：每 3 行压暗一档，屏幕质感全靠它。"""
    layer = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    for y in range(STATUS_H, H, 3):
        d.line([(0, y), (W, y)], fill=(0, 0, 0, p["scan_alpha"]), width=1)
    return add_layer(img, layer)


def draw_glitch(img, p):
    """只留一点横向色散：青/品红错开 1px 的短线，信号不稳的感觉。
    之前那几条横向亮块在 240px 屏上读起来像没清干净的 UI 残留，已删。"""
    layer = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    for dy, x0, x1, col, a in (
        (0, 118, 152, p["streak"], 58),
        (1, 116, 150, p["streak2"], 58),
        (0, 34, 62, p["streak2"], 40),
        (1, 32, 60, p["streak"], 40),
    ):
        d.line([(x0, 205 + dy), (x1, 205 + dy)], fill=rgba(col, a), width=1)
    return add_layer(img, layer)


def draw_statusbar(img, p):
    layer = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    d.rectangle([0, 0, W, STATUS_H], fill=rgba(p["status"], 255))
    d.line([(0, STATUS_H), (W, STATUS_H)], fill=rgba(p["status_line"], 130), width=1)
    d.line([(0, STATUS_H + 1), (W, STATUS_H + 1)], fill=rgba(p["status_line"], 44), width=1)
    return add_layer(img, layer)


def compose(p):
    img = vertical_gradient(p["top"], p["bottom"])
    img = draw_stars(img, p)
    img = neon_ambient(img, p)
    img = draw_bokeh(img, p)
    img = draw_skyline(img, p)
    img = draw_horizon(img, p)
    img = draw_halo(img, p)
    img = draw_streaks(img, p)
    img = draw_rain(img, p)
    img = draw_glitch(img, p)
    img = add_vignette(img, p["vignette"])
    img = add_grain(img, p["grain"])
    img = draw_scanlines(img, p)
    img = draw_statusbar(img, p)
    return img


def to_rgb565(img):
    out = bytearray()
    px = img.convert("RGB").load()
    for y in range(H):
        for x in range(W):
            r, g, b = px[x, y]
            v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
            out.append(v & 0xFF)
            out.append((v >> 8) & 0xFF)
    return bytes(out)


def emit_c(name, data):
    lines = ["// 由 tools/gen_bg_asset.py 生成，勿手改", f"static const uint8_t {name}_map[] = {{"]
    for i in range(0, len(data), 20):
        lines.append("    " + ",".join(f"0x{b:02X}" for b in data[i:i + 20]) + ",")
    lines.append("};")
    lines.append("")
    lines.append(f"const lv_image_dsc_t {name} = {{")
    lines.append("    .header.magic = LV_IMAGE_HEADER_MAGIC,")
    lines.append("    .header.cf = LV_COLOR_FORMAT_RGB565,")
    lines.append("    .header.flags = 0,")
    lines.append(f"    .header.w = {W},")
    lines.append(f"    .header.h = {H},")
    lines.append(f"    .header.stride = {W * 2},")
    lines.append(f"    .data_size = {len(data)},")
    lines.append(f"    .data = {name}_map,")
    lines.append("};")
    lines.append("")
    return "\n".join(lines)


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    chunks = ['// 由 tools/gen_bg_asset.py 生成，勿手改', '#include "bg_assets.h"', ""]
    for name, p in (("bg_dark", DARK), ("bg_light", LIGHT)):
        img = compose(p)
        img.resize((480, 480), Image.NEAREST).save(os.path.join(OUT_DIR, f"{name}.png"))
        data = to_rgb565(img)
        print(name, len(data), "bytes")
        chunks.append(emit_c(name, data))
    with open(C_PATH, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(chunks))
    with open(H_PATH, "w", encoding="utf-8", newline="\n") as f:
        f.write(
            "#ifndef BG_ASSETS_H\n#define BG_ASSETS_H\n\n#include <lvgl.h>\n\n"
            "// 待机背景底图：暗色 / 亮色各一张，240x240 RGB565\n"
            "extern const lv_image_dsc_t bg_dark;\nextern const lv_image_dsc_t bg_light;\n\n"
            "#endif\n"
        )
    print("wrote", C_PATH)
    print("wrote", H_PATH)


if __name__ == "__main__":
    main()
