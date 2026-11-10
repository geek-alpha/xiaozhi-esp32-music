# -*- coding: utf-8 -*-
"""金鱼娘待机背景生成器：出 240x240 RGB565 资源。

    py -3 tools/gen_bg_asset.py

输出：
    main/display/bg_assets.c / bg_assets.h   固件内嵌底图（dark / light 各一张）
    _bg_out/bg_*.png                          预览图，改完先看图再刷机

风格取向（第一性原理）：屏幕中央是一张圆润、明亮、Q 版的蓝发角色，
所以背景只做「她所在的水」——从上往下透的天光、飘过的尾纱、上升的气泡、
水底的焦散光纹。凡是直角、HUD 框、扫描线这类赛博元素一个不留：
它们会把 Q 版脸压成仪表盘，跟金鱼娘这个设定也没关系。

静态全烘进图里，运行时只 blit 一次；动态留给固件里少数小对象
（核心呼吸、水波涟漪、上升气泡、尾纱流光）。
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

# 表情固定贴在 content_ 顶部正中，水波涟漪圆心跟着它走（EMOJI 120x120）。
# 实测（串口日志 Chat view ... in 226x202, emoji at 53,0）：content_ 从 y=24 起，
# 加 2px border + 5px pad 后表情顶在 y=31，所以圆心 31+60 = 91。
EMOJI_CX, EMOJI_CY = W // 2, 91

# 尾纱两条主弧的端点：固件里的 trail 流光沿这两条线走（tech_backdrop.cc 的
# kBladeA / kBladeB），改这里必须同步改那边，否则流光会飘在纱外。
VEIL_A = ((108, 152), (2, 216))
VEIL_B = ((132, 152), (238, 216))

# dark：夜里的小水槽。上青绿透光、下深蓝，尾纱是金鱼橙，气泡冷白。
DARK = dict(
    top=(38, 96, 106),
    mid=(22, 56, 82),
    bottom=(10, 24, 46),
    shaft=(178, 252, 240),
    halo=(255, 186, 176),
    halo2=(255, 144, 124),
    caustic=(176, 252, 246),
    veil_a=(255, 138, 96),
    veil_b=(255, 178, 202),
    veil_core=(255, 244, 234),
    bubble=(204, 250, 255),
    scale=(146, 232, 244),
    petal=(255, 168, 198),
    fish=(255, 136, 92),
    weed=(30, 74, 78),
    status=(14, 30, 44),
    status_line=(255, 176, 164),
    vignette=150,
    grain=3,
)

# light：白天的樱花水。樱白到青白，天光更强，尾纱同色系压低饱和度。
LIGHT = dict(
    top=(255, 250, 252),
    mid=(228, 246, 248),
    bottom=(176, 208, 230),
    shaft=(255, 255, 255),
    halo=(255, 214, 214),
    halo2=(255, 182, 182),
    caustic=(255, 255, 255),
    veil_a=(255, 172, 150),
    veil_b=(255, 202, 218),
    veil_core=(255, 255, 252),
    bubble=(255, 255, 255),
    scale=(188, 224, 240),
    petal=(255, 180, 206),
    fish=(246, 148, 116),
    weed=(150, 192, 188),
    status=(240, 246, 252),
    status_line=(255, 182, 172),
    vignette=92,
    grain=2,
)


def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


def rgba(color, a):
    return (color[0], color[1], color[2], int(max(0, min(255, a))))


def add_layer(img, layer):
    return Image.alpha_composite(img.convert("RGBA"), layer).convert("RGB")


def new_layer():
    return Image.new("RGBA", (W, H), (0, 0, 0, 0))


def radial_glow(cx, cy, radius, color, peak_alpha, power=2.2, squash=1.0):
    """径向发光：逐像素算 alpha。同一张 RGBA 层上叠圆是后画覆盖先画，叠不出
    「中心亮、边缘透明」的渐变，只有逐像素写才累积得出来。squash<1 压扁成椭圆光带。"""
    layer = new_layer()
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


def water_gradient(p):
    """三段水色：上（天光透进来的那层）→ 中（水体主色）→ 下（水底）。
    单段两停渐变读起来像塑料板；三段才有「光从上往下被吃掉」的层次。"""
    img = Image.new("RGB", (W, H))
    px = img.load()
    mid_y = int(H * 0.46)
    for y in range(H):
        if y <= mid_y:
            c = lerp(p["top"], p["mid"], (y / max(1, mid_y)) ** 1.15)
        else:
            c = lerp(p["mid"], p["bottom"], ((y - mid_y) / max(1, H - 1 - mid_y)) ** 0.85)
        for x in range(W):
            px[x, y] = c
    return img


def draw_shafts(img, p):
    """天光光柱：从水面往下散开的柔光竖条，越往下越淡。
    条数少、边界糊到几乎看不出线，只留下「上方有光」的方向感。"""
    layer = new_layer()
    d = ImageDraw.Draw(layer)
    for cx, half_w, a in ((40, 20, 52), (104, 30, 74), (170, 24, 58), (216, 14, 42)):
        d.polygon(
            [(cx - half_w * 0.5, STATUS_H), (cx + half_w * 0.5, STATUS_H),
             (cx + half_w * 1.35, H - 20), (cx - half_w * 1.35, H - 20)],
            fill=rgba(p["shaft"], a),
        )
    layer = layer.filter(ImageFilter.GaussianBlur(13))
    return add_layer(img, layer)


def draw_caustics(img, p):
    """焦散光纹：水面把光聚成网状亮线打到水底，是「这是水」最省事的信号。
    两条相位不同的正弦叠出交叉网格，只留画面下 1/3——铺满整屏会糊住角色的脸。"""
    layer = new_layer()
    d = ImageDraw.Draw(layer)
    for k, (amp, freq, phase, y0, a) in enumerate((
        (7.0, 0.052, 0.0, 168, 62),
        (5.0, 0.071, 1.7, 176, 52),
        (9.0, 0.037, 3.1, 158, 40),
    )):
        prev = None
        for x in range(0, W + 4, 4):
            y = y0 + amp * math.sin(freq * x + phase) + 3.0 * math.sin(freq * 2.3 * x + phase * 2)
            if prev is not None:
                d.line([prev, (x, y)], fill=rgba(p["caustic"], a), width=2)
            prev = (x, y)
    layer = layer.filter(ImageFilter.GaussianBlur(2.6))
    return add_layer(img, layer)


def _bezier(p0, p1, ctrl, t):
    u = 1 - t
    x = u * u * p0[0] + 2 * u * t * ctrl[0] + t * t * p1[0]
    y = u * u * p0[1] + 2 * u * t * ctrl[1] + t * t * p1[1]
    return x, y


def draw_veil(img, p, p0, p1, bow, color, color2, width, peak, blur=3.2):
    """一尾纱：沿二次贝塞尔扫一条中段粗、两端收的软带，再用更细的芯提一次亮。
    纱的质感全靠「边上糊、中间透」——所以模糊层给宽度、芯层只给一条细线，
    整条加粗会变成一根塑料管，跟金鱼的尾摆没关系。"""
    mid = ((p0[0] + p1[0]) / 2, (p0[1] + p1[1]) / 2)
    dx, dy = p1[0] - p0[0], p1[1] - p0[1]
    ln = max(1.0, math.hypot(dx, dy))
    ctrl = (mid[0] - dy / ln * bow * ln, mid[1] + dx / ln * bow * ln)

    halo = new_layer()
    dh = ImageDraw.Draw(halo)
    sharp = new_layer()
    ds = ImageDraw.Draw(sharp)
    n = 84
    prev = None
    prev_c = None
    for s in range(n + 1):
        t = s / n
        pt = _bezier(p0, p1, ctrl, t)
        # 包络：两端收窄到 0.25，中段最宽；正弦开方让它收得比直线柔和
        env = math.sin(math.pi * t) ** 0.55 * 0.75 + 0.25
        if prev is not None:
            dh.line([prev, pt], fill=rgba(color, int(peak * env / 2.6)), width=int(width * env))
            ds.line([prev_c, pt], fill=rgba(color2, int(peak * env / 2.2)), width=max(1, int(width * env * 0.26)))
        prev = pt
        prev_c = pt
    out = add_layer(img, halo.filter(ImageFilter.GaussianBlur(blur)))
    return add_layer(out, sharp.filter(ImageFilter.GaussianBlur(0.7)))


def draw_tails(img, p):
    """四道尾纱：下方两道往下垂（裙摆），上方两道往斜上飘（尾摆上扬）。
    下方两道刻意不从同一点起手、中间留 24px 缝——交汇处会顶出一个尖峰，
    在 240px 屏上读起来就是一座山，不是纱。上方两道更淡更细，只给上扬的动势。"""
    for p0, p1, bow, w, peak, blur in (
        (VEIL_A[0], VEIL_A[1], -0.18, 26, 110, 7.5),
        (VEIL_B[0], VEIL_B[1], 0.18, 26, 104, 7.5),
        ((100, 138), (8, 92), 0.26, 13, 74, 5.0),
        ((140, 138), (232, 92), -0.26, 13, 70, 5.0),
    ):
        img = draw_veil(img, p, p0, p1, bow, p["veil_a"], p["veil_b"], w, peak, blur=blur)
        img = draw_veil(img, p, p0, p1, bow, p["veil_core"], p["veil_core"],
                        max(3, w // 3), peak + 40, blur=1.1)
    return img


def draw_bubbles(img, p):
    """气泡：大小不一的空心圆 + 一点左上高光。实心点会读成雪或星星，
    空心圈才读得出「水里的泡」。避开角色脸部那圈，别把脸戳出洞。"""
    layer = new_layer()
    d = ImageDraw.Draw(layer)
    rnd = random.Random(1907)
    placed = 0
    while placed < 22:
        cx = rnd.randrange(6, W - 6)
        cy = rnd.randint(STATUS_H + 6, H - 6)
        r = rnd.choice([2.0, 2.5, 3.0, 4.0, 5.0, 6.5])
        if math.hypot(cx - EMOJI_CX, cy - EMOJI_CY) < 66 + r:
            continue
        placed += 1
        a = rnd.randint(36, 82)
        d.ellipse([cx - r, cy - r, cx + r, cy + r], outline=rgba(p["bubble"], a), width=1)
        d.point((cx - r * 0.42, cy - r * 0.42), fill=rgba(p["bubble"], min(255, a + 90)))
    return add_layer(img, layer.filter(ImageFilter.GaussianBlur(0.3)))


def draw_scales(img, p):
    """鳞片纹：一片片小圆弧切出的扇形网格，只铺在角色外圈。
    金鱼娘这个设定下鳞片比任何赛博元素都对味，但铺到脸上就变成鱼鳞脸了，
    所以圆心 74px 以内整片跳过。alpha 压到个位数，远看只是一层「有肌理的水」。"""
    layer = new_layer()
    d = ImageDraw.Draw(layer)
    step = 15
    rad = 8
    for row, y in enumerate(range(STATUS_H + 4, H, rad)):
        for col, x in enumerate(range(-step, W + step, step)):
            cx = x + (step // 2 if row % 2 else 0)
            if math.hypot(cx - EMOJI_CX, y - EMOJI_CY) < 70:
                continue
            if x < -step or x > W + step:
                continue
            d.arc([cx - step // 2, y - rad, cx + step // 2, y + rad], 200, 340,
                  fill=rgba(p["scale"], 34), width=1)
    return add_layer(img, layer.filter(ImageFilter.GaussianBlur(0.5)))


def draw_petals(img, p):
    """樱花瓣：五片式花形，用水滴多边形而不是椭圆。
    椭圆加旋转在 240px 上只会读成一堆粉方块；花形靠「尖端 + 圆弧」才能被认出来。
    整层画完统一旋转，省得逐片算旋转矩阵；只放边缘，中央留给脸。"""
    layer = new_layer()
    d = ImageDraw.Draw(layer)
    rnd = random.Random(8)
    pts = [(22, 52), (206, 74), (36, 196), (198, 178), (150, 34), (78, 214), (222, 138), (12, 128)]
    for cx, cy in pts:
        s = rnd.uniform(0.8, 1.25)
        a = rnd.randint(110, 170)
        tip_ang = rnd.uniform(0, math.tau)
        shape = []
        for k in range(14):
            ang = math.tau * k / 14
            # 一个方向拉长成水滴，另一侧收窄：r = s * (0.55 + 0.45*cos) 就是花瓣的经典轮廓
            r = s * (5.2 + 3.4 * math.cos(ang - tip_ang))
            shape.append((cx + r * math.cos(ang), cy + r * 0.72 * math.sin(ang)))
        d.polygon(shape, fill=rgba(p["petal"], a))
        d.line([(cx, cy), (cx + 5.4 * s * math.cos(tip_ang), cy + 3.9 * s * math.sin(tip_ang))],
               fill=rgba((255, 255, 255), a // 3), width=1)
    layer = layer.filter(ImageFilter.GaussianBlur(0.35)).rotate(-14, resample=Image.BICUBIC)
    return add_layer(img, layer)


def draw_fish(img, p):
    """远处两尾小金鱼：身体一个扁椭圆、尾巴两条扫开的弧，尺寸压到十几像素。
    它们在画面里只担「这是金鱼的地盘」，不是主体——所以半透明、避开中央。"""
    layer = new_layer()
    d = ImageDraw.Draw(layer)
    for cx, cy, s, a, flip in (
        (46, 122, 1.35, 130, 0),
        (196, 104, 1.0, 100, 1),
    ):
        dirx = -1 if flip else 1
        d.ellipse([cx - 7 * s, cy - 4 * s, cx + 7 * s, cy + 4 * s], fill=rgba(p["fish"], a))
        d.polygon([
            (cx - 6 * s * dirx, cy),
            (cx - 15 * s * dirx, cy - 6 * s),
            (cx - 12 * s * dirx, cy),
            (cx - 15 * s * dirx, cy + 6 * s),
        ], fill=rgba(p["fish"], a * 0.75))
        # 眼睛和背鳍：两笔就够，多了在 7px 的鱼身上全是噪点
        d.point((cx + 3 * s * dirx, cy - 1 * s), fill=rgba((40, 20, 26), min(255, a + 80)))
        d.line([(cx - 2 * s * dirx, cy - 4 * s), (cx + 2 * s * dirx, cy - 6 * s)],
               fill=rgba(p["veil_core"], a * 0.8), width=1)
    return add_layer(img, layer.filter(ImageFilter.GaussianBlur(0.4)))


def draw_halo(img, p):
    """角色背后的暖光：把脸托在光里，跟她金鱼橙的尾纱同色系。
    刻意不画硬边框——硬圆环会把 Q 版脸压成仪表盘。"""
    img = add_layer(img, radial_glow(EMOJI_CX, EMOJI_CY + 4, 92, p["halo"], 74, 1.9))
    # 两圈不完整的涟漪弧：比整圈硬环柔和，也不会把脸框成仪表盘
    layer = new_layer()
    d = ImageDraw.Draw(layer)
    for r, a, w in ((76, 46, 1), (90, 30, 1)):
        d.arc([EMOJI_CX - r, EMOJI_CY - r + 4, EMOJI_CX + r, EMOJI_CY + r + 4], 186, 354,
              fill=rgba(p["halo2"], a), width=w)
    img = add_layer(img, layer.filter(ImageFilter.GaussianBlur(1.6)))
    return img


def draw_weeds(img, p):
    """水底水草：几根摇摆的细曲线，从底边往上长。
    有了它画面才有「底」，不然全屏均匀水色在 240px 上读起来像没画完。"""
    layer = new_layer()
    d = ImageDraw.Draw(layer)
    rnd = random.Random(613)
    for base_x in (10, 26, 44, 196, 214, 230):
        h = rnd.randint(26, 54)
        sway = rnd.choice([-1, 1]) * rnd.uniform(4, 11)
        prev = None
        for s in range(h):
            t = s / h
            x = base_x + sway * math.sin(t * 2.4) * t
            y = H - 2 - s
            if prev is not None:
                d.line([prev, (x, y)], fill=rgba(p["weed"], int(120 * (1 - t * 0.55))), width=1)
            prev = (x, y)
    return add_layer(img, layer.filter(ImageFilter.GaussianBlur(0.7)))


def draw_statusbar(img, p):
    layer = new_layer()
    d = ImageDraw.Draw(layer)
    d.rectangle([0, 0, W, STATUS_H], fill=rgba(p["status"], 236))
    d.line([(0, STATUS_H), (W, STATUS_H)], fill=rgba(p["status_line"], 120), width=1)
    d.line([(0, STATUS_H + 1), (W, STATUS_H + 1)], fill=rgba(p["status_line"], 40), width=1)
    return add_layer(img, layer)


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


def compose(p):
    img = water_gradient(p)
    img = draw_shafts(img, p)
    img = draw_caustics(img, p)
    img = draw_scales(img, p)
    img = draw_fish(img, p)
    img = draw_halo(img, p)
    img = draw_tails(img, p)
    img = draw_bubbles(img, p)
    img = draw_petals(img, p)
    img = draw_weeds(img, p)
    img = add_vignette(img, p["vignette"])
    img = add_grain(img, p["grain"])
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
            "// 金鱼娘待机背景底图：dark（夜水槽）/ light（樱花水）各一张，240x240 RGB565\n"
            "extern const lv_image_dsc_t bg_dark;\nextern const lv_image_dsc_t bg_light;\n\n"
            "#endif\n"
        )
    print("wrote", C_PATH)
    print("wrote", H_PATH)


if __name__ == "__main__":
    main()
