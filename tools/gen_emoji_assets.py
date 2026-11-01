# -*- coding: utf-8 -*-
"""二次元 Q 版表情生成器：4x 超采样绘制，LANCZOS 降采样得平滑描边。

生成 main/display/emoji_assets.c / .h（固件内嵌表情、口型贴片、眨眼贴片、发音遮罩）。
用法（项目根目录）：python tools/gen_emoji_assets.py
需要 Pillow。输出 120x120 RGBA，转 RGB565A8 嵌入固件。
"""
import os
import math
from PIL import Image, ImageChops, ImageDraw, ImageFilter

S = 4
SZ = 240
W = SZ * S

SKIN = (255, 228, 214, 255)
SKIN_SH = (250, 205, 188, 255)
LINE = (72, 42, 40, 255)
HAIR = (118, 166, 255, 255)
HAIR_DK = (82, 126, 212, 255)
HAIR_LT = (160, 198, 255, 255)
WHITE = (255, 255, 255, 255)
IRIS_TOP = (58, 38, 92, 255)
IRIS_BOT = (146, 208, 255, 255)
MOUTH = (198, 80, 94, 255)
TONGUE = (255, 142, 152, 255)
BLUSH = (255, 156, 174, 255)
# 轮廓线专用色：中性深紫黑。不能跟 LINE 共用暖棕，否则在蓝发上发脏；
# 也不能偏蓝，否则用户把发色改成红/绿后描边和头发打起来。
# 必须同时满足两个条件：
#   1) 不落入 hair_masks 的任一条判据（b-r<40 且非亮白），否则描边会被拉进 A8 遮罩，
#      随 image_recolor 跟着发色一起染，改发色时轮廓就消失了
#   2) 降采样后与发色混出的中间像素要尽量深，才不会被判成头发（见 OUTLINE_W 注释）
OUTLINE = (46, 38, 52, 255)
OUTLINE_W = 3.5  # 逻辑单位；最终 120px 画布上约 1.75px
# 最外圈边缘光。屏幕背景是近黑的科技底（_bg_out/bg_0.png 实测），
# 深色轮廓在黑底上等于隐形——这就是“没线条感”的真正原因。外圈必须用亮色才有对比。
RIM = (198, 224, 255, 255)
RIM_GLOW = (110, 164, 255, 255)


def canvas():
    return Image.new("RGBA", (W, W), (0, 0, 0, 0))


def el(d, box, fill=None, outline=None, width=0):
    d.ellipse([int(v) for v in box], fill=fill, outline=outline, width=int(width))


def poly(d, pts, fill, outline=None, width=0):
    p = [(int(x), int(y)) for x, y in pts]
    if outline is not None and width > 0:
        d.polygon(p, fill=fill, outline=outline, width=int(width))
    else:
        d.polygon(p, fill=fill)


def hair_back(d, fill=None, outline=True):
    el(d, (30 * S, 26 * S, 210 * S, 214 * S), fill=HAIR_DK if fill is None else fill,
       outline=OUTLINE if outline else None, width=int(OUTLINE_W * S))


def hair_front(d, fill=None, dark=None, highlight=True, outline=True):
    F = HAIR if fill is None else fill
    K = HAIR_DK if dark is None else dark
    ol = OUTLINE if outline else None
    ow = int(OUTLINE_W * S)
    pts = []
    # 发际线：中间厚两侧薄（M 形），左 -> 右
    for i in range(41):
        x = 40 + 160 * i / 40.0
        y = 94 - 9 * math.cos((x - 120) * math.pi / 80.0)
        pts.append((x * S, y * S))
    # 右太阳穴 -> 右上
    pts.append((204 * S, 44 * S))
    # 顶部椭圆弧：右 -> 左
    for i in range(41):
        t = math.pi * i / 40.0
        pts.append(((120 + 84 * math.cos(t)) * S, (98 - 74 * math.sin(t)) * S))
    pts.append((36 * S, 44 * S))
    poly(d, pts, F, ol, ow)
    # 刘海分绉尖：三个尖彼此之间靠描边分开，否则同色叠同色就是一块蓝，看不出分绉
    for tx in (78, 120, 162):
        tip_y = 104 - 10 * math.cos((tx - 120) * math.pi / 80.0)
        poly(d, [((tx - 16) * S, (tip_y - 20) * S), ((tx + 16) * S, (tip_y - 20) * S), (tx * S, tip_y * S)],
             F, ol, ow)
    # 发丝高光
    if highlight:
        d.line([(78 * S, 62 * S), (150 * S, 52 * S)], fill=(255, 255, 255, 110), width=int(3 * S))
    # 鬃角
    poly(d, [(38 * S, 96 * S), (58 * S, 102 * S), (52 * S, 168 * S), (34 * S, 150 * S)], K, ol, ow)
    poly(d, [(202 * S, 96 * S), (182 * S, 102 * S), (188 * S, 168 * S), (206 * S, 150 * S)], K, ol, ow)
    # 呆毛
    poly(d, [(118 * S, 36 * S), (140 * S, 10 * S), (152 * S, 20 * S), (134 * S, 42 * S)], F, ol, ow)


def hair_masks(items):
    """每张脸各一张遮罩：只在「几何是头发」且「这张脸这里确实是蓝系」的像素上生效。
    不能 21 张共用一张：闭眼弧、放大眼会盖住刘海，取交集下来刘海就缺一块（实测缺 898 像素）。
    光靠重放几何也不行：hair_back 是一整个大椭圆，全靠后画的 face 盖住，不减脸就是整颗头全染。"""
    back = Image.new("L", (W, W), 0)
    hair_back(ImageDraw.Draw(back), fill=255, outline=False)
    face_img = Image.new("RGBA", (W, W), (0, 0, 0, 0))
    face(ImageDraw.Draw(face_img), outline=False)
    back = ImageChops.subtract(back, face_img.split()[3])

    front = Image.new("L", (W, W), 0)
    # 几何重放必须关掉描边：outline 是 4 元组 RGBA，画到 L 图上是 46 的灰值，
    # 会形成一圈 gv>=8 的假几何，把刚加的那圈轮廓线整个拉进遮罩，改发色时轮廓跟着被染
    hair_front(ImageDraw.Draw(front), fill=255, dark=255, highlight=False, outline=False)

    geo = ImageChops.lighter(back, front).resize((SZ, SZ), Image.LANCZOS).resize((OUT, OUT), Image.LANCZOS)
    gp = geo.load()

    masks = {}
    for name, im in items.items():
        px = im.load()
        m = Image.new("L", (OUT, OUT), 0)
        mp = m.load()
        for y in range(OUT):
            for x in range(OUT):
                gv = gp[x, y]
                if gv < 8:
                    continue
                r, g, b, a = px[x, y]
                if b - r >= 40 and b >= 140:
                    mp[x, y] = gv
                elif min(r, g, b) >= 140 and max(r, g, b) - min(r, g, b) <= 60:
                    # 发丝高光那笔白叠在蓝底上变成亮白，旧的「蓝系」判据把它当非头发，遮罩留成洞。
                    # 这里必须按满值吃干净：给半透明就有等比例的亮白透上来，换完发色就是额头那道白疤。
                    # 眉毛暗色、腮红高饱和都进不了这条分支；脸部在 geo 里本来就是 0
                    mp[x, y] = gv
        masks[name] = m
    return masks





def face(d, outline=True):
    el(d, (52 * S, 58 * S, 188 * S, 206 * S), fill=SKIN,
       outline=OUTLINE if outline else None, width=int(OUTLINE_W * S))


def brows(d, style="normal"):
    for cx in (88, 152):
        inner = cx > 120
        if style == "angry":
            y1 = 80 if inner else 86
            y2 = 86 if inner else 80
            d.line([((cx - 17) * S, y1 * S), ((cx + 17) * S, y2 * S)], fill=LINE, width=int(3 * S))
        elif style == "sad":
            y1 = 86 if inner else 80
            y2 = 80 if inner else 86
            d.line([((cx - 17) * S, y1 * S), ((cx + 17) * S, y2 * S)], fill=LINE, width=int(3 * S))
        elif style == "raised":
            d.arc([(cx - 17) * S, 58 * S, (cx + 17) * S, 80 * S], 195, 345, fill=LINE, width=int(3 * S))
        else:
            d.arc([(cx - 17) * S, 64 * S, (cx + 17) * S, 84 * S], 200, 340, fill=LINE, width=int(3 * S))


BLINK_MODE = False  # True 时所有睁眼改画闭眼，用来出眨眼贴片
BLINK_STYLE = "flat"


def eye(d, cx, cy, rx=31, ry=35, lid=1.0, look=(0, 0), heart=False):
    if BLINK_MODE:
        closed_eye(d, cx, cy, BLINK_STYLE)
        return
    cy2 = cy + ry * (1 - lid)
    ry2 = max(ry * lid, 1.0)
    el(d, ((cx - rx) * S, (cy2 - ry2) * S, (cx + rx) * S, (cy2 + ry2) * S), fill=WHITE)
    if lid > 0.4:
        ixr = rx * 0.80
        iyr = ry2 * 0.84
        icx, icy = cx + look[0], cy2 + look[1]
        steps = 26
        for i in range(steps):
            t = i / (steps - 1.0)
            col = tuple(int(IRIS_TOP[k] + (IRIS_BOT[k] - IRIS_TOP[k]) * t) for k in range(3)) + (255,)
            k = 1 - t
            el(d, ((icx - ixr * k) * S, (icy - iyr * k) * S,
                   (icx + ixr * k) * S, (icy + iyr * k) * S), fill=col)
        el(d, ((icx - ixr * 0.44) * S, (icy - iyr * 0.36) * S,
               (icx + ixr * 0.44) * S, (icy + iyr * 0.48) * S), fill=(26, 16, 42, 255))
        if heart:
            el(d, ((icx - 12) * S, (icy - 15) * S, (icx - 1) * S, (icy - 3) * S), fill=(255, 118, 148, 255))
            el(d, ((icx + 1) * S, (icy - 15) * S, (icx + 12) * S, (icy - 3) * S), fill=(255, 118, 148, 255))
            poly(d, [((icx - 12) * S, (icy - 7) * S), ((icx + 12) * S, (icy - 7) * S),
                     (icx * S, (icy + 15) * S), (icx * S, (icy - 7) * S)], (255, 118, 148, 255))
        el(d, ((icx - ixr * 0.64) * S, (icy - iyr * 0.68) * S,
               (icx - ixr * 0.12) * S, (icy - iyr * 0.16) * S), fill=(255, 255, 255, 250))
        el(d, ((icx + ixr * 0.18) * S, (icy + iyr * 0.32) * S,
               (icx + ixr * 0.54) * S, (icy + iyr * 0.68) * S), fill=(255, 255, 255, 175))
    # 上眼睑
    d.arc([(cx - rx) * S, (cy2 - ry2) * S, (cx + rx) * S, (cy2 + ry2) * S], 182, 358,
          fill=LINE, width=int(4.6 * S))
    # 睫毛尖
    poly(d, [((cx - rx * 0.96) * S, (cy2 - ry2 * 0.30) * S),
             ((cx - rx * 1.30) * S, (cy2 - ry2 * 0.58) * S),
             ((cx - rx * 0.74) * S, (cy2 - ry2 * 0.90) * S)], LINE)


def closed_eye(d, cx, cy, kind="happy"):
    if kind == "happy":
        d.arc([(cx - 30) * S, (cy - 12) * S, (cx + 30) * S, (cy + 30) * S], 190, 350,
              fill=LINE, width=int(4.6 * S))
    elif kind == "flat":
        # 平静闭眼：眼睑小幅下弯，直线会像两道横杠
        d.arc([(cx - 26) * S, (cy - 12) * S, (cx + 26) * S, (cy + 14) * S], 15, 165,
              fill=LINE, width=int(4.4 * S))
    elif kind == "sad":
        d.arc([(cx - 28) * S, (cy - 4) * S, (cx + 28) * S, (cy + 30) * S], 20, 160,
              fill=LINE, width=int(4.4 * S))
    # 闭着的眼角也要有睫毛，否则睁眼时的睫毛尖会凭空消失
    d.line([((cx - 25) * S, (cy - 3) * S), ((cx - 34) * S, (cy - 9) * S)],
           fill=LINE, width=int(3.2 * S))


def blush(d, alpha=1.0, strength=1.0):
    for cx in (64, 176):
        el(d, ((cx - 21 * strength) * S, 138 * S, (cx + 21 * strength) * S, 158 * S), fill=BLUSH)
        el(d, ((cx - 12 * strength) * S, 143 * S, (cx + 12 * strength) * S, 153 * S),
           fill=(255, 190, 202, 255))


TALK_MODE = False  # True 时所有嘴型统一换成张嘴，用来出说话贴片
MOUTH_BOX = (98, 140, 142, 192)  # 240 逻辑坐标，嘴周边一块纯皮肤区


def mouth(d, kind="smile"):
    if TALK_MODE:
        kind = "open"
    if kind == "smile":
        d.arc([106 * S, 146 * S, 134 * S, 168 * S], 20, 160, fill=MOUTH, width=int(3.6 * S))
    elif kind == "grin":
        d.chord([98 * S, 142 * S, 142 * S, 180 * S], 0, 180, fill=MOUTH)
        d.chord([108 * S, 160 * S, 132 * S, 178 * S], 0, 180, fill=TONGUE)
    elif kind == "open":
        el(d, (108 * S, 148 * S, 132 * S, 176 * S), fill=MOUTH)
        el(d, (112 * S, 164 * S, 128 * S, 175 * S), fill=TONGUE)
    elif kind == "o":
        el(d, (112 * S, 150 * S, 128 * S, 170 * S), fill=MOUTH)
    elif kind == "flat":
        d.line([(110 * S, 158 * S), (130 * S, 158 * S)], fill=MOUTH, width=int(3.6 * S))
    elif kind == "sad":
        d.arc([106 * S, 156 * S, 134 * S, 178 * S], 200, 340, fill=MOUTH, width=int(3.6 * S))
    elif kind == "wavy":
        pts = []
        for i in range(19):
            t = i / 18.0
            pts.append(((106 + 28 * t) * S, (158 + (3.5 if i % 2 else -3.5)) * S))
        d.line(pts, fill=MOUTH, width=int(3 * S), joint="curve")
    elif kind == "cat":
        d.arc([102 * S, 144 * S, 122 * S, 164 * S], 20, 160, fill=MOUTH, width=int(3.6 * S))
        d.arc([118 * S, 144 * S, 138 * S, 164 * S], 20, 160, fill=MOUTH, width=int(3.6 * S))


def tear(d, side=-1):
    cx = 88 if side < 0 else 152
    poly(d, [(cx * S, 146 * S), ((cx + 8) * S, 158 * S), ((cx - 8) * S, 158 * S)], (150, 214, 255, 255))
    el(d, ((cx - 8) * S, 152 * S, (cx + 8) * S, 178 * S), fill=(150, 214, 255, 255))


def sweat(d):
    poly(d, [(196 * S, 96 * S), (206 * S, 114 * S), (186 * S, 114 * S)], (140, 210, 255, 255))
    el(d, (186 * S, 104 * S, 206 * S, 128 * S), fill=(140, 210, 255, 255))


def add_outer_line(img, r=1):
    """最外圈：1px 亮实线 + 1px 半透明光晕，把头从深色背景里拔出来。
    逐形状 outline 只能画出每个形状自己的边，脸与头发、头发与外缘的交接处会留断口，
    整张 alpha 外扩再减回原 alpha，才能保证「头 vs 背景」是一条连续的线。
    ring 保留 grown-a 的差值而不是二值化，抗锯齿边缘得到的是一圈渐变实线，不是硬边。"""
    a = img.split()[3]

    def dilated(rad):
        g = a.copy()
        for dx in range(-rad, rad + 1):
            for dy in range(-rad, rad + 1):
                if dx or dy:
                    g = ImageChops.lighter(g, ImageChops.offset(a, dx, dy))
        return g

    g1 = dilated(1)
    g2 = dilated(2)
    out = Image.new("RGBA", img.size, (0, 0, 0, 0))
    halo = Image.new("RGBA", img.size, RIM_GLOW)
    halo.putalpha(ImageChops.subtract(g2, g1).point(lambda v: int(v * 0.55)))
    out.alpha_composite(halo)
    ring = Image.new("RGBA", img.size, RIM)
    ring.putalpha(ImageChops.subtract(g1, a))
    out.alpha_composite(ring)
    out.alpha_composite(img)
    return out


def compose(fn):
    img = canvas()
    fn(ImageDraw.Draw(img))
    return img.resize((SZ, SZ), Image.LANCZOS)


def build(spec):
    def fn(d):
        hair_back(d)
        face(d)
        hair_front(d)
        spec(d)
    return compose(fn)


def s_happy(d):
    brows(d, "normal")
    eye(d, 88, 118)
    eye(d, 152, 118)
    blush(d)
    mouth(d, "grin")


def s_neutral(d):
    brows(d, "normal")
    eye(d, 88, 118, rx=29, ry=33)
    eye(d, 152, 118, rx=29, ry=33)
    blush(d, strength=0.7)
    mouth(d, "smile")


def s_laugh(d):
    brows(d, "raised")
    closed_eye(d, 88, 118, "happy")
    closed_eye(d, 152, 118, "happy")
    blush(d)
    mouth(d, "grin")


def s_sad(d):
    brows(d, "sad")
    eye(d, 88, 120, lid=0.72, look=(0, 3))
    eye(d, 152, 120, lid=0.72, look=(0, 3))
    blush(d, strength=0.8)
    mouth(d, "sad")
    tear(d, -1)


def anger_mark(d):
    col = (255, 92, 112, 255)
    cx, cy, r = 196, 62, 22
    for a in (0, 90, 180, 270):
        rad = math.radians(a)
        ex, ey = cx + r * math.cos(rad), cy + r * math.sin(rad)
        d.line([((cx + r * 0.42 * math.cos(rad)) * S, (cy + r * 0.42 * math.sin(rad)) * S),
                (ex * S, ey * S)], fill=col, width=int(5 * S))


def s_angry(d):
    brows(d, "angry")
    eye(d, 88, 118, rx=29, ry=31)
    eye(d, 152, 118, rx=29, ry=31)
    mouth(d, "wavy")
    anger_mark(d)


def s_surprised(d):
    brows(d, "raised")
    eye(d, 88, 116, rx=33, ry=38)
    eye(d, 152, 116, rx=33, ry=38)
    mouth(d, "o")


def s_love(d):
    brows(d, "normal")
    eye(d, 88, 118, heart=True)
    eye(d, 152, 118, heart=True)
    blush(d, strength=1.2)
    mouth(d, "cat")


def s_sleepy(d):
    brows(d, "normal")
    closed_eye(d, 88, 122, "flat")
    closed_eye(d, 152, 122, "flat")
    blush(d, strength=0.6)
    mouth(d, "o")


def s_shy(d):
    brows(d, "sad")
    eye(d, 88, 120, lid=0.85, look=(-4, 2))
    eye(d, 152, 120, lid=0.85, look=(-4, 2))
    blush(d, strength=1.4)
    mouth(d, "wavy")


def s_thinking(d):
    brows(d, "sad")
    eye(d, 88, 116, look=(5, -4))
    eye(d, 152, 116, look=(5, -4))
    mouth(d, "flat")


def s_shocked(d):
    brows(d, "raised")
    eye(d, 88, 114, rx=34, ry=39)
    eye(d, 152, 114, rx=34, ry=39)
    mouth(d, "open")
    sweat(d)


def sunglasses(d):
    for cx in (88, 152):
        d.rounded_rectangle([(cx - 30) * S, 100 * S, (cx + 30) * S, 140 * S],
                            radius=14 * S, fill=(38, 40, 58, 255), outline=LINE, width=int(3 * S))
        d.line([((cx - 22) * S, 112 * S), ((cx - 2) * S, 152 * S)], fill=(120, 132, 170, 160), width=int(5 * S))
    d.line([(120 * S, 108 * S), (120 * S, 112 * S)], fill=LINE, width=int(5 * S))
    d.line([(58 * S, 106 * S), (46 * S, 100 * S)], fill=LINE, width=int(4 * S))
    d.line([(182 * S, 106 * S), (194 * S, 100 * S)], fill=LINE, width=int(4 * S))


def tongue(d, side=0):
    cx = 120 + side
    el(d, ((cx - 13) * S, 158 * S, (cx + 13) * S, 186 * S), fill=TONGUE)
    d.line([(cx * S, 158 * S), (cx * S, 182 * S)], fill=(226, 108, 122, 200), width=int(2 * S))


def drool(d):
    poly(d, [(132 * S, 166 * S), (140 * S, 180 * S), (124 * S, 180 * S)], (168, 214, 255, 235))
    el(d, (122 * S, 172 * S, 142 * S, 198 * S), fill=(168, 214, 255, 235))
    el(d, (187 * S, 104 * S, 197 * S, 116 * S), fill=(168, 214, 255, 210))


def heart(d, cx, cy, s, col=(255, 108, 140, 255)):
    el(d, ((cx - s * 0.9) * S, (cy - s * 0.9) * S, (cx - s * 0.05) * S, (cy) * S), fill=col)
    el(d, ((cx + s * 0.05) * S, (cy - s * 0.9) * S, (cx + s * 0.9) * S, (cy) * S), fill=col)
    poly(d, [((cx - s * 0.88) * S, (cy - s * 0.3) * S), ((cx + s * 0.88) * S, (cy - s * 0.3) * S),
             (cx * S, (cy + s * 0.95) * S)], col)


def s_funny(d):
    brows(d, "raised")
    closed_eye(d, 88, 116, "happy")
    closed_eye(d, 152, 116, "happy")
    blush(d)
    mouth(d, "grin")
    for side, sx in ((1, 66), (-1, 174)):
        poly(d, [(sx * S, 132 * S), ((sx + side * 9) * S, 144 * S), ((sx - side * 4) * S, 146 * S)],
             (150, 214, 255, 240))


def s_crying(d):
    brows(d, "sad")
    eye(d, 88, 120, lid=0.62, look=(0, 4))
    eye(d, 152, 120, lid=0.62, look=(0, 4))
    for cx in (88, 152):
        d.rounded_rectangle([(cx - 10) * S, 140 * S, (cx + 10) * S, 190 * S], radius=9 * S,
                            fill=(150, 214, 255, 230))
        el(d, ((cx - 11) * S, 168 * S, (cx + 11) * S, 198 * S), fill=(150, 214, 255, 230))
    blush(d, strength=1.1)
    if TALK_MODE:
        mouth(d, "open")
    else:
        d.ellipse([104 * S, 148 * S, 136 * S, 182 * S], fill=MOUTH)
        el(d, (108 * S, 164 * S, 132 * S, 180 * S), fill=TONGUE)


def s_winking(d):
    brows(d, "normal")
    eye(d, 88, 118)
    closed_eye(d, 152, 118, "happy")
    blush(d)
    mouth(d, "cat")


def s_cool(d):
    brows(d, "normal")
    sunglasses(d)
    mouth(d, "smile")
    if not TALK_MODE:
        d.line([(104 * S, 152 * S), (136 * S, 152 * S)], fill=MOUTH, width=int(3.6 * S))


def s_relaxed(d):
    brows(d, "normal")
    closed_eye(d, 88, 120, "happy")
    closed_eye(d, 152, 120, "happy")
    blush(d, strength=0.8)
    mouth(d, "smile")


def s_delicious(d):
    brows(d, "normal")
    eye(d, 88, 118, lid=0.58, look=(0, 2))
    eye(d, 152, 118, lid=0.58, look=(0, 2))
    blush(d, strength=1.1)
    mouth(d, "o")
    drool(d)


def s_kissy(d):
    brows(d, "normal")
    closed_eye(d, 88, 118, "happy")
    eye(d, 152, 118, look=(-3, 1))
    blush(d, strength=1.3)
    if TALK_MODE:
        mouth(d, "open")
    else:
        el(d, (112 * S, 152 * S, 128 * S, 168 * S), fill=MOUTH)
    heart(d, 186, 92, 15)
    heart(d, 200, 62, 10)


def s_confident(d):
    brows(d, "normal")
    eye(d, 88, 120, lid=0.55, look=(4, 2))
    eye(d, 152, 120, lid=0.55, look=(4, 2))
    mouth(d, "cat")


def s_silly(d):
    brows(d, "normal")
    eye(d, 88, 118)
    closed_eye(d, 152, 118, "happy")
    blush(d)
    if TALK_MODE:
        mouth(d, "open")
    else:
        d.arc([104 * S, 146 * S, 136 * S, 168 * S], 20, 160, fill=MOUTH, width=int(3.6 * S))
    tongue(d)


def s_confused(d):
    brows(d, "raised")
    eye(d, 88, 116, look=(7, -11))
    eye(d, 152, 116, look=(7, -11))
    mouth(d, "flat")
    sweat(d)


def s_talk_small(d):
    brows(d, "normal")
    eye(d, 88, 118, rx=29, ry=33)
    eye(d, 152, 118, rx=29, ry=33)
    blush(d, strength=0.7)
    el(d, (110 * S, 150 * S, 130 * S, 168 * S), fill=MOUTH)


def s_talk_open(d):
    brows(d, "normal")
    eye(d, 88, 118, rx=29, ry=33)
    eye(d, 152, 118, rx=29, ry=33)
    blush(d, strength=0.7)
    el(d, (108 * S, 148 * S, 132 * S, 178 * S), fill=MOUTH)
    el(d, (112 * S, 166 * S, 128 * S, 177 * S), fill=TONGUE)


REGISTRY = {
    "neutral": s_neutral,
    "happy": s_happy,
    "laughing": s_laugh,
    "funny": s_funny,
    "sad": s_sad,
    "angry": s_angry,
    "crying": s_crying,
    "loving": s_love,
    "embarrassed": s_shy,
    "surprised": s_surprised,
    "shocked": s_shocked,
    "thinking": s_thinking,
    "winking": s_winking,
    "cool": s_cool,
    "relaxed": s_relaxed,
    "delicious": s_delicious,
    "kissy": s_kissy,
    "confident": s_confident,
    "sleepy": s_sleepy,
    "silly": s_silly,
    "confused": s_confused,
}

ANIM = {}

# 说话张嘴不做整张脸：只在嘴周边一块纯皮肤区上换嘴型，一张才 22×26×3 字节
MOUTH_W = (MOUTH_BOX[2] - MOUTH_BOX[0]) // 2
MOUTH_H = (MOUTH_BOX[3] - MOUTH_BOX[1]) // 2
MOUTH_OX = MOUTH_BOX[0] // 2
MOUTH_OY = MOUTH_BOX[1] // 2


OUT = 120
# 本脚本在 tools/ 下，产物要落到项目根的 main/display/ 与 _emoji_out/
HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def rgba(name, fn, talk=False):
    global TALK_MODE
    TALK_MODE = talk
    try:
        img = build(fn)
    finally:
        TALK_MODE = False
    return add_outer_line(img.resize((OUT, OUT), Image.LANCZOS))


def mouth_patch(fn):
    global TALK_MODE
    TALK_MODE = True
    try:
        img = build(fn)
    finally:
        TALK_MODE = False
    return img.crop(MOUTH_BOX).resize((MOUTH_W, MOUTH_H), Image.LANCZOS)


# 闭眼风格按表情挑：可爱系用上弯弧，哭丧系用下弯弧，其余直线
BLINK_STYLES = {
    "happy": "happy", "loving": "happy", "kissy": "happy", "silly": "happy",
    "winking": "happy", "delicious": "happy", "confident": "happy", "relaxed": "happy",
    "sad": "sad", "crying": "sad", "embarrassed": "sad",
}


# 只认真正换了内容的像素：发丝/抗锯齿那种 7 个色阶的抖动不算睁闭眼差异，
# 把它们写进贴片只会在染色遮罩的空隙里露出原蓝
BLINK_DIFF_MIN = 24


def render_blink(fn, style):
    global BLINK_MODE, BLINK_STYLE
    BLINK_MODE, BLINK_STYLE = True, style
    try:
        img = build(fn)
    finally:
        BLINK_MODE, BLINK_STYLE = False, "flat"
    # 必须和 rgba() 走同一套外轮廓：少了这圈，睁眼版有轮廓、闭眼版没有，
    # 整圈外轮廓全被算成「睁闭眼差异」，眨眼贴片从 72x42 涨到 104x110（+750KB flash）
    return add_outer_line(img.resize((OUT, OUT), Image.LANCZOS))


def diff_bbox(a, b):
    d = ImageChops.difference(a.convert("RGB"), b.convert("RGB"))
    d = d.point(lambda v: 255 if v > BLINK_DIFF_MIN else 0)
    return d.getbbox()


def blink_patches(items):
    """每张表情渲染一版闭眼，取「睁眼版 vs 闭眼版」差异包围盒的并集，只存这块。
    本来就闭眼/戴墨镜的表情差异为空，自然不生成贴片（它们不需要眨）。
    返回 (贴片, 包围盒, 各表情的闭眼整脸)，闭眼整脸留给眨眼遮罩复用，不必重渲染。"""
    closed = {}
    box = None
    for name, fn in REGISTRY.items():
        b = render_blink(fn, BLINK_STYLES.get(name, "flat"))
        df = diff_bbox(items[name], b)
        if df is None:
            continue
        closed[name] = b
        box = df if box is None else (min(box[0], df[0]), min(box[1], df[1]),
                                      max(box[2], df[2]), max(box[3], df[3]))
    if box is None:
        return {}, None, {}
    # 贴片宽高取偶，LVGL 内部按 2 字节 stride 拷贝时不会踩边
    box = (box[0] & ~1, box[1] & ~1, box[2] + (box[2] & 1), box[3] + (box[3] & 1))
    patches = {n: blink_patch(items[n], img, box) for n, img in closed.items()}
    return patches, box, closed


def blink_patch(a, b, box):
    """贴片只承载「睁眼→闭眼真正变化」的像素，其余一律 alpha=0。
    头发/眉毛/腮红/皮肤两版完全一样，贴片一碰它们就出事：写进头发像素会在染色层下方闪回原蓝，
    留空又会把刘海下缘啃出锯齿和一条缝。只交换差异像素，眼睛以外的地方谁都别动。"""
    ac, bc = a.crop(box), b.crop(box)
    w, h = ac.size
    out = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    pa, pb, po = ac.load(), bc.load(), out.load()
    for y in range(h):
        for x in range(w):
            p, q = pa[x, y], pb[x, y]
            if max(abs(q[k] - p[k]) for k in range(4)) > BLINK_DIFF_MIN:
                po[x, y] = q
    return out


def blink_hair_patches(closed, box):
    """眨眼时贴片会带出「睁眼被眼球盖住、闭眼才露出的刘海」，这些像素在睁眼版遮罩里是空的，
    于是闪回原蓝。这里给每个会眨的表情再出一张闭眼版遮罩的局部裁片，贴片上按发色染一遍。"""
    if box is None:
        return {}
    out = {}
    for name, img in closed.items():
        out[name] = hair_masks({name: img})[name].crop(box)
    return out


def to_rgb565a8(img):
    w, h = img.size
    px = img.load()
    rgb = bytearray()
    alpha = bytearray()
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
            rgb.append(v & 0xFF)
            rgb.append((v >> 8) & 0xFF)
            alpha.append(a)
    return bytes(rgb) + bytes(alpha)


def to_a8(img):
    w, h = img.size
    px = img.load()
    return bytes(px[x, y] for y in range(h) for x in range(w))


def c_array(name, data, per_line=20):
    lines = []
    for i in range(0, len(data), per_line):
        lines.append("    " + ",".join("0x%02x" % b for b in data[i:i + per_line]) + ",")
    return "static const uint8_t %s_map[] = {\n%s\n};\n" % (name, "\n".join(lines))


def export_png(out, items):
    for name, img in items.items():
        bg = Image.new("RGB", (OUT, OUT), (12, 12, 16))
        bg.paste(img, (0, 0), img)
        bg.save(os.path.join(out, name + ".png"))


def sheet(out, names):
    cols = 5
    rows = (len(names) + cols - 1) // cols
    cell = 128
    img = Image.new("RGB", (cols * cell, rows * cell), (12, 12, 16))
    for i, name in enumerate(names):
        im = Image.open(os.path.join(out, name + ".png"))
        img.paste(im, ((i % cols) * cell + 4, (i // cols) * cell + 4))
    img.save(os.path.join(out, "_sheet.png"))


def dsc(name, w, h):
    return ("\nconst lv_image_dsc_t %s = {\n"
            "    .header.magic = LV_IMAGE_HEADER_MAGIC,\n"
            "    .header.cf = LV_COLOR_FORMAT_RGB565A8,\n"
            "    .header.flags = 0,\n"
            "    .header.w = %d,\n"
            "    .header.h = %d,\n"
            "    .header.stride = %d,\n"
            "    .data_size = %d,\n"
            "    .data = %s_map,\n};\n" % (name, w, h, w * 2, w * h * 3, name))


def dsc_a8(name, w, h):
    return ("\nconst lv_image_dsc_t %s = {\n"
            "    .header.magic = LV_IMAGE_HEADER_MAGIC,\n"
            "    .header.cf = LV_COLOR_FORMAT_A8,\n"
            "    .header.flags = 0,\n"
            "    .header.w = %d,\n"
            "    .header.h = %d,\n"
            "    .header.stride = %d,\n"
            "    .data_size = %d,\n"
            "    .data = %s_map,\n};\n" % (name, w, h, w, w * h, name))


def talk_sheet(out, items, mouths, scale=2):
    names = list(mouths.keys())
    cols = 6
    rows = (len(names) + cols - 1) // cols
    cell = OUT * scale + 8
    img = Image.new("RGB", (cols * cell, rows * cell), (12, 12, 16))
    for i, name in enumerate(names):
        base = items[name].copy()
        base.alpha_composite(mouths[name], (MOUTH_OX, MOUTH_OY))
        bg = Image.new("RGB", (OUT, OUT), (12, 12, 16))
        bg.paste(base, (0, 0), base)
        bg = bg.resize((OUT * scale, OUT * scale), Image.LANCZOS)
        img.paste(bg, ((i % cols) * cell + 4, (i // cols) * cell + 4))
    img.save(os.path.join(out, "_sheet_talk.png"))


def export_c(items, mouths, blinks, bbox, hair, blink_hair):
    hdr = os.path.join(HERE, "main", "display", "emoji_assets.h")
    src = os.path.join(HERE, "main", "display", "emoji_assets.c")
    names = list(items.keys())
    bw = (bbox[2] - bbox[0]) if bbox else 0
    bh = (bbox[3] - bbox[1]) if bbox else 0
    with open(hdr, "w", encoding="utf-8") as f:
        f.write("// 由 tools/gen_emoji_assets.py 生成，勿手改\n")
        f.write("#pragma once\n\n#include \"lvgl.h\"\n\n")
        f.write("#define EMOJI_W %d\n#define EMOJI_H %d\n" % (OUT, OUT))
        f.write("#define MOUTH_W %d\n#define MOUTH_H %d\n" % (MOUTH_W, MOUTH_H))
        f.write("#define MOUTH_OX %d\n#define MOUTH_OY %d\n" % (MOUTH_OX, MOUTH_OY))
        f.write("#define BLINK_W %d\n#define BLINK_H %d\n" % (bw, bh))
        f.write("#define BLINK_OX %d\n#define BLINK_OY %d\n\n" % (bbox[0], bbox[1]))
        f.write("typedef struct {\n    const char * name;\n    const lv_image_dsc_t * img;\n"
                "    const lv_image_dsc_t * mouth;\n    const lv_image_dsc_t * blink;\n"
                "    const lv_image_dsc_t * blink_hair;\n"
                "    const lv_image_dsc_t * hair;\n} emoji_asset_t;\n\n")
        f.write("extern const emoji_asset_t emoji_table[];\nextern const int emoji_table_count;\n")
        for name in names:
            f.write("extern const lv_image_dsc_t emoji_%s_hair;\n" % name)
        for name in names:
            f.write("extern const lv_image_dsc_t emoji_%s;\n" % name)
            if name in mouths:
                f.write("extern const lv_image_dsc_t emoji_%s_mouth;\n" % name)
            if name in blinks:
                f.write("extern const lv_image_dsc_t emoji_%s_blink;\n" % name)
                f.write("extern const lv_image_dsc_t emoji_%s_blink_hair;\n" % name)
    with open(src, "w", encoding="utf-8") as f:
        f.write("// 由 tools/gen_emoji_assets.py 生成，勿手改\n")
        f.write("#include \"emoji_assets.h\"\n\n")
        for name in names:
            f.write(c_array("emoji_" + name, to_rgb565a8(items[name])))
            if name in mouths:
                f.write(c_array("emoji_" + name + "_mouth", to_rgb565a8(mouths[name])))
            if name in blinks:
                f.write(c_array("emoji_" + name + "_blink", to_rgb565a8(blinks[name])))
                f.write(c_array("emoji_" + name + "_blink_hair", to_a8(blink_hair[name])))
        for name in names:
            f.write(c_array("emoji_" + name + "_hair", to_a8(hair[name])))
        for name in names:
            f.write(dsc("emoji_" + name, OUT, OUT))
            if name in mouths:
                f.write(dsc("emoji_" + name + "_mouth", MOUTH_W, MOUTH_H))
            if name in blinks:
                f.write(dsc("emoji_" + name + "_blink", bw, bh))
                f.write(dsc_a8("emoji_" + name + "_blink_hair", bw, bh))
        for name in names:
            f.write(dsc_a8("emoji_" + name + "_hair", OUT, OUT))
        f.write("\nconst emoji_asset_t emoji_table[] = {\n")
        for name in names:
            talk = "&emoji_%s_mouth" % name if name in mouths else "NULL"
            blink = "&emoji_%s_blink" % name if name in blinks else "NULL"
            blink_hair = "&emoji_%s_blink_hair" % name if name in blinks else "NULL"
            f.write("    {\"%s\", &emoji_%s, %s, %s, %s, &emoji_%s_hair},\n"
                    % (name, name, talk, blink, blink_hair, name))
        f.write("};\n\nconst int emoji_table_count = sizeof(emoji_table) / sizeof(emoji_table[0]);\n")
    return src, os.path.getsize(src)


if __name__ == "__main__":
    out = os.path.join(HERE, "_emoji_out")
    os.makedirs(out, exist_ok=True)
    items = {}
    for name, fn in list(REGISTRY.items()) + list(ANIM.items()):
        items[name] = rgba(name, fn)
    mouths = {name: mouth_patch(fn) for name, fn in REGISTRY.items()}
    blinks, bbox, closed = blink_patches(items)
    hair = hair_masks(items)
    blink_hair = blink_hair_patches(closed, bbox)
    for n, im in hair.items():
        im.save(os.path.join(out, "_hair_%s.png" % n))
    export_png(out, items)
    for name, img in mouths.items():
        img.save(os.path.join(out, "_mouth_%s.png" % name))
    for name, img in blinks.items():
        img.save(os.path.join(out, "_blink_%s.png" % name))
    sheet(out, list(REGISTRY.keys()))
    talk_sheet(out, items, mouths)
    src, size = export_c(items, mouths, blinks, bbox, hair, blink_hair)
    bw, bh = (bbox[2] - bbox[0], bbox[3] - bbox[1]) if bbox else (0, 0)
    total = len(items) * OUT * OUT * 3 + len(mouths) * MOUTH_W * MOUTH_H * 3 + len(blinks) * bw * bh * 4 + len(items) * OUT * OUT
    print("generated %d faces + %d mouth patches + %d blink patches -> %s"
          % (len(items), len(mouths), len(blinks), out))
    print("mouth patch %dx%d at (%d,%d)" % (MOUTH_W, MOUTH_H, MOUTH_OX, MOUTH_OY))
    print("blink patch %dx%d at (%d,%d), faces: %s" % (bw, bh, bbox[0], bbox[1], ",".join(blinks)))
    print("wrote %s (%.0f KB source, %.0f KB binary)" % (src, size / 1024.0, total / 1024.0))
