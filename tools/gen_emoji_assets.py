# -*- coding: utf-8 -*-
"""二次元 Q 版表情生成器：4x 超采样绘制，LANCZOS 降采样得平滑描边。

生成 main/display/emoji_assets.c / .h（固件内嵌表情、口型贴片、眨眼贴片、发音遮罩）。
用法（项目根目录）：python tools/gen_emoji_assets.py
需要 Pillow。输出 120x120 RGBA，转 RGB565A8 嵌入固件。
"""
import os
import math
import re
from collections import deque

import numpy as np
from PIL import Image, ImageChops, ImageDraw, ImageFilter

S = 4
SZ = 240
W = SZ * S

# 最终输出尺寸（逻辑 240 画布的降采样目标）。
# 屏只有 240 高，162 是「脸占满 + 给 HUD 留 78px（天气 3 行 72px）」的上限
OUT = 162
K = OUT / float(SZ)


# 静态骨架（头发/发带/蝴蝶结/鳍耳/身体/尾巴）全部来自参考立绘，由
# tools/_make_base.py 把五官填成皮肤后输出。多边形拼贴永远追不上手绘的质感，
# 这是唯一能让整体"像"的路；表情仍然由代码画，所以 21 张脸/眨眼/口型全部保留。
BASE_PNG = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                        "tools", "_ref_assets", "base.png")
# 头部裁切（base.png 像素）：x 0~1000 是整张底板的横向全宽。
# 不能只裁到头图的 888 宽——两侧长发在 y>700 会向外扩散到 950，
# 身体图若跟着裁到 888 就在那里留下一条竖直硬切边（上一版小人两侧的矩形块）。
# 现在头/身同宽同比例（240/1000），拼接边界完全重合。
HEAD_CROP = (0, 0, 1000, 700)
HEAD_K = 240.0 / 1000.0  # 整图宽占满 240 逻辑画布
HEAD_OX = 0
# 纵向锚点：也让角色的眼睛中心落在与大肥鱼同一个画布高度上。
# 之前只对了横向（head_ox），纵向永远贴 y=0，各角色眼位在原图里高低不一，
# 屏上就是「有的头顶大片空、眼睛几乎挂到下巴」。
HEAD_OY = 0
# 源立绘缓存（apply_cast 里清）；放模块顶部是因为 apply_cast 比它的定义处先引用
_SRC_CACHE = {}


def sc(v):
    """逻辑单位 → 输出像素。尺寸/坐标一律过这里，别写死 //2。"""
    return int(round(v * K))

SKIN = (255, 240, 231, 255)
SKIN_SH = (248, 212, 202, 255)
# 眉毛/睫毛原来用暖棕 (72,42,40)，在蓝发旁边发脏；参考图的线是冷紫黑
LINE = (58, 44, 68, 255)
# 三档发色必须同时满足 hair_masks 的判据（b>=140 且 b-r>=40），否则深发根会被判成
# 「非头发」，换发色时头顶留一块原色，所以深档只能压到 b=178 不能再深。
# 与参考图对齐的关键不是绝对色值，是明度跨度：深档到亮档拉开 100 级，
# 上一版只拉开 64 级，整颗头读成一块平涂的浅蓝塑料
HAIR = (62, 110, 200, 255)
HAIR_DK = (38, 58, 128, 255)
HAIR_LT = (140, 220, 252, 255)
WHITE = (255, 255, 255, 255)
IRIS_TOP = (30, 52, 124, 255)
IRIS_BOT = (128, 208, 250, 255)
MOUTH = (226, 112, 126, 255)
TONGUE = (255, 142, 152, 255)
BLUSH = (255, 172, 188, 255)
# 轮廓线专用色：中性深紫黑。不能跟 LINE 共用暖棕，否则在蓝发上发脏；
# 也不能偏蓝，否则用户把发色改成红/绿后描边和头发打起来。
# 必须同时满足两个条件：
#   1) 不落入 hair_masks 的任一条判据（b-r<40 且非亮白），否则描边会被拉进 A8 遮罩，
#      随 image_recolor 跟着发色一起染，改发色时轮廓就消失了
#   2) 降采样后与发色混出的中间像素要尽量深，才不会被判成头发（见 OUTLINE_W 注释）
OUTLINE = (46, 38, 52, 255)
OUTLINE_W = 5.0  # 逻辑单位；162px 输出上约 3.4px。3.5 时只剩 2.4px，屏上读不出线
# 最外圈边缘光。屏幕背景是近黑的科技底（_bg_out/bg_0.png 实测），
# 深色轮廓在黑底上等于隐形——这就是“没线条感”的真正原因。外圈必须用亮色才有对比。
RIM = (168, 200, 246, 255)
RIM_GLOW = (96, 140, 220, 255)

# 发带/蝴蝶结/鳍耳三组色。刻意不复用 HAIR：它们不跟发色走，用户换发色时要保持原样
BAND = (247, 251, 255, 255)
RIBBON = (104, 176, 255, 255)
RIBBON_DK = (64, 128, 216, 255)
# 鳍耳刻意压到 b<140：hair_masks 把「b>=140 且 b-r>=40」判成头发，
# 浅蓝鳍耳会在换发色时跟着一起变色
# 鳍耳必须同时躲开两条判据：b<118（不然被当成头发染色），且 b-r<55。
# (78,94,138) 在旧判据 b>=140 下安全，门槛降到 118 后就会被吃进去
FIN = (72, 84, 118, 255)
FIN_DK = (46, 58, 96, 255)


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


HB_CX, HB_CY, HB_RX, HB_RY = 120, 120, 90, 94
# 参考图的头发是「一个整体轮廓包住脸 + 两侧垂到腰」，不是「头上一圆球 + 旁边两片」。
# 拆成三块画出来读成贴片，所以整个头发只能是一条闭合轮廓。
HAIR_TOP = 24
HAIR_RX, HAIR_CY = 104, 132
HAIR_BOT = 210


def _hair_grad(t):
    """0=发根 → 1=发梢，三段线性插值。4x 超采样降采样后色阶差小于 1 个量化级，是连续渐变。"""
    stops = (HAIR_DK, HAIR, HAIR_LT)
    t = min(1.0, max(0.0, t))
    n = len(stops) - 1
    i = min(int(t * n), n - 1)
    u = t * n - i
    a, b = stops[i], stops[i + 1]
    return tuple(int(a[k] + (b[k] - a[k]) * u) for k in range(3)) + (255,)


def _hair_silhouette(n=40, n2=26, n3=22):
    """头发整体轮廓（逻辑坐标，顺时针）：顶部椭圆弧 → 右外缘下拖 → 波浪发梢 → 左外缘上行。
    下半部分必须一直垂到 y=230：长发是这颗头一半的识别度，只画到下巴就变成短披肩。"""
    pts = []
    for i in range(n + 1):                       # 顶部弧：左太阳穴 → 头顶 → 右太阳穴
        th = math.pi - math.pi * i / n
        pts.append((120 + HAIR_RX * math.cos(th),
                    HAIR_CY - (HAIR_CY - HAIR_TOP) * math.sin(th)))

    def edge_x(t):                               # 侧边外缘：保持体积，上半段收、下半段卷
        # 三个大 lobe 是参考图那种泳松卷发的最低成本近似。
        # 幅度必须给到 20：14 时缩到屏上只剩一条直线，上一版就亏在这里
        return 120 + HAIR_RX * (1.0 - 0.04 * t) + 20 * t ** 0.6 * math.sin(t * math.pi * 2.2)

    for i in range(1, n2 + 1):                   # 右外缘：往下
        t = i / float(n2)
        pts.append((edge_x(t), HAIR_CY + (HAIR_BOT - HAIR_CY) * t))
    xr = pts[-1][0]
    xl = 240 - xr
    for i in range(1, n3):                       # 发稍：右 → 左，波浪收尾
        u = i / float(n3)
        # 3 个大卷而不是 5 个小波：5 个缩到屏上就是一圈锯齿裙边。
        # 中段再上提 18（-18*sin）是为了让下巴下方开一个 U 形：
        # 长发在两侧分开、露出脖子，否则脸下面是一整块蓝，读成围裙
        pts.append((xr + (xl - xr) * u,
                    HAIR_BOT - 12 + 18 * (0.5 - 0.5 * math.cos(2 * math.pi * 3 * u)) ** 1.2
                    - 18 * math.sin(math.pi * u) ** 1.5))
    for i in range(n2, 0, -1):                   # 左外缘：往上
        t = i / float(n2)
        pts.append((240 - edge_x(t), HAIR_CY + (HAIR_BOT - HAIR_CY) * t))
    return pts


def hair_back(d, fill=None, outline=True):
    """头发底座：一条闭合轮廓 + 逐行渐变（发根深、发梢亮）。
    渐变必须走 mask 而不是「一行一条横线填满宽」：横线填满会把两束垂发之间的脸区也贴上色，
    脸再盖回来就只剩一条硬边。没有画布引用时（hair_masks 的几何重放）退化成单色。"""
    ow = int(OUTLINE_W * S)
    px = [(int(x * S), int(y * S)) for x, y in _hair_silhouette()]
    if fill is not None:
        poly(d, px, fill, OUTLINE if outline else None, ow)
        return
    img = getattr(d, "_img_ref", None)
    if img is None:
        poly(d, px, HAIR, OUTLINE if outline else None, ow)
        return
    mask = Image.new("L", (W, W), 0)
    ImageDraw.Draw(mask).polygon(px, fill=255)
    grad = Image.new("RGBA", (W, W), (0, 0, 0, 0))
    gd = ImageDraw.Draw(grad)
    for y in range(HAIR_TOP, HAIR_BOT + 12):
        # 0.95 次幂：让深色集中在头顶，发梢亮起来更像染的，线性会读成上下两块色
        grad_row = _hair_grad(((y - HAIR_TOP) / float(HAIR_BOT - HAIR_TOP)) ** 0.78)
        gd.line([(0, y * S), (W, y * S)], fill=grad_row, width=S + 1)
    grad.putalpha(mask)
    img.alpha_composite(grad)
    if outline:
        d.polygon(px, outline=OUTLINE, width=ow)


def _hair_edge(x):
    """发际弧。hair_front 和刘海发束共用这一条，改一处两边一起动，不会错位。
    参考图的刘海垂得很低，额头只露一窄条：脸顶 80、眉区 112~124，
    所以下缘落在 96~110，既不盖眉也不露出大片脑门。"""
    return 100 - 9 * math.cos((x - 120) * math.pi / 80.0)


def ahoge(d, col, ol, ow):
    """头顶那根上翘呆毛。参考图里它是整张图的记忆点，必须画成弯钩。
    不能用 line：单线条会被最外圈边缘光吃光，缩到 162px 就只剩一个白点。
    所以按二次贝塞尔中心线向两侧扰成一条渐细的面，根部半宽 7.5 逻辑（输出约 10px）。"""
    # 参考图的呆毛从头顶中央起笔、向左上甩得很长，尖端回钩。
    # 上一版 (100,28)-(94,-4)-(56,8) 横向只跨 44，缩到屏上是头顶一粒小钩。
    # 控制点的 y 不能再往负走：贝塞尔中点 = 0.25*24+0.5*(-14)+0.25*2 ≈ 1，已在画布边缘
    p0, p1, p2 = (114, 24), (98, -14), (46, 2)
    N = 22
    left, right = [], []
    for i in range(N + 1):
        t = i / float(N)
        mt = 1.0 - t
        cx = mt * mt * p0[0] + 2 * mt * t * p1[0] + t * t * p2[0]
        cy = mt * mt * p0[1] + 2 * mt * t * p1[1] + t * t * p2[1]
        dx = 2 * mt * (p1[0] - p0[0]) + 2 * t * (p2[0] - p1[0])
        dy = 2 * mt * (p1[1] - p0[1]) + 2 * t * (p2[1] - p1[1])
        ln = math.hypot(dx, dy) or 1.0
        nx, ny = -dy / ln, dx / ln
        # 参考图的呆毛是根细钩子（根部约 13 逻辑宽），上一版半宽 21 画出来是一把砍刀
        w = 11.0 * (1.0 - t) ** 0.55 + 2.4
        left.append(((cx + nx * w) * S, (cy + ny * w) * S))
        right.append(((cx - nx * w) * S, (cy - ny * w) * S))
    poly(d, left + right[::-1], col, ol, ow)


def hair_front(d, fill=None, dark=None, highlight=True, outline=True):
    F = HAIR if fill is None else fill
    K = HAIR_DK if dark is None else dark
    ol = OUTLINE if outline else None
    ow = int(OUTLINE_W * S)
    # 刘海下缘：5 缕长度交错的尖发。上一版是一条均匀波浪，缩到屏上
    # 读成“蓝布边上缝了花边”——参考图的识别点就在发缕一长一短、中间分缝。
    # 下缘用 sin 包络而不是直线：直线拼出来是一圈齿轮，参考图的发缕是圆润的舌形，
    # 尖端带弧度。126 是眼睛上缘，发缕最长的只到 124：再长就把眉毛全盖了
    # 4 缕改为 3 片宽舌，缕与缕之间只回到浅谷 LEAD 而不是回到发根：
    # 上一版每缕画完回到 y=86，缶下来就是 4 座尖塔夹 3 个大白三角，脸被遮成只剩下巴。
    # 参考图的刘海是连成一片的，底边只微微起伏
    # 斜刘海：参考图不是中分也不是齐平，是从左太阳穴到右太阳穴逐渐压低的斜片。
    # 左侧短（100）露出额头，右侧长（126）盖住右眼外上角
    LOCKS = ((72, 100, 26), (118, 112, 26), (164, 126, 26))
    LEAD = 96
    pts = [(42 * S, 92 * S)]
    for cx, tip, half in LOCKS:
        pts.append(((cx - half) * S, LEAD * S))
        for i in range(1, 11):
            u = i / 10.0
            pts.append((((cx - half) + 2 * half * u) * S,
                        (LEAD + (tip - LEAD) * math.sin(math.pi * u) ** 1.35) * S))
        pts.append(((cx + half) * S, LEAD * S))
    pts.append((200 * S, 92 * S))
    pts.append((212 * S, 40 * S))
    for i in range(41):                          # 顶部外弧，右 → 左
        t = math.pi * i / 40.0
        pts.append(((120 + 100 * math.cos(t)) * S, (104 - 84 * math.sin(t)) * S))
    pts.append((28 * S, 40 * S))
    # 鬓发：从太阳穴垂到下巴以下。内侧不能压到脸上，外缘带一点弯，
    # 直上直下的多边形在屏上是个方框
    side_l = [(26, 84), (52, 108), (50, 140), (40, 172), (26, 196), (18, 168), (20, 132), (22, 100)]
    side_r = [(214, 84), (188, 108), (190, 140), (200, 172), (214, 196), (222, 168), (220, 132), (218, 100)]
    shapes = [pts,
              [(x * S, y * S) for x, y in side_l],
              [(x * S, y * S) for x, y in side_r]]
    # 呆毛不在这一层画了：它必须压在喀秋莎外面，画在这里会被发带整根盖掉。
    # 改到 build() 的 accessories 之后，hair_masks 里单独补一次几何重放
    #
    # 刘海和鬓发也必须走「发根深→发梢亮」的渐变：上一版只有 hair_back 有渐变，
    # 前面这层是纯色 HAIR，屏上读成「一圈平涂的蓝布贴在脸上」。
    # fill 不为空时是 hair_masks 的几何重放，必须走单色分支
    img = getattr(d, "_img_ref", None)
    if fill is not None or img is None:
        for sh in shapes:
            poly(d, sh, F, ol, ow)
    else:
        mask = Image.new("L", (W, W), 0)
        md = ImageDraw.Draw(mask)
        for sh in shapes:
            md.polygon([(int(x), int(y)) for x, y in sh], fill=255)
        grad = Image.new("RGBA", (W, W), (0, 0, 0, 0))
        gd = ImageDraw.Draw(grad)
        for y in range(HAIR_TOP, HAIR_BOT + 12):
            gd.line([(0, y * S), (W, y * S)],
                    fill=_hair_grad(((y - HAIR_TOP) / float(HAIR_BOT - HAIR_TOP)) ** 0.78),
                    width=S + 1)
        grad.putalpha(mask)
        img.alpha_composite(grad)
        for sh in shapes:
            d.polygon([(int(x), int(y)) for x, y in sh], outline=OUTLINE, width=ow)
    if highlight:
        d.line([(70 * S, 58 * S), (146 * S, 50 * S)], fill=(255, 255, 255, 110), width=int(3 * S))


def band_polys():
    """喀秋莎荷叶边发带的几何（已乘 S，画布像素）。accessories 画它、hair_geo_mask 减它。
    参考图的喀秋莎是宽大的一片荷叶边裙边，不是一根细头箍：
    外缘 87×60、内缘 73×46，厚度 14 逻辑（屏上 9px）。上一版只有 6 厚，缩下去就是一圈珠子。
    外缘比头还宽（横向盖过头顶两侧），上一版 87×60 只到头顶内侧。
    """
    rx, ry, cy = 88, 44, 52
    n = 240
    outer, inner = [], []
    for i in range(n + 1):
        t = i / float(n)
        # 165~375：两侧各垂到太阳穴（y≈70），不是只盖头顶变成一顶小皇冠
        th = math.radians(165 + 210 * t)
        # 荷叶瓣必须是圆的：(1-cos)/2 的凸起，瓣顶和瓣谷都是圆角，
        # 上一版用 |sin| 画出来是一圈尖齿，缩小到屏上直接变荆棘王冠。
        # 9 瓣 + 凸起 7：参考图的瓣又大又少，13 瓣缩到屏上就是一圈齿
        wob = (2.0 + 9.0 * (0.5 - 0.5 * math.cos(2 * math.pi * 7 * t))) * math.sin(math.pi * t) ** 0.35
        outer.append(((120 + (rx + wob) * math.cos(th)) * S,
                      (cy + (ry + wob) * math.sin(th)) * S))
        # 内缘用同心缩进 34（rx-34, ry-34 = 54 x 10）：等宽度的带子，
        # 缩 24 时只有 16px 读成白头箍，把中心下移 39 又变成一整块白帽子。
        # 34 是两者之间唯一能保持「一条宽荷叶边」的值
        inner.append(((120 + (rx - 34 + wob * 0.5) * math.cos(th)) * S,
                      (cy + (ry - 34 + wob * 0.5) * math.sin(th)) * S))
    return outer + inner[::-1]


def accessories(d):
    """荷叶边发带 + 蝴蝶结。
    刻意不写进 hair_front：hair_masks 把 hair_front 的每一笔重放成"头发几何"，
    白带子会命中"亮白"那条判据被吞进遮罩，用户换发色时整条发带跟着变色。"""
    ol, ow = OUTLINE, int(OUTLINE_W * S)
    poly(d, band_polys(), BAND, ol, ow)
    # 蝴蝶结：压在右太阳穴上方。参考图是两只圆瓣 + 中间一个小结，
    # 用三角形画出来会读成两片叶子，所以瓣也是椭圆
    # 蝴蝶结只有约 30 逻辑宽（参考图上它占脸宽的 1/4），上一版 68 宽是它的两倍多
    bx, by = 202, 58
    el(d, ((bx - 15) * S, (by - 9) * S, (bx - 2) * S, (by + 9) * S), fill=RIBBON, outline=ol, width=ow)
    el(d, ((bx + 2) * S, (by - 9) * S, (bx + 15) * S, (by + 9) * S), fill=RIBBON, outline=ol, width=ow)
    d.line([((bx - 14) * S, (by - 4) * S), ((bx - 5) * S, (by - 1) * S)], fill=RIBBON_DK, width=int(2.0 * S))
    d.line([((bx + 14) * S, (by - 4) * S), ((bx + 5) * S, (by - 1) * S)], fill=RIBBON_DK, width=int(2.0 * S))
    el(d, ((bx - 6) * S, (by - 6) * S, (bx + 6) * S, (by + 6) * S), fill=RIBBON_DK, outline=ol, width=ow)
    # 飘带只留很短的两片：给到 17 逻辑时它们从瓣后跟出来，屏上读成一对翅膀
    poly(d, [((bx - 6) * S, (by + 5) * S), ((bx - 10) * S, (by + 13) * S), ((bx - 1) * S, (by + 12) * S)],
         RIBBON, ol, ow)
    poly(d, [((bx + 6) * S, (by + 5) * S), ((bx + 11) * S, (by + 12) * S), ((bx + 2) * S, (by + 12) * S)],
         RIBBON_DK, ol, ow)


def ears(d):
    """鳍耳：脸两侧的鱼鳍，比头发深一档，读成「另一种材质」。
    必须「横向长、竖向短」：参考图里耳朵是 x 23~69 / y 106~128 的扁片，
    上一版 30x42 的竖三角在屏上读成猫耳。尖端三个鳍条缺口是识别点，不能省"""
    ol, ow = OUTLINE, int(OUTLINE_W * S)
    for sx, dirn in ((58, -1), (182, 1)):
        # 一片向外下垂的鳍：前缘一条平滑斜边，后缘三个分叉。
        # 上一版写成 3 个外尖 + 3 个内凹交替，降采样后只剩一条灰色横杠
        pts = [(sx * S, 118 * S),
               ((sx + dirn * 32) * S, 124 * S),
               ((sx + dirn * 18) * S, 131 * S),
               ((sx + dirn * 40) * S, 138 * S),
               ((sx + dirn * 18) * S, 145 * S),
               ((sx + dirn * 28) * S, 152 * S),
               (sx * S, 148 * S)]
        poly(d, pts, FIN, ol, ow)
        for ty, tx in ((126, 24), (138, 27), (149, 18)):
            d.line([((sx + dirn * 4) * S, 145 * S), ((sx + dirn * tx) * S, ty * S)],
                   fill=FIN_DK, width=int(2.2 * S))


# 换发色遮罩的排除区（逻辑坐标）。剩蝴蝶结：它压在头顶发区上、颜色和头发一样蓝，
# 不挖掉就会跟着用户选的颜色一起变。脸、喀秋莎、鳍耳都不再靠矩形排除——
# 手写矩形是硬边，挖在头发上染色后就是一块原色方块（上一版两侧脸颊旁的蓝块）。
# 眼区另用椭圆排除，见 hair_masks。
# 框的位置只能按底图实测，不能照矢量：accessories() 里蝴蝶结画在 bx=202，那是画给
# 生成图看的位置，从参考立绘裁出来的底板整体偏左下约 40 逻辑单位。旧值
# (150,60,183,85) 就是照矢量写的，落在底图上正好压住蝴蝶结左上方的真头发——
# 换发色后那里留一块原蓝方块（实测覆盖率 0%，用户报的「蝴蝶结旁边缺一块」）。
# 现在是按底板亮蓝像素实测的框：蝴蝶结亮蓝像素误染 0，被误挖的头发恢复 99 像素。
HAIR_EXCL = (
    (158, 69, 188, 92),      # 蝴蝶结（底板实测框，含描边）
)

# 判据：头发是底板里唯一成片的蓝。但「蓝」不是充分条件——虹膜、蝴蝶结、鳍耳同样蓝，
# 脸和喀秋莎是近白。旧版按颜色逐像素判，把整张脸和身体都判成头发，
# 用户一改发色就是「整只人偶变红」；发丝高光那笔亮白又被第二条判据吃进来。
# 现在：蓝判据 → 开运算切断发区与鳍耳之间那截细桥 → 取最大连通块 → 在宽松判据里
# 条件膨胀长回（呆毛/鳍耳/发梢都在这一步回来）→ 补发丝白缝。
# 实测漏染：只取最大块 25.3% → 条件膨胀后 6.8%（剩下的是蝴蝶结和眼睛，故意排除的）。
HAIR_BR_MIN = 45     # b - r 下限
HAIR_B_MIN = 90      # b 下限
# 重建用的宽松判据：发丝暗部、鳍耳（b-r 中位 58）都落在这条线以上。
# 用它当「allowed」而不用它直接当遮罩——直接当遮罩会连 rim 和虹膜一起吃进来。
HAIR_LOOSE_BR = 16
HAIR_LOOSE_B = 70
HAIR_BRIGHT_B = 160   # 发束外缘亮边：b 与 b-r 同时高，尾巴达不到
HAIR_BRIGHT_BR = 80
# 发丝高光那笔白与发带的白在色值上不可分（r 中位 247 vs 248、b-r 中位 -3 vs -5，
# 实测两组直方图几乎重合），而矢量几何跟手绘底图又对不齐，任何“把高光也拉进遮罩”
# 的写法都会把整圈喀秋莎一起染成粉。高光就留给原图。
HAIR_OPEN = 3        # 开运算核（腐蚀→最大块），切断细桥
HAIR_HOLE_MAX = 900  # 只补发丝缝这种细洞；喀秋莎白块邻接占比过不了下面的门槛
HAIR_HOLE_SPAN = 22  # 洞的 bbox 跨度上限：再大就是脸/发带空腔，不是发丝缝
HAIR_HOLE_FILL = 0.45  # 洞的填充率上限：细缝是长条，圆胖块是脸
HAIR_HOLE_TOUCH = 0.7


# ---- 角色参数（None = 内置的大肥鱼，保持原有全部行为） -----------------
# 这套生成器原本只为单个角色写死：坐标、瞳色、发色判据全是模块级常量。
# 参数化走「覆盖全局」，不动 21 个表情函数的调用点——它们传的 88/152 是
# 左右眼的逻辑编号，不是真实坐标，由 _eye_pair 负责映射到当前角色。
_CAST = None
# 角色脸的管线开关：True = 代码画五官（和大肥鱼同一套矢量画法）；
# False = 抠原画五官做差分。抠图路线在 162px 输出上表现很差：原画眼球在立绘里
# 只有二十来像素，缩到屏上虹膜糊成一坨蓝斑、睫毛和高光全糊没。代码五官自带
# 眼白/高光/睫毛，同尺寸下是有神的大眼。需要对比时把这里改回 False 即可。
CAST_CODE_FACE = False

# 五官风格参数：大肥鱼是女性角色，圆眼、虹膜顶满、外眼角带翘睫毛、腮红重；
# 另外五个是男性角色，照搬那套五官等于「女孩子换了个头发」，用户原话是「眼神
# 应该有区别」。只给参数、不复制第二份画法——差异都在下面这些比例上。
_FACE_BASE = {
    "ry_mul": 1.0,        # 眼型纵向缩放：<1 更细长
    "iris_w": 0.80,       # 虹膜横占眼白（留 look 平移余量）
    "iris_h": 0.96,       # 虹膜纵占眼白
    "lash_w": 3.0,        # 上眼睑线宽
    "lash_tip": True,     # 外眼角那振翘睫毛，萌系标志
    "lid_w": 3.4,         # 闭眼线宽
    "brow_w": 2.8,        # 眉毛线宽
    "brow_len": 6.6,      # 眉毛半宽
    "brow_flat": 1.0,     # 眉毛弧度：<1 更平（男性平眉）
    "blush": 1.0,         # 腮红强度乘数
    "pupil": 1.0,         # 瞳孔大小乘数
    "highlight": 1.0,     # 高光大小乘数
}
FACE = dict(_FACE_BASE)
_CAST_HAIR = None
_CAST_HAIR_LOOSE = None
_CAST_HAIR_BRIGHT = None
# 以下三项由 apply_cast 覆盖；默认值 = 大肥鱼的参考底板
BLUSH_CX = (67.2, 127.2)
BLUSH_Y = (125.2, 135.0)
TEAR_Y = 125.2


def apply_cast(cfg):
    """把一个角色的参数注入全局。cfg=None 回到内置的大肥鱼。

    缓存必须一起清：head_image/body 按模块级字典缓存，不清就会把上一个角色的
    脸贴到新角色的表情上——切角色时表现为「换了个名字但人没变」。
    """
    global _CAST, _CAST_HAIR, _CAST_HAIR_LOOSE, _CAST_HAIR_BRIGHT
    global BASE_PNG, HEAD_CROP, HEAD_K, BODY_CROP, BODY_AXIS, BODY_OX, BODY_OY
    global EYE_CX, EYE_CY, IRIS_TOP, IRIS_BOT, LINE, SKIN, SKIN_SH
    global MOUTH, TONGUE, BLUSH, OUTLINE
    global MOUTH_C, MOUTH_S, MOUTH_BOX, MOUTH_W, MOUTH_H, MOUTH_OX, MOUTH_OY
    global BLUSH_CX, BLUSH_Y, TEAR_Y, HAIR_EXCL, HEAD_OX, HEAD_OY
    global FACE
    global _HEAD_CACHE, _BODY_CACHE

    _CAST = cfg
    # 五官风格跟着角色走：cfg 没给 face 就回到大肥鱼那套基准值，
    # 未传 face 的角色出图像素与改前完全一致。
    FACE = dict(_FACE_BASE, **(cfg.get("face") or {})) if cfg else dict(_FACE_BASE)
    _HEAD_CACHE = {}
    _BODY_CACHE = {}
    _SRC_CACHE.clear()
    _DIFF_CACHE.clear()
    if cfg is None:
        _CAST_HAIR = _CAST_HAIR_LOOSE = _CAST_HAIR_BRIGHT = None
        BASE_PNG = _DEFAULT_BASE_PNG
        HEAD_CROP = _DEFAULT_HEAD_CROP
        HEAD_K = _DEFAULT_HEAD_K
        BODY_CROP = _DEFAULT_BODY_CROP
        BODY_OX = _DEFAULT_BODY_OX
        BODY_OY = _DEFAULT_BODY_OY
        EYE_CX = _DEFAULT_EYE_CX
        EYE_CY = _DEFAULT_EYE_CY
        HAIR_EXCL = _DEFAULT_HAIR_EXCL
        MOUTH_C = _DEFAULT_MOUTH_C
        MOUTH_S = _DEFAULT_MOUTH_S
        MOUTH_BOX = _DEFAULT_MOUTH_BOX
        MOUTH_W = _DEFAULT_MOUTH_W
        MOUTH_H = _DEFAULT_MOUTH_H
        MOUTH_OX = _DEFAULT_MOUTH_OX
        MOUTH_OY = _DEFAULT_MOUTH_OY
        HEAD_OX = _DEFAULT_HEAD_OX
        HEAD_OY = _DEFAULT_HEAD_OY
        BLUSH_CX = (67.2, 127.2)
        BLUSH_Y = (125.2, 135.0)
        TEAR_Y = 125.2
        for name, val in _DEFAULT_COLORS.items():
            globals()[name] = val
        return

    c = cfg["colors"]
    # 底板用原立绘，不用 *_base.png：那张是「五官填平版」，抹过的地方会留下淡眼影，
    # open/keep 这类本该等于原图的表情就会露出两道灰影子（星野、墨言最明显）。
    # 现在只在真要换姿势时才局部抹（_erase_eye_on / _erase_mouth_on），没碰的像素就是原画。
    BASE_PNG = cfg["head_src"].replace("_base.png", ".png")
    HEAD_CROP = cfg["head_crop"]
    # head_k 优先：它决定「整个头在画布上多大」，不是算出来的
    HEAD_K = cfg.get("head_k") or (240.0 / float(HEAD_CROP[2] - HEAD_CROP[0]))
    HEAD_OX = cfg.get("head_ox", 0.0)
    HEAD_OY = cfg.get("head_oy", 0.0)
    BODY_CROP = cfg["body_crop"]
    # 身体上缘抬高几像素（源图坐标按 k 换算），让“横切断面”藏到下巴底下：
    # 下巴那一行如果正好横切过道具的浅色描边（紫苑的调色板就是），切线就是一条
    # 亮线横在脖子上。多留的几行是同一张脸的下巴肉/道具，被脸层盖住看不见。
    _ov = int(round(BODY_JOIN_OVERLAP / HEAD_K))
    BODY_CROP = (BODY_CROP[0], max(0, BODY_CROP[1] - _ov), BODY_CROP[2], BODY_CROP[3])
    BODY_AXIS = cfg.get("body_axis")
    BODY_OX, BODY_OY = (OUT - BODY_W) // 2, _body_oy()
    EYE_CX = (cfg["eye_l"][0], cfg["eye_r"][0])
    EYE_CY = (cfg["eye_l"][1] + cfg["eye_r"][1]) / 2.0
    HAIR_EXCL = tuple(cfg.get("hair_excl", ()))
    SKIN = c["skin"]
    SKIN_SH = c["skin_sh"]
    LINE = c["line"]
    IRIS_TOP = c["iris_top"]
    IRIS_BOT = c["iris_bot"]
    MOUTH = c["mouth"]
    TONGUE = c["tongue"]
    BLUSH = c["blush"]
    OUTLINE = c.get("outline", OUTLINE)
    mcx, mcy, mw = cfg["mouth"]
    # _m() 把旧基准（脸中心 x=120 / 嘴线 y=156）线性映射到真实嘴位。
    # mw = smile 嘴的目标逻辑宽（smile 跨 108..132 = 24 旧基准单位）
    MOUTH_C = (mcx, mcy)
    MOUTH_S = mw / 24.0
    # 嘴框跟着嘴宽派生：最宽的是 grin（105..135 = 30 单位 = 1.25×mw）、
    # 最高的是 o（151..167 = 16 单位）。半宽不够就会把嘴角和描边裁掉，
    # 屏上表现为“笑的时候嘴缺一个角”。
    MOUTH_BOX = (mcx - 0.75 * mw, mcy - 0.55 * mw, mcx + 0.75 * mw, mcy + 0.62 * mw)
    # 这四个是 MOUTH_BOX 的派生量，模块加载时按大肥鱼的框算过一次。
    # 不跟着重算的话，每个角色都套大肥鱼的嘴框——灼的嘴就贴到腮上去了
    MOUTH_W = sc(MOUTH_BOX[2] - MOUTH_BOX[0])
    MOUTH_H = sc(MOUTH_BOX[3] - MOUTH_BOX[1])
    MOUTH_OX = sc(MOUTH_BOX[0])
    MOUTH_OY = sc(MOUTH_BOX[1])
    el_cx, el_cy = cfg["eye_l"][0], cfg["eye_l"][1]
    er_cx, er_cy = cfg["eye_r"][0], cfg["eye_r"][1]
    # 腮红位置不照抄原画框：立绘的腮红框从「只盖住左脸」到「横跨整张脸」都有，
    # 换成椭圆后一边挤在脸中央压在嘴上、一边跑到脸外，五个角色三套长相。
    # 按眼睛定：贴在外眼角下方一点，左右严格对称，尺寸跟着眼睛走。
    # cfg["blush_box"] 仍保留，但只当底板清旧腮红时的搜索框用。
    half_gap = (er_cx - el_cx) / 2.0
    rx_avg2 = (cfg["eye_l"][2] + cfg["eye_r"][2]) / 2.0
    # 腮红要落在眼睛外缘之外。只跟半眼距走（+0.55rx）时椭圆整个压在眼睛上，
    # 屏上就是两块粉胶囊贴在瞳孔旁，闭眼时最扎眼——横向按「眼外缘 + 半个腮红」推。
    bx = half_gap + rx_avg2 * 1.45
    mid = 240.0 / 2.0
    BLUSH_CX = (mid - bx, mid + bx)
    # 纵向不按嘴反推：眼中心到嘴只剩 11 逻辑，腮红挤在中间会叠到眼白上。
    # 改用眼睛自己的相对位置（和大肥鱼一样：眼下 7.5~17）
    BLUSH_Y = (el_cy + 7.5, el_cy + 17.0)
    TEAR_Y = el_cy
    _CAST_HAIR = cfg.get("hair")
    _CAST_HAIR_LOOSE = cfg.get("hair_loose") or cfg.get("hair")
    _CAST_HAIR_BRIGHT = cfg.get("hair_bright")


def _eye_pair(cx):
    """调用点写死的 cx（88=左 / 152=右）→ 当前角色的该只眼参数。"""
    if _CAST is None:
        return None
    return _CAST["eye_l"] if cx < 120 else _CAST["eye_r"]


def _hair_blue(p):
    r, g, b, a = p
    if a <= 40:
        return False
    if _CAST_HAIR is not None:
        return _CAST_HAIR(r, g, b)
    return b - r >= HAIR_BR_MIN and b >= HAIR_B_MIN


def _hair_loose(p):
    r, g, b, a = p
    if a <= 40:
        return False
    if _CAST_HAIR_LOOSE is not None:
        return _CAST_HAIR_LOOSE(r, g, b)
    return b - r >= HAIR_LOOSE_BR and b >= HAIR_LOOSE_B


def _hair_bright(p):
    """发束外缘那道亮边。它紧贴尾巴内缘、又在几何允许带外，原来整条漏染，
    换发色后发束右缘留一条原蓝（用户报的「鱼尾那边一块没变色」）。
    它 b>=160/b-r>=80，尾巴本体 b~120/b-r~54，两者在这条线上分得开——
    所以只拿它当 allowed（不当 seed），重建能沿亮边长出去，进不了尾巴内部。"""
    r, g, b, a = p
    if a <= 40:
        return False
    if _CAST_HAIR_BRIGHT is not None:
        return _CAST_HAIR_BRIGHT(r, g, b)
    return b >= HAIR_BRIGHT_B and b - r >= HAIR_BRIGHT_BR


def _close_gaps(mask):
    """条件膨胀的允许区做一次闭运算（3x3 膨胀后腐蚀）。
    发丝之间的深紫描边不满足任何颜色判据，1~2px 宽就能把外圈的发梢、鳍耳、
    散落的发束跟主体切断——重建跨不过去，那些小块就一直留原蓝。
    闭运算把这条细缝接上：只连 ≤2px 的断口，大块的空腔（脸、发带）宽度远超核，
    腐蚀阶段会原样退回去。核再放大到 5x5 实测一个像素都不多连，就停在这里。"""
    return mask.filter(ImageFilter.MaxFilter(3)).filter(ImageFilter.MinFilter(3))


def _reconstruct(seed_p, allowed_p, w, h):
    """seed 在 allowed 里做 8 连通条件膨胀（geodesic reconstruction）。
    拓扑上够不着的区域不会进来：rim 被 b-r≈6 的深紫描边隔开、虹膜被眼白隔开。
    """
    out = Image.new("L", (w, h), 0)
    op = out.load()
    stack = []
    for y in range(h):
        for x in range(w):
            if seed_p[x, y]:
                op[x, y] = 255
                stack.append((x, y))
    while stack:
        cx, cy = stack.pop()
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                nx, ny = cx + dx, cy + dy
                if 0 <= nx < w and 0 <= ny < h and not op[nx, ny] and allowed_p[nx, ny]:
                    op[nx, ny] = 255
                    stack.append((nx, ny))
    return out


def _largest_block(mp, w, h):
    """8 连通，返回像素坐标列表（头发主体）。"""
    seen = [[False] * w for _ in range(h)]
    best = []
    for y in range(h):
        for x in range(w):
            if seen[y][x] or not mp[x, y]:
                continue
            stack = [(x, y)]
            seen[y][x] = True
            cur = []
            while stack:
                cx, cy = stack.pop()
                cur.append((cx, cy))
                for dy in (-1, 0, 1):
                    for dx in (-1, 0, 1):
                        nx, ny = cx + dx, cy + dy
                        if 0 <= nx < w and 0 <= ny < h and not seen[ny][nx] and mp[nx, ny]:
                            seen[ny][nx] = True
                            stack.append((nx, ny))
            if len(cur) > len(best):
                best = cur
    return best


def _fill_hair_holes(mask):
    """发丝高光那笔亮白叠在蓝底上不判蓝，会在遮罩里留一道白缝，染色后是条原色线。
    只补「四周基本被头发包住」的细缝。脸不是细缝——遮罩吃大后它会被彻底围住，
    旧版只卡面积（<900px）就把它放进来当洞补上，染色后整颗头变绿。
    """
    w, h = mask.size
    mp = mask.load()
    inv = mask.point(lambda v: 0 if v else 255)
    ip = inv.load()
    out = mask.copy()
    op = out.load()
    seen = [[False] * w for _ in range(h)]
    for y in range(h):
        for x in range(w):
            if seen[y][x] or not ip[x, y]:
                continue
            stack = [(x, y)]
            seen[y][x] = True
            cur = []
            edge = False
            while stack:
                cx, cy = stack.pop()
                cur.append((cx, cy))
                if cx in (0, w - 1) or cy in (0, h - 1):
                    edge = True
                for dy in (-1, 0, 1):
                    for dx in (-1, 0, 1):
                        nx, ny = cx + dx, cy + dy
                        if 0 <= nx < w and 0 <= ny < h and not seen[ny][nx] and ip[nx, ny]:
                            seen[ny][nx] = True
                            stack.append((nx, ny))
            if edge or len(cur) > HAIR_HOLE_MAX:
                continue
            xs = [p[0] for p in cur]
            ys = [p[1] for p in cur]
            bw, bh = max(xs) - min(xs) + 1, max(ys) - min(ys) + 1
            # 细缝的填充率必然低；圆胖块是脸/发带被围出的空腔
            if bw > HAIR_HOLE_SPAN or bh > HAIR_HOLE_SPAN:
                continue
            if len(cur) > HAIR_HOLE_FILL * bw * bh:
                continue
            touch = total = 0
            for cx, cy in cur:
                for dy in (-1, 0, 1):
                    for dx in (-1, 0, 1):
                        nx, ny = cx + dx, cy + dy
                        if (dx or dy) and 0 <= nx < w and 0 <= ny < h:
                            total += 1
                            if mp[nx, ny]:
                                touch += 1
            if total and touch / float(total) >= HAIR_HOLE_TOUCH:
                for cx, cy in cur:
                    op[cx, cy] = 255
    return out


def _clip_to_alpha(mask, img):
    """遮罩最终必须服从底图 alpha：补洞会把发丝之间的透明缝隙一起补成实色，
    染色后那块透明区就成了一块纯色板（整束头发看着像色块，不再是一缕一缕）。
    顺带把遮罩值压到原 alpha 以内，发丝边缘才有渐变，不会是硬边。"""
    w, h = mask.size
    px, mp = img.load(), mask.load()
    out = mask.copy()
    op = out.load()
    for y in range(h):
        for x in range(w):
            if not mp[x, y]:
                continue
            a = px[x, y][3]
            if a < 40:
                op[x, y] = 0
            elif a < 255:
                op[x, y] = min(mp[x, y], a)
    return out


def hair_masks(items):
    """换发色遮罩：蓝判据 → 挖蝴蝶结 → 开运算断细桥 → 最大连通块 → 补发丝白缝。"""
    ex = Image.new("L", (OUT, OUT), 0)
    ed = ImageDraw.Draw(ex)
    for (x0, y0, x1, y1) in HAIR_EXCL:
        ed.rectangle([sc(x0), sc(y0), sc(x1), sc(y1)], fill=255)
    # 眼区椭圆排除。虹膜本身是蓝的，降采样后左眼外侧跟垂下来的发丝粘成一团，
    # 最大连通块顺着这点把整颗虹膜吃进遮罩，染色后眼睛跟着变色。
    # 用椭圆而不是矩形：矩形会连眼睛上方的刘海一起挖掉，染色后是一条原色横线。
    for ecx in EYE_CX:
        ed.ellipse([sc(ecx - 15.2), sc(EYE_CY - 12.4), sc(ecx + 15.2), sc(EYE_CY + 12.4)], fill=255)
    ep = ex.load()

    masks = {}
    for name, im in items.items():
        px = im.load()
        blue = Image.new("L", (OUT, OUT), 0)
        bp = blue.load()
        for y in range(OUT):
            for x in range(OUT):
                if ep[x, y] or not _hair_blue(px[x, y]):
                    continue
                bp[x, y] = 255
        eroded = blue.filter(ImageFilter.MinFilter(HAIR_OPEN))
        keep = Image.new("L", (OUT, OUT), 0)
        kp = keep.load()
        for cx, cy in _largest_block(eroded.load(), OUT, OUT):
            kp[cx, cy] = 255
        # 旧版到这里是「最大块膨胀回原蓝区」：只认颜色不认连通性，
        # 呆毛、鳍耳、发梢这些不成片的部分整块丢掉，实测漏 25.3% 的蓝像素。
        # 换成条件膨胀后，跟头发连着的都长回来，隔开的（rim/虹膜）依旧进不来。
        loose = Image.new("L", (OUT, OUT), 0)
        lop = loose.load()
        for y in range(OUT):
            for x in range(OUT):
                if ep[x, y]:
                    continue
                if _hair_loose(px[x, y]):
                    lop[x, y] = 255
        masks[name] = _clip_to_alpha(
            _fill_hair_holes(_reconstruct(kp, _close_gaps(loose).load(), OUT, OUT)), im)
    return masks



def hair_shade(items, masks, name=None):
    """染色层只有「新色」一个自由度：纯色 recolor 会把底板里的发丝明暗一起吃平成塑料色。
    A8 阴影层把比头发中位亮度暗的部分压暗，色相仍取新色 → 亮部纯新色、暗部深新色。
    头发在底板里、五官补丁不碰发区，所以全表情共用一张。
    """
    if name is None:
        name = "neutral" if "neutral" in items else next(iter(items))
    img, mask = items[name], masks[name]
    lum = img.convert("L")
    lp, mk = lum.load(), mask.load()
    vals = sorted(lp[x, y] for y in range(OUT) for x in range(OUT) if mk[x, y])
    mid = vals[len(vals) // 2] if vals else 1
    mid = mid or 1
    out = Image.new("L", (OUT, OUT), 0)
    op = out.load()
    for y in range(OUT):
        for x in range(OUT):
            if not mk[x, y]:
                continue
            d = (mid - lp[x, y]) / float(mid)
            if d > 0:
                op[x, y] = int(min(1.0, d * 2.4) ** 0.85 * 210)
    return out






# 身体图里的下半截长发：脸图只有 162 高，垂到肩/胸以下的发丝画在身体图里，
# 身体原来没有遮罩，换发色就是「上半截变、下半截还是原蓝」。
# 身体是参考图裁出来的静态图，蓝判据会把尾巴整条吃进来（实测尾巴 (66,74,120) b-r=54，
# 头发 (45,73,116) b-r=71，颜色区间完全重叠，靠阈值分不开），所以左右各给一条允许带：
# 尾巴在带外被整条排除，裙摆/围裙上的零星命中也不在带内。
BODY_HAIR_REGIONS = (
    # 左侧垂发：外沿贴到画布左边，内沿到 x≈46
    ((0, 0), (47, 0), (45, 10), (43, 24), (41, 38), (37, 48), (31, 57), (20, 60), (7, 57), (0, 50)),
    # 右侧垂发：右边界卡在 127，尾巴尾鳍左沿在 128
    ((86, 0), (129, 0), (128, 8), (127, 20), (126, 32), (124, 43), (120, 52), (113, 58),
     (104, 56), (97, 47), (92, 32), (88, 14)),
)


def _poly_mask(poly, w, h):
    m = Image.new("L", (w, h), 0)
    ImageDraw.Draw(m).polygon(poly, fill=255)
    return m


def body_hair_mask(body_img):
    """和 hair_masks 同一套判据，只是画布换成身体图，并且每条发带独立取最大块：
    左右两束在图上不连通（中间隔着裙子），全图取最大块只会留下一边。
    """
    w, h = body_img.size
    px = body_img.load()
    out = Image.new("L", (w, h), 0)
    op = out.load()
    for poly in BODY_HAIR_REGIONS:
        ap = _poly_mask(poly, w, h).load()
        seed = Image.new("L", (w, h), 0)
        sp = seed.load()
        loose = Image.new("L", (w, h), 0)
        lp = loose.load()
        for y in range(h):
            for x in range(w):
                p = px[x, y]
                if ap[x, y]:
                    if _hair_blue(p):
                        sp[x, y] = 255
                    if _hair_loose(p):
                        lp[x, y] = 255
                elif _hair_bright(p):
                    lp[x, y] = 255
        keep = Image.new("L", (w, h), 0)
        kp = keep.load()
        for cx, cy in _largest_block(sp, w, h):
            kp[cx, cy] = 255
        rp = _reconstruct(kp, _close_gaps(loose).load(), w, h).load()
        for y in range(h):
            for x in range(w):
                if rp[x, y]:
                    op[x, y] = 255
    return _clip_to_alpha(_fill_hair_holes(out), body_img)


def body_hair_shade(body_img, mask):
    """和 hair_shade 同一个思路，画布是身体图。不压暗的话下半截是一块平涂色，
    和上半截的发丝层次一刀两断。"""
    w, h = body_img.size
    lum = body_img.convert("L")
    lp, mk = lum.load(), mask.load()
    vals = sorted(lp[x, y] for y in range(h) for x in range(w) if mk[x, y])
    if not vals:
        return Image.new("L", (w, h), 0)
    mid = vals[len(vals) // 2] or 1
    out = Image.new("L", (w, h), 0)
    op = out.load()
    for y in range(h):
        for x in range(w):
            if not mk[x, y]:
                continue
            d = (mid - lp[x, y]) / float(mid)
            if d > 0:
                op[x, y] = int(min(1.0, d * 2.4) ** 0.85 * 210)
    return out


# 参考图的脸是「扁宽荷包蛋」：脸宽只占头宽的 0.46，四周全是泳松卷发。
# 上一版脸 132 宽 / 头 192 宽（0.69），头发只剩两条窄边，读成“一张脸贴在帘子上”。
# 现在脸 116（占 0.58）：比参考图留大一点，因为屏上只有 162px，
# 脸再小眉毛和嘴就糊成像素点了
FACE_CX, FACE_CY = 120, 118
# 实测参考图：脸宽 380 / 脸高 370（1.03）、眼睛宽 200（占脸宽 0.53）。
# 上一版脸 116×88（1.32）——脸扁了 30%，眼睛只占 0.34，这就是“不像”的根。
# 脸顶必须提到 69（118-49）：额头在参考图里是可见的一大块，不是一条缝
FACE_RX, FACE_RYU = 56, 49
FACE_BOT = 168


def _face_silhouette(n=36, m=26):
    """脸型 = 上半椭圆 + 下半三次贝塞尔。单个椭圆是颗蛋：下巴钝在 206、脸高比脸宽还长，
    读完像少女不像萝莉。二次元萝莉脸的三个特征是「最宽点靠上、下颊继续撑住（婴儿肥）、
    最后一段才收成小圆下巴」，只能靠下半段单独给曲线——控制点让 x 先撑到 168 才sag，
    末段切线接近水平，下巴才是圆的不是锥的。"""
    pts = []
    for i in range(n + 1):                       # 上半：左太阳穴 → 头顶 → 右太阳穴
        th = math.pi - math.pi * i / n
        pts.append((FACE_CX + FACE_RX * math.cos(th),
                    FACE_CY - FACE_RYU * math.sin(th)))
    p0 = (FACE_CX + FACE_RX, FACE_CY)
    p1 = (FACE_CX + FACE_RX + 3, 148)
    p2 = (FACE_CX + FACE_RX * 0.76, 160)
    p3 = (FACE_CX, FACE_BOT)
    right = []
    for i in range(1, m + 1):
        t = i / float(m)
        mt = 1.0 - t
        right.append((mt ** 3 * p0[0] + 3 * mt * mt * t * p1[0] + 3 * mt * t * t * p2[0] + t ** 3 * p3[0],
                      mt ** 3 * p0[1] + 3 * mt * mt * t * p1[1] + 3 * mt * t * t * p2[1] + t ** 3 * p3[1]))
    pts.extend(right)                            # 颊 → 下巴
    pts.extend([(2 * FACE_CX - x, y) for x, y in right[::-1]])   # 下巴 → 左颊（镜像上行）
    return pts


def face(d, outline=True):
    poly(d, [(x * S, y * S) for x, y in _face_silhouette()], SKIN,
         OUTLINE if outline else None, int(OUTLINE_W * S))


def _brow_dy():
    """眉毛整体相对眼睛的偏移量。

    眉毛 y 原本是写死的（100.7/103.0 等），因为只有大肥鱼一个角色、眼睛就在 117.6。
    换角色后 eye_cy 跑到 104~110，写死的值直接把眉毛画到刘海上（book 最明显：
    黑色鬁发上浮着两道白横线）。统一按「大肥鱼眼顶 106.8」做基准平移。
    """
    if _CAST is None:
        return 0.0
    p = _CAST["eye_l"]
    return (p[1] - p[3]) - 106.8


def brows(d, style="normal"):
    # 角色立绘里眉毛本来就被鬁发遮住时（hoop/book 的大鬁发盖到眼下），
    # 强行画就会在头发上浮出两道线——不如不画，原画就没露
    if _CAST is not None and not _CAST.get("brow", True):
        return
    # 眉毛在参考图里是被鬁发半盖着的短弧，y 112~120 才露在发际下方。
    # 跟着眼睛下移后不能再往下一分：眼顶在 120.2，压上去就被眼白盖掉
    dy = _brow_dy()
    bw = FACE["brow_len"]
    bwid = int(FACE["brow_w"] * S)
    for i, cx in enumerate(EYE_CX):
        inner = i == 1
        if style == "angry":
            y1 = (100.7 if inner else 103.0) + dy
            y2 = (103.0 if inner else 100.7) + dy
            d.line([((cx - bw) * S, y1 * S), ((cx + bw) * S, y2 * S)], fill=LINE, width=bwid)
        elif style == "sad":
            y1 = (103.0 if inner else 100.7) + dy
            y2 = (100.7 if inner else 103.0) + dy
            d.line([((cx - bw) * S, y1 * S), ((cx + bw) * S, y2 * S)], fill=LINE, width=bwid)
        elif style == "listen":
            d.arc([(cx - bw) * S, (94.1 + dy) * S, (cx + bw) * S, (103.9 + dy) * S], 192, 348,
                  fill=LINE, width=bwid)
        elif style == "raised":
            d.arc([(cx - bw) * S, (92.4 + dy) * S, (cx + bw) * S, (102.1 + dy) * S], 195, 345,
                  fill=LINE, width=bwid)
        else:
            # brow_flat 把弧高往下压：女性弧度弯、男性一字眉，只差这一个系数
            fh = 8.9 * FACE["brow_flat"]
            d.arc([(cx - bw) * S, (105.7 + dy - fh) * S, (cx + bw) * S, (105.7 + dy) * S], 212, 328,
                  fill=LINE, width=bwid)


BLINK_MODE = False  # True 时所有睁眼改画闭眼，用来出眨眼贴片
BLINK_STYLE = "flat"


EYE_CX = (74.4, 118.1)   # 从参考底板实测：原图眼心 310/492 × k=0.24
EYE_CY = 117.6


def _eye_cx(cx):
    """眼距归一。21 张脸里的 cx 一律写死 88/152（内距 4 逻辑），缩到屏上两眼粘成一团；
    逐个调用点改容易漏一个，直接在入口掰到角色的左右眼位。"""
    if _CAST is not None:
        return _eye_pair(cx)[0]
    return EYE_CX[0] if cx < 120 else EYE_CX[1]


def _el_span(cx, cy, rx, ry, y):
    """椭圆在第 y 行的 x 跨度（像素坐标）。rx/ry<=0 或该行不落在椭圆内返回 None。"""
    if rx <= 0 or ry <= 0:
        return None
    dy = (y + 0.5 - cy) / float(ry)
    if dy <= -1.0 or dy >= 1.0:
        return None
    hw = rx * math.sqrt(max(0.0, 1.0 - dy * dy))
    return (cx - hw, cx + hw)


def _clip_el(d, cx, cy, rx, ry, clip, fill):
    """画椭圆，但只保留落在 clip 椭圆内的部分（逐行求交集）。

    PIL 没有裁剪区，而虹膜/瞳孔/高光必须服从眼白边界：look 最大 ±11 逻辑
    （silly 抬头看），不裁就把整块虹膜画到刘海和额头上——屏上读成「眼球翻出眼眶」。
    """
    if rx <= 0 or ry <= 0:
        return
    ccx, ccy, crx, cry = clip
    y0 = max(0, int(math.floor(cy - ry)))
    y1 = min(W, int(math.ceil(cy + ry)))
    for y in range(y0, y1 + 1):
        s1 = _el_span(cx, cy, rx, ry, y)
        if s1 is None:
            continue
        s2 = _el_span(ccx, ccy, crx, cry, y)
        if s2 is None:
            continue
        xa = int(math.ceil(max(s1[0], s2[0])))
        xb = int(math.floor(min(s1[1], s2[1])))
        if xb < xa:
            continue
        d.line([(xa, y), (xb, y)], fill=fill)


def eye(d, cx, cy, rx=23, ry=16, lid=1.0, look=(0, 0), heart=False, big=1.0, scale=1.0,
        hl=1.0, full_iris=False):
    pair = _eye_pair(cx)
    # 左右眼由调用点写死的 88/152 判定：cx 下一行就被掰成角色眼位，
    # 拿掰完的值跟 EYE_CX 比，角色左眼坐标落在 88 右边时睫毛会朝内长
    orig_cx = cx
    sgn = -1 if cx < 120 else 1
    cx = _eye_cx(cx)
    # 眼型直接锁死：调用点里散着 rx=29/ry=33、rx=31/ry=36 的旧值，
    # 那是「竖椭圆大眼」，缩到屏上眼白连成镜片。
    if pair is not None:
        # 角色模式：左右眼各用自己实测的半径。3/4 侧脸近大远小，
        # 两只给同一个 rx 会把远侧眼画成超出脸缘的大眼。
        # ry_mul 是性别风格：男性角色压窄眼高，眼睛才不会圆成少女眼
        # scale 只给 diff 管线的 code 眼用：那边画在原画眼上方，必须跟原画尺度而不是跟归一尺度，否则小一圈、露出抹眼后的白斑
        cy, rx, ry = pair[1], pair[2] * big * scale, pair[3] * big * FACE["ry_mul"] * scale
    else:
        cy = EYE_CY
        # 20 是「眼宽占脸宽 0.34」算出来的，跟参考图一致；再大就贴到鬁发上了
        # big 只给惊讲/震惊用：这两个表情用「瞑目」做信号，参考图里眼睛会大一圈
        rx, ry = 13.2 * big, 10.8 * big * FACE["ry_mul"]
    if BLINK_MODE:
        closed_eye(d, orig_cx, cy, BLINK_STYLE)
        return
    cy2 = cy + ry * (1 - lid)
    ry2 = max(ry * lid, 1.0)
    el(d, ((cx - rx) * S, (cy2 - ry2) * S, (cx + rx) * S, (cy2 + ry2) * S), fill=WHITE)
    if lid > 0.4:
        # 虹膜/眼白 = 0.82：参考图虹膜几乎填满眼白，只留一圈窄白。
        # 不能给到 0.9：瞳孔还要靠 look 在眼白里平移，余量小于偏移量就会露出白角
        # 横竖给不同比例是「萌」的关键：原画虹膜是竖椭圆（上下顶到眼睑、左右只留
        # 一线白）。早先横竖同按 0.8 缩，瞳孔就成了正圆或扁圆，屏上像贴了两片圆片，
        # 而大眼 Q 版的辨识点恰好是那个立起来的竖瞳。
        # 横向留 0.20 是 look 的平移余量（最大 ±6 逻辑 ≈ 0.36rx），再满就露白角。
        # 比例本身可调：男性角色虹膜不顶满眼白，读起来才是「看人」不是「睁圆眼」。
        if full_iris:
            # 眼皮压低后眼白变扁，再按 iris_w=0.78 缩虹膜，横向就露出两条宽眼白，
            # 屏上读成「白眼球 + 中间一根竖瞳」。原画那双眼是虹膜横着顶满、上下被
            # 眼睑裁出来，所以这里横向给到超出，多出来的由 clip 裁掉。
            ixr = rx * 1.08
            iyr = ry2 * 1.06
        else:
            ixr = rx * FACE["iris_w"]
            iyr = ry2 * FACE["iris_h"]
        icx, icy = cx + look[0], cy2 + look[1]
        # 眼白椭圆就是虹膜的裁剪框：虹膜/瞳孔/高光一律裁在里面。
        # 只靠缩小虹膜留余量治不住 look——抬头 11 逻辑就把整块虹膜顶到眼白外面。
        clip = (cx * S, cy2 * S, rx * S, ry2 * S)
        steps = 26
        for i in range(steps):
            t = i / (steps - 1.0)
            col = tuple(int(IRIS_TOP[k] + (IRIS_BOT[k] - IRIS_TOP[k]) * t) for k in range(3)) + (255,)
            k = 1 - t
            _clip_el(d, icx * S, icy * S, ixr * k * S, iyr * k * S, clip, col)
        pf = FACE["pupil"]
        # 瞳孔原来是上下不对称的两个半径（0.40/0.52），换算成同心椭圆：
        # 中心偏移 (0.52-0.40)/2，半径取均值
        _clip_el(d, icx * S, (icy + iyr * 0.06 * pf) * S,
                 ixr * 0.50 * pf * S, iyr * 0.46 * pf * S, clip, (26, 16, 42, 255))
        if heart:
            el(d, ((icx - 7.5) * S, (icy - 9.3) * S, (icx - 0.6) * S, (icy - 1.9) * S), fill=(255, 118, 148, 255))
            el(d, ((icx + 0.6) * S, (icy - 9.3) * S, (icx + 7.5) * S, (icy - 1.9) * S), fill=(255, 118, 148, 255))
            poly(d, [((icx - 7.5) * S, (icy - 4.3) * S), ((icx + 7.5) * S, (icy - 4.3) * S),
                     (icx * S, (icy + 9.3) * S), (icx * S, (icy - 4.3) * S)], (255, 118, 148, 255))
        # hl 只给 diff 管线的 code 眼用：那支画在原画眼上方，原画的眼睛几乎不反光
        # （深色虹膜、上睑压低），照搬大肥鱼那两个白点就是贴在脸上的玻璃珠。
        hf = FACE["highlight"] * hl
        _clip_el(d, (icx - ixr * 0.38 * hf) * S, (icy - iyr * 0.42 * hf) * S,
                 ixr * 0.26 * hf * S, iyr * 0.26 * hf * S, clip, (255, 255, 255, 250))
        _clip_el(d, (icx + ixr * 0.36 * hf) * S, (icy + iyr * 0.50 * hf) * S,
                 ixr * 0.18 * hf * S, iyr * 0.18 * hf * S, clip, (255, 255, 255, 175))
    # 上眼睑：参考图那道睫毛线是整张脸最重的一笔，4.6 逻辑宽缩到屏上是 1px，
    # 读不出来，必须给到 6.4
    d.arc([(cx - rx) * S, (cy2 - ry2) * S, (cx + rx) * S, (cy2 + ry2) * S], 182, 358,
          fill=LINE, width=int(FACE["lash_w"] * S))
    # 睫毛尖朝外：左眼朝左、右眼朝右。原来两边都朝左，右眼看上去像被拉歪
    if FACE["lash_tip"]:
        poly(d, [((cx + sgn * rx * 0.96) * S, (cy2 - ry2 * 0.24) * S),
                 ((cx + sgn * rx * 1.36) * S, (cy2 - ry2 * 0.64) * S),
                 ((cx + sgn * rx * 0.66) * S, (cy2 - ry2 * 0.94) * S)], LINE)


def _skin_fill_ellipse(img, cx, cy, rw, rh_up, rh_dn=None, flat=False):
    """把一块椭圆抹成皮肤。坐标一律画布像素（已乘 S）。

    填色从这块区域外圈一圈的亮像素里取中位——那就是它周围真正的皮肤色。
    端点单点取样会被原画眼线的灰、腮红的粉带跑（闭眼时那块发灰的圆片就是它），
    所以端点找不到真皮肤时直接用环带中位色，宁可平一点也不要一道脏色。
    上下半径分开：眼睛上方紧贴着眉毛和刘海，抹宽了会把它们一起洗掉。
    """
    rh_dn = rh_up if rh_dn is None else rh_dn
    px = img.load()
    w, h = img.size

    base = None
    vals = []
    bx0 = max(0, int(cx - rw - 6 * S))
    bx1 = min(w, int(cx + rw + 6 * S))
    by0 = max(0, int(cy - rh_up - 6 * S))
    by1 = min(h, int(cy + rh_dn + 6 * S))
    for y in range(by0, by1):
        rh = rh_up if y < cy else rh_dn
        dy = (y - cy) / rh
        for x in range(bx0, bx1):
            dx = (x - cx) / rw
            d2 = dx * dx + dy * dy
            if not (1.15 < d2 < 2.4):
                continue
            p = px[x, y]
            if p[3] > 200 and min(p[0], p[1], p[2]) >= 190:
                vals.append(p)
    if vals:
        # 取中位：0.75 分位是为了躲眼窝阴影，但立绘上环带里大半是亮部，取高分位
        # 抹出来的一整块比脸还白，屏上就是「眼睛浮在白补丁上」
        vals.sort(key=lambda q: q[0] + q[1] + q[2])
        base = vals[min(len(vals) - 1, int(len(vals) * 0.5))]

    def pick(xs, step, y):
        x = xs
        for _ in range(int(12 * S)):
            x += step
            if x < 0 or x >= w:
                break
            p = px[x, y]
            if p[3] > 200 and min(p[0], p[1], p[2]) >= 190:
                return p
        return None

    x1 = min(w - 1, int(round(cx + rw)))
    y0 = max(0, int(round(cy - rh_up)))
    y1 = min(h - 1, int(round(cy + rh_dn)))
    done = 0
    for y in range(y0, y1 + 1):
        rh = rh_up if y < cy else rh_dn
        dy = (y - cy) / rh
        if abs(dy) >= 1.0:
            continue
        half = rw * math.sqrt(1.0 - dy * dy)
        lx = int(round(cx - half))
        rx2 = min(x1, int(round(cx + half)))
        if lx >= rx2:
            continue
        pl = pr = base
        if not flat:
            pl = pick(lx, -1, y)
            pr = pick(rx2, 1, y)
        if pl is None and pr is None:
            if base is None:
                continue
            pl = pr = base
        elif pl is None:
            pl = pr if base is None else base
        elif pr is None:
            pr = pl if base is None else base
        seg = rx2 - lx
        for x in range(lx, rx2 + 1):
            t = (x - lx) / float(seg)
            px[x, y] = (int(pl[0] + (pr[0] - pl[0]) * t),
                        int(pl[1] + (pr[1] - pl[1]) * t),
                        int(pl[2] + (pr[2] - pl[2]) * t), 255)
        done += seg + 1
    return done


def _erase_eye_on(out, ckey, skey, cut=0.0, ex=1.25, flat=False):
    """在输出画布上抹掉一只原画眼（cut>0 时只抹上面那截，下半瞳孔留着）。

    坐标系是输出画布（已乘 S），不是头部图内部——底板现在保留原画五官，
    只在闭眼/半闭这类真的要改眼睛的表情上局部抹，抹完留下的那块才看不见。

    半径取「归一 rx」与「原画 rx×HEAD_K」的较大者再放 25%：眼距归一让两只眼
    各挪了几个逻辑像素，单用哪边的半径都会剩一圈没抹到的灰。
    """
    cx, cy, rxn, ryn = _CAST[ckey]
    _, _, rsx, rsy = _CAST[skey]
    rw = max(rxn, rsx * HEAD_K) * ex
    rh = max(ryn, rsy * HEAD_K) * ex
    px, py = cx * S, cy * S
    box = (max(0, int(px - rw * S)), max(0, int(py - rh * S)),
           min(out.width, int(px + rw * S)), min(out.height, int(py + rh * S)))
    before = out.crop(box).copy()
    dn = (cut - 0.25) if cut > 0.0 else 1.12
    # flat 只给 code 眼用：逐行从两端取色在立绘上会两头都采到高光，抹出来一整块比脸白。
    # 宁可平一点——反正上面马上要盖一只完整的矢量眼
    _skin_fill_ellipse(out, px, py, rw * S, rh * S, max(0.02, dn) * rh * S, flat=flat)
    # 抹完把头发还回去：五张立绘里刘海都压到眼上沿，平白抹白一块就是在脸中间
    # 刷了一道墙。逐色判一次再映射回来，hair 判据是标量 lambda 不能直接喂数组。
    pred = _CAST.get("hair")
    if pred is None:
        return
    a = np.array(before).astype(np.int32)
    flat = a[..., :3].reshape(-1, 3)
    uniq, inv = np.unique(flat, axis=0, return_inverse=True)
    ok = np.array([bool(pred(int(c[0]), int(c[1]), int(c[2]))) for c in uniq])
    hair = ok[inv].reshape(a.shape[:2]) & (a[..., 3] > 160)
    if not hair.any():
        return
    dst = out.crop(box)
    dst.paste(before, (0, 0), Image.fromarray((hair * 255).astype(np.uint8), "L"))
    out.paste(dst, (box[0], box[1]))


def _erase_mouth_on(out, src_c=None):
    """在输出画布上抹掉原画嘴（换嘴型前先粉掉，否则两个嘴叠着）。

    src_c = 原画嘴的真实中心（由嘴贴片的墨迹重心反算）。配置里的 src_mouth 是手填的，
    和原画差十几像素时这个椭圆就盖不住旧嘴，屏上留下半个嘴印子。
    """
    sx, sy, hw = _CAST["src_mouth"]
    if src_c is not None:
        sx, sy = src_c
    cx = (sx - HEAD_CROP[0]) * HEAD_K + HEAD_OX
    cy = (sy - HEAD_CROP[1]) * HEAD_K + HEAD_OY
    rw = hw * 1.45 * HEAD_K
    _skin_fill_ellipse(out, cx * S, cy * S, rw * S,
                       hw * 0.85 * HEAD_K * S, hw * 1.05 * HEAD_K * S)


def closed_eye(d, cx, cy, kind="happy"):
    pair = _eye_pair(cx)
    sgn = -1 if cx < 120 else 1
    cx = _eye_cx(cx)
    # 眼位和眼宽必须跟睁眼一致：原来这一支把 cy 硬掰到基底的 EYE_CY、半径也用基底的 13.3，
    # 角色模式下就是「闭眼画在另一个高度、宽度对不上眼睛」——眨眼贴片一贴，
    # 屏上看到的是眼睛旁边多了一道弧。半径全按 w 的比例算，基底分支与旧值等价。
    if pair is not None:
        # ry_mul 必须和 eye() 同步：不同步就是「睁眼窄、闭眼圆」，眨眼时眼型跳一下
        cy, w, hh = pair[1], pair[2], pair[3] * FACE["ry_mul"]
        # 眼区不用在这里抹：底图上原画的眼睛已由 _erase_cast_eyes 抹掉，
        # 闭眼态的眼区本来就是一块干净皮肤，画一道弧就行
    else:
        cy, w, hh = EYE_CY, 13.3, 10.8
    if kind == "happy":
        d.arc([(cx - w * 1.07) * S, (cy - hh * 1.5) * S,
               (cx + w * 1.07) * S, (cy + hh * 1.6) * S], 190, 350,
              fill=LINE, width=int(FACE["lid_w"] * S))
    elif kind == "flat":
        # 平静闭眼：眼睫小幅下弯，直线会像两道横杠
        d.arc([(cx - w) * S, (cy - hh * 1.3) * S,
               (cx + w) * S, (cy + hh * 1.1) * S], 15, 165,
              fill=LINE, width=int(FACE["lid_w"] * 0.88 * S))
    elif kind == "sad":
        d.arc([(cx - w * 1.04) * S, (cy - hh * 1.0) * S,
               (cx + w * 1.04) * S, (cy + hh * 1.7) * S], 20, 160,
              fill=LINE, width=int(FACE["lid_w"] * 0.88 * S))
    # 闭着的眼角也要有睫毛，否则睁眼时的睫毛尖会凭空消失
    if FACE["lash_tip"]:
        d.line([((cx + sgn * w * 0.93) * S, (cy - w * 0.083) * S),
                ((cx + sgn * w * 1.27) * S, (cy - w * 0.33) * S)],
               fill=LINE, width=int(2.0 * S))


def blush(d, alpha=1.0, strength=1.0):
    # 腮红必须在眼睛下方一圈：底板上的脸只有 94.6 宽，圆捣出去就画在蓝色头发上。
    # y 141~152 是眼底（144.6）与嘴顶（150）之间唯一容得下它的带
    s = min(strength, 1.2) * FACE["blush"]
    for cx in BLUSH_CX:
        el(d, ((cx - 8.4 * s) * S, BLUSH_Y[0] * S, (cx + 8.4 * s) * S, BLUSH_Y[1] * S), fill=BLUSH)
        el(d, ((cx - 3.9 * s) * S, (BLUSH_Y[0] + 2.7) * S, (cx + 3.9 * s) * S, (BLUSH_Y[1] - 1.8) * S),
           fill=(255, 190, 200, 255))


TALK_MODE = False  # True 时所有嘴型统一换成张嘴，用来出说话贴片
# 嘴周边一块纯皮肤区（240 逻辑坐标）。底板上的下巴在 164.9，再往下就是脖子和衣领，
# 所以下界只能到 166——多留 2 逻辑就会把领口带进口型贴片，说话时下巴跳一下
MOUTH_BOX = (85, 127, 111, 148)

# 嘴型坐标从旧基准（脸中心 x=120 / 嘴线 y=156）线性变换到参考底板实测位置。
# 11 个分支各自手算必错，过一次函数既不会算错，以后挪嘴也只改这两个常量。
MOUTH_C = (97.2, 138.6)
MOUTH_S = 0.52


def _m(*v):
    out = []
    for i, x in enumerate(v):
        val = ((x - 120) * MOUTH_S + MOUTH_C[0]) if i % 2 == 0 else ((x - 156) * MOUTH_S + MOUTH_C[1])
        out.append(val * S)
    return out


def _mouth_dark():
    """唇线色：嘴色压暗，不用 LINE——LINE 是紫黑，配玫红嘴会发脏。"""
    r, g, b = MOUTH[0], MOUTH[1], MOUTH[2]
    return (int(r * 0.62), int(g * 0.42), int(b * 0.48), 255)


def _mouth_deep():
    """口腔色：比唇色更深。162px 屏上 240 画布要缩 0.675 倍，单层平涂的嘴
    会被 LANCZOS 和皮肤平均成一块淡粉印子；三层（唇缘/口腔/舌）拉开明度差
    才能在缩小后还读得出「张开的嘴」。"""
    r, g, b = MOUTH[0], MOUTH[1], MOUTH[2]
    return (int(r * 0.42), int(g * 0.20), int(b * 0.26), 255)


def _mw(f):
    """嘴的线宽（输出像素）：跟嘴宽一起缩放。

    原来固定 2.4*S，落在大肥鱼那张小嘴上正好 1px；五个角色的嘴宽是它的两倍
    （MOUTH_S≈1.05 vs 0.52），同样的 1px 线拉长一倍就成一条淡粉丝，
    屏上读不出嘴型。至少 1px。
    """
    return max(1, int(round(f * MOUTH_S * S)))


def mouth(d, kind="smile"):
    # TALK_MODE 必须用「独立的 if」覆盖 kind，不能写成 if/elif 的第一支：
    # 写成 elif 链时 TALK_MODE=True 会进第一支把 kind 改成 open，然后整条链
    # 全部跳过——说话时嘴一个像素都不画，屏上只剩一张没有嘴的脸。
    if TALK_MODE:
        kind = "open"
    if kind == "attentive":
        # 专注听：只有一点点弧。嘴张成 o 加高眉一凑就是「啊？」——
        # 惊讶和倾听的区别就在嘴唇松不松，而安静地听人是闭着嘴的
        d.arc(_m(111, 148, 129, 162), 25, 155, fill=MOUTH, width=_mw(2.2))
    elif kind == "smile":
        d.arc(_m(108, 149, 132, 166), 20, 160, fill=MOUTH, width=_mw(2.4))
    elif kind == "grin":
        d.chord(_m(106, 148, 134, 167), 0, 180, fill=MOUTH)
        d.chord(_m(112, 158, 128, 166), 0, 180, fill=TONGUE)
    elif kind == "open":
        # 说话时的张嘴。三层实心椭圆叠出来是一整块红，缩到 162px 就是「厚嘴唇」；
        # 简笔的张口是「一条上唇线 + 下面半个月牙」，所以只填下半边 chord，
        # 再压一条唇线在平口上。尺寸跟 smile 的弧同档（宽 22 逻辑 / 屏上 15px），
        # 说话口型比静止嘴还大的话，屏上就是一张一合的血盆大口。
        d.chord(_m(109, 153, 131, 165), 0, 180, fill=_mouth_dark())
        d.chord(_m(112, 159, 128, 165), 0, 180, fill=TONGUE)
        d.arc(_m(109, 153, 131, 165), 0, 180, fill=MOUTH, width=_mw(2.0))
    elif kind == "o":
        # 张嘴必须带舌头：光一个红椭圆夹在两眼下方，无论大小都读成红鼻子。
        # 参考图的 o 嘴其实就是「一个口 + 下面露舌」
        d.ellipse(_m(111, 152, 129, 166.5), fill=_mouth_dark())
        d.ellipse(_m(112.5, 153.5, 127.5, 165), fill=_mouth_deep())
        d.ellipse(_m(115, 159.5, 125, 165), fill=TONGUE)
    elif kind == "flat":
        d.line([_m(113, 156)[0], _m(113, 156)[1], _m(127, 156)[0], _m(127, 156)[1]],
               fill=MOUTH, width=_mw(2.4))
    elif kind == "sad":
        d.arc(_m(109, 152, 131, 167), 200, 340, fill=MOUTH, width=_mw(2.4))
    elif kind == "wavy":
        pts = []
        for i in range(19):
            t = i / 18.0
            pts.append(_m(109 + 22 * t, 156 + (3.0 if i % 2 else -3.0)))
        d.line(pts, fill=MOUTH, width=_mw(2.0), joint="curve")
    elif kind == "cat":
        d.arc(_m(106, 146, 120, 160), 20, 160, fill=MOUTH, width=_mw(2.4))
        d.arc(_m(120, 146, 134, 160), 20, 160, fill=MOUTH, width=_mw(2.4))


def tear(d, side=-1):
    cx = EYE_CX[0] if side < 0 else EYE_CX[1]
    poly(d, [(cx * S, TEAR_Y * S), ((cx + 4.4) * S, (TEAR_Y + 8.0) * S), ((cx - 4.4) * S, (TEAR_Y + 8.0) * S)], (150, 214, 255, 255))
    el(d, ((cx - 4.4) * S, (TEAR_Y + 4.4) * S, (cx + 4.4) * S, (TEAR_Y + 18.7) * S), fill=(150, 214, 255, 255))


def sweat(d):
    # 汗滴只能贴在右脸外缘外侧：脸右边界在 147.6，再往里就压在腮红上
    poly(d, [(140.5 * S, 92.4 * S), (147.6 * S, 104.8 * S), (133.4 * S, 104.8 * S)], (140, 210, 255, 255))
    el(d, (133.4 * S, 97.7 * S, 147.6 * S, 115.4 * S), fill=(140, 210, 255, 255))


FACE_EDGE_SKIP = None


def face_edge_skip(body_img):
    """描边跳过掩码：身体接住的那段脸边界不描边，否则脖子上横一道亮线。

    往外膨两像素是必需的：身体的接缝是渐变的（BODY_TOP_SOFT），上缘几行 alpha 不满，
    只听原 alpha 的话，紧贴着接缝的那圈描边还是能透出来（紫苑和青苹身上那条直横线
    就是这么来的）。"""
    ba = body_img.split()[3]
    # 二值化：渐变后身体上缘的 alpha 只有 20 左右，直接拿它当掩码等于没盖，
    # 描边照样从接缝那里透出来。只看“这里有没有身体”，不看多淡。
    ba = ba.point(lambda v: 255 if v > 8 else 0)
    m = Image.new("L", (OUT, OUT), 0)
    m.paste(ba, (BODY_OX, BODY_OY), ba)
    return m.filter(ImageFilter.MaxFilter(5))


def add_outer_line(img, r=1):
    """最外圈：1px 亮实线 + 1px 半透明光晕，把头从深色背景里拔出来。
    逐形状 outline 只能画出每个形状自己的边，脸与头发、头发与外缘的交接处会留断口，
    整张 alpha 外扩再减回原 alpha，才能保证「头 vs 背景」是一条连续的线。
    ring 保留 grown-a 的差值而不是二值化，抗锯齿边缘得到的是一圈渐变实线，不是硬边。

    FACE_EDGE_SKIP 非空时，它不透明的地方不描边：脸底下被身体接住的那段边界归身体负责，
    两边都画就是一条亮线(168,200,246)横在脖子上，缩小到 162px 就是「头身之间一道白缝」。"""
    a = img.split()[3]

    def dilated(rad):
        # 必须用 MaxFilter（不回绕）。早先用 ImageChops.offset 逐像素铺开，它会把图
        # 上下左右卷绕：裁窗最上一行是头发（不透明），卷到最下一行就凭空长出一圈
        # 「外轮廓」，在脖子底部横着一条亮线。身体接上来之后这条线正好栈在下巴下面，
        # 屏上就是「嘴和下巴之间多了一道白缝」。
        return a.filter(ImageFilter.MaxFilter(2 * rad + 1))

    g1 = dilated(1)
    g2 = dilated(2)
    ring_a = ImageChops.subtract(g1, a)
    halo_a = ImageChops.subtract(g2, g1).point(lambda v: int(v * 0.34))
    if FACE_EDGE_SKIP is not None and FACE_EDGE_SKIP.size == img.size:
        keep = ImageChops.invert(FACE_EDGE_SKIP)
        ring_a = ImageChops.multiply(ring_a, keep)
        halo_a = ImageChops.multiply(halo_a, keep)
    # 裁窗下缘永远不是剪影边界（下面接的是身体），这里描边就是在脖子上画一道亮线。
    # 跳过掩码只管得住身体不透明的地方，肩侧的道具/衣角不在里面，所以下缘两行直接封掉。
    if ring_a.height >= 3:
        cut = (0, ring_a.height - 2, ring_a.width, ring_a.height)
        ring_a.paste(0, cut)
        halo_a.paste(0, cut)
    out = Image.new("RGBA", img.size, (0, 0, 0, 0))
    halo = Image.new("RGBA", img.size, RIM_GLOW)
    halo.putalpha(halo_a)
    out.alpha_composite(halo)
    ring = Image.new("RGBA", img.size, RIM)
    ring.putalpha(ring_a)
    out.alpha_composite(ring)
    out.alpha_composite(img)
    return out


# ---- Q 版小身体：独立一张静态图，垫在头像下面 -------------------------
# 不摊进 21 张脸里去：身体对每个表情都完全一样，画进每张脸等于把这张图复制 21 份（+450KB flash），
# 而且会让眨眼贴片机制多背一份“睁眼/闭眼完全相同的像素”——diff_bbox 靠的就是这些像素完全相同。
BODY_LW, BODY_LH = 240, 120   # 逻辑画布，跟头像同宽同比例：两边同源才能拼接无缝
BODY_W, BODY_H = sc(BODY_LW), sc(BODY_LH)   # 输出像素，跟 OUT 同比例缩
# 身体图是从立绘中腰横切下来的，硬切边会在近黑背景上露出一条白直线（原画的
# 白色贴纸描边），看着像贴错了贴纸。顶部几行 alpha 渐隐把这个切口藏起来。
# 角色模式：身体上缘渐变行数（输出像素）。固件里的身体盒尺寸是写死的（162x81），
# 头和身体的纵向比例本来就对不上，硬切面必然露出台阶；渐变把它抹成一段过渡。
BODY_TOP_SOFT = 6
BODY_FADE = 4
SUIT = (88, 110, 158, 255)      # 衣服主色：科技蓝灰，固定色不跟 accent 走，衣服该是衣服自己的颜色
SUIT_DK = (64, 82, 124, 255)
SUIT_LT = (120, 148, 202, 255)
COLLAR = (170, 210, 255, 255)
SHOE = (44, 54, 86, 255)
APRON = (246, 249, 255, 255)
APRON_SH = (206, 222, 244, 255)
TAIL = (86, 112, 172, 255)
TAIL_LT = (140, 168, 220, 255)


# 手臂单独成层：摆臂要动起来，只有把手臂从身体里拆出来才动得了。
# 不用运行期旋转：LVGL 旋转是最近邻采样，90px 宽的小人转 13° 手臂边缘全成台阶；
# 预生成几帧只花 flash，跑起来就是换个 src，一笔运算都不多。
# 手臂必须画在躯干外侧，而且要整条肤色。上一版只露 9px 在躯干旁，填的又是 SUIT_DK
# （64,82,124），和躯干 SUIT（88,110,158）几乎同色——屏幕上只剩两个手掌圆在飘，
# 读不成人。整条臂改肤色也才看得出「这是手臂在抬」，而不是两个球在动。
ARM_PTS = [(66, 35), (44, 86), (22, 77), (43, 25)]
ARM_HAND_DX, ARM_HAND_Y, ARM_HAND_R = 33, 82, 16
ARM_PIVOT = (54, 34)   # 肩枢轴压在躯干里：转起来臂根一直被躯干盖住，肩膀处不会裂缝
# 0°=自然下垂，9°=手抬到最高。上限还是约 20°（再大手掌会被 180 宽画布裁掉），
# 但屏幕上只露得到手臂下半截，20° 时手球一排能窜出 10px，看着像在打拍子。
# 9° 时手球位移才 4px，是“站着轻轻晃”的量级，不是“挥手”
ARM_DEGS = (0, 5, 9)


def _rot(p, pivot, deg):
    a = math.radians(deg)
    ca, sa = math.cos(a), math.sin(a)
    x, y = p[0] - pivot[0], p[1] - pivot[1]
    return (pivot[0] + x * ca - y * sa, pivot[1] + x * sa + y * ca)


BODY_CROP = (0, 620, 1000, 1055)   # 与 HEAD_CROP 完全同一条横向边界；底部留 10px 避开小红书水印
_BODY_CACHE = {}


def _arm_layer(deg):
    """参考图的手臂是抱在胸前的静态姿势，已经焊死在身体底板里，不再有可旋转的手臂层。

    保留函数和三帧，是为了固件侧的 EMOJI_ARM_FRAMES 循环一行都不用改；三帧全透明
    等价于关掉挥手动画。要恢复挥手，得先从底板把两只手抠成独立素材。
    """
    return Image.new("RGBA", (BODY_W, BODY_H), (0, 0, 0, 0))


_SRC_CACHE = {}


def _cast_src():
    """当前角色的源立绘，进缓存前先把「从图边连进来的纯白」抠透明。

    phone 的立绘背景是白色不透明（四角透明，中下部左右是 (255,255,255)），
    头图和身体图会各带一块白板贴在脸上；其它角色边缘本来就是透明，不受影响。
    只抠与边缘连通且接近纯白的像素：角色身上的白衣与背景之间隔着轮廓线，不会误伤。
    """
    if BASE_PNG not in _SRC_CACHE:
        im = Image.open(BASE_PNG).convert("RGBA")
        w, h = im.size
        px = im.load()
        tol = 18
        seen = bytearray(w * h)
        q = deque()

        def bg_white(p):
            return p[3] > 16 and p[0] > 255 - tol and p[1] > 255 - tol and p[2] > 255 - tol

        for x in range(w):
            for y in (0, h - 1):
                if bg_white(px[x, y]):
                    seen[y * w + x] = 1
                    q.append((x, y))
        for y in range(h):
            for x in (0, w - 1):
                if bg_white(px[x, y]):
                    seen[y * w + x] = 1
                    q.append((x, y))
        while q:
            x, y = q.popleft()
            px[x, y] = (0, 0, 0, 0)
            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                nx, ny = x + dx, y + dy
                if 0 <= nx < w and 0 <= ny < h and not seen[ny * w + nx] and bg_white(px[nx, ny]):
                    seen[ny * w + nx] = 1
                    q.append((nx, ny))
        _SRC_CACHE[BASE_PNG] = im
    return _SRC_CACHE[BASE_PNG]


def _fade_top(im, n):
    """把图片顶部 n 行的 alpha 线性渐隐到 0，用于藏切口。"""
    if n <= 0:
        return im
    im = im.copy()
    px = im.load()
    w, h = im.size
    n = min(n, h)
    for y in range(n):
        k = (y + 1) / float(n + 1)
        for x in range(w):
            r, g, b, a = px[x, y]
            if a:
                px[x, y] = (r, g, b, int(a * k))
    return im


# 人物中轴（源图 x）。角色模式由 apply_cast 设，大肥鱼保持 None 走老裁法。
BODY_AXIS = None


def body():
    """身体直接用参考图身体区，不再手绘女仆装。

    手绘身体最致命的是裙摆：多边形画不出荷叶边的布料褶皱，缩到 80px 高就是
    "梯形机器人 + 两根天线"，怎么调坐标都不像。
    """
    if "img" not in _BODY_CACHE:
        im = _cast_src().crop(BODY_CROP)
        if BODY_AXIS is not None:
            # 头是按两眼中心对齐到画布正中的；身体若按整张立绘居中裁，
            # 人物在源图上偏一侧（抱道具的手把构图推歪）就成「头在中间、身子歪」。
            # 按同一根中轴取一个和输出同比例（BODY_W:BODY_H）的窗，头身才同轴，
            # 也不会因为硬拉伸把肩膀压扁。
            want = int(round(im.height * BODY_W / float(BODY_H)))
            left = int(round((BODY_AXIS - BODY_CROP[0]) - want / 2.0))
            sheet = Image.new("RGBA", (want, im.height), (0, 0, 0, 0))
            _paste_head(sheet, im, -left, 0)
            im = sheet
        im = im.resize((BODY_W, BODY_H), Image.LANCZOS)
        # 角色模式：身体盒是固件里写死的 162x81，而头的缩放按「头宽占比」归一，两者
        # 纵向比例天然对不上。于是拉切面下面几行、身上会有一道“台阶”。把上缘拉成
        # 一段渐变，跨过切面的道具/肩线就是慢慢接上，而不是一条硬边。
        _BODY_CACHE["img"] = _fade_top(im, BODY_TOP_SOFT if _CAST else BODY_FADE)
    return _BODY_CACHE["img"]



_HEAD_CACHE = {}
# 裁窗口边缘羽化（立绘原图像素）。立绘里伸出窗口的手臂/道具在矩形边上会留下
# 一条竖白线，屏上就是「贴纸方角」；头发轮廓在窗口内侧，羽化碰不到。
HEAD_EDGE_FADE = 14


def _fade_edges(im, f, bottom=True):
    """裁窗口四条边做 alpha 衰减。立绘里伸出窗口的手臂/道具是实心像素，
    切在矩形上就是一条竖白线，屏上看着就是个贴纸方角；头发轮廓本来就在
    窗口内侧，衰减碰不到它。

    bottom=False：下缘不是「贴纸边」而是和身体的接缝，这里渐隐等于把下巴擦掉，
    透出脖子——屏上读成「嘴长在下巴下面」。只有左右和上缘需要羽化。"""
    f = int(f)
    if f <= 0:
        return im
    w, h = im.size
    fx, fy = min(f, w // 2), min(f, h // 2)
    rx = np.minimum(np.clip(np.arange(w) / float(fx), 0, 1),
                    np.clip((w - 1 - np.arange(w)) / float(fx), 0, 1))
    ry = np.minimum(np.clip(np.arange(h) / float(fy), 0, 1),
                    np.clip((h - 1 - np.arange(h)) / float(fy), 0, 1))
    if not bottom:
        ry = ry.copy()
        ry[h - fy:] = 1.0
    a = np.array(im).astype(np.float32)
    a[..., 3] *= np.minimum(rx[None, :], ry[:, None])
    return Image.fromarray(a.clip(0, 255).astype(np.uint8), "RGBA")


def head_image():
    """参考图头部，按 HEAD_CROP/HEAD_K 缩放到 4x 画布像素。缓存在内存里，21 张脸共用一张。"""
    if "img" not in _HEAD_CACHE:
        im = _fade_edges(_cast_src().crop(HEAD_CROP), HEAD_EDGE_FADE, bottom=False)
        w = int(round(im.width * HEAD_K * S))
        h = int(round(im.height * HEAD_K * S))
        _HEAD_CACHE["img"] = im.resize((w, h), Image.LANCZOS)
        _HEAD_CACHE["size"] = (w, h)
        # 底板保留原画五官：抹了再贴回去的那一版，屏上是「一块比周围亮的皮肤
        # 补丁 + 放大 1.35 倍的眼」，跟原图差得远。现在 open/keep 根本不碰原画，
        # 只有真要换姿势的眼睛/嘴才在局部抹（见 _erase_eye_on / _erase_mouth_on）。
    return _HEAD_CACHE["img"]


def _paste_head(img, head, ox, oy):
    """负偏移安全贴图。头放大后 head_oy 是负的（头顶出画布才是脸占满的构图），
    alpha_composite 不收负坐标，所以要先把两边裁到重叠区再合。"""
    dx, dy = max(0, ox), max(0, oy)
    sx, sy = max(0, -ox), max(0, -oy)
    w = min(head.width - sx, img.width - dx)
    h = min(head.height - sy, img.height - dy)
    if w <= 0 or h <= 0:
        return
    img.alpha_composite(head.crop((sx, sy, sx + w, sy + h)), (dx, dy))


def head_layer(d):
    """把参考图头部贴到画布。左右超出由画布自己裁（宁可裁掉鳍耳外缘，
    也不能让脸偏到一边）。"""
    _paste_head(d._img_ref, head_image(), int(round(HEAD_OX * S)), int(round(HEAD_OY * S)))


def compose(fn):
    img = canvas()
    d = ImageDraw.Draw(img)
    # hair_back 要用「画布引用 + mask」做逐行渐变，ImageDraw 本身不给图，
    # 这里挂一个属性传下去；hair_masks 那种直接 new 出来的 draw 没有它，走单色分支
    d._img_ref = img
    fn(d)
    return img.resize((SZ, SZ), Image.LANCZOS)


def build(spec):
    """只贴底板 + 画五官。头发/发带/鳍耳/身体/尾巴全在底板里，不再重画。"""
    def fn(d):
        head_layer(d)
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


def listening_face(look=(-4, -3), lid=1.0):
    """倾听脸的同一套画法，只允许换「瞳孔落点」和「眼皮高度」。
    眼神贴片和原生 listening 必须同源：下面 gaze_patches 拿它逐像素求差，
    两边各写一份画法，眉毛位置上差一个像素就会把眉毛整段算进贴片。"""
    def fn(d):
        brows(d, "listen")
        # look 横向不能超 6px：虹膜半径 25.6 比眼白 32 只差这么点，再多就露出眼白外了。
        # 眼睛不能跟着放大：放大+抬眉+张嘴就是标准的「惊话」组合拳
        eye(d, 88, 116, rx=31, ry=36, look=look, lid=lid)
        eye(d, 152, 116, rx=31, ry=36, look=look, lid=lid)
        blush(d, strength=0.45)
        mouth(d, "attentive")
    return fn


def s_listening(d):
    """倾听：人在听人说话时的脸。关键约束：这张图缩到 120px 屏上，
    任何小于 3px 的差别都等于没改。所以只留三个读得出来的信号：
    眉明显抬高（专注，不是惊讶）、瞳孔往一侧上方看去（注意力在对方脸上）、
    嘴微张（听人说话时唇是松的）。腮红只能给一半：给满就变成「高兴」不是「在听」。"""
    listening_face()(d)


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
    cx, cy, r = 137.9, 88.8, 10.7
    for a in (0, 90, 180, 270):
        rad = math.radians(a)
        ex, ey = cx + r * math.cos(rad), cy + r * math.sin(rad)
        d.line([((cx + r * 0.42 * math.cos(rad)) * S, (cy + r * 0.42 * math.sin(rad)) * S),
                (ex * S, ey * S)], fill=col, width=int(3.2 * S))


def s_angry(d):
    brows(d, "angry")
    eye(d, 88, 118, rx=29, ry=31)
    eye(d, 152, 118, rx=29, ry=31)
    mouth(d, "wavy")
    anger_mark(d)


def s_surprised(d):
    brows(d, "raised")
    eye(d, 88, 116, big=1.25)
    eye(d, 152, 116, big=1.25)
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
    eye(d, 88, 114, big=1.3)
    eye(d, 152, 114, big=1.3)
    mouth(d, "open")
    sweat(d)


def sunglasses(d):
# 镜片必须比眼睛宽一圈、并且贴着 EYE_CY：镜片中心一旦偏离眼睛，
# 屏幕上就是两片黑圆贴在脸上，读不出「戴墨镜」
    for cx in EYE_CX:
        d.rounded_rectangle([(cx - 14.2) * S, (EYE_CY - 9.8) * S, (cx + 14.2) * S, (EYE_CY + 9.8) * S],
                            radius=7.1 * S, fill=(38, 40, 58, 255), outline=LINE, width=int(1.8 * S))
        d.line([((cx - 9.8) * S, (EYE_CY - 5.3) * S), ((cx - 0.9) * S, (EYE_CY + 8.0) * S)],
               fill=(120, 132, 170, 160), width=int(2.5 * S))
    d.line([(97.0 * S, (EYE_CY - 3.6) * S), (97.0 * S, (EYE_CY - 0.9) * S)], fill=LINE, width=int(2.5 * S))
    d.line([(59.7 * S, (EYE_CY - 1.8) * S), (52.6 * S, (EYE_CY - 5.3) * S)], fill=LINE, width=int(2.1 * S))
    d.line([(132.5 * S, (EYE_CY - 1.8) * S), (139.6 * S, (EYE_CY - 5.3) * S)], fill=LINE, width=int(2.1 * S))


def tongue(d, side=0):
    """舌头挂在嘴上，坐标必须过 _m 跟嘴走。原来写死 y=137.5，嘴一按 MOUTH_DY 挪
    下去，舌头就飘在原来的位置——屏上读成一个粉色鼻子。"""
    cx, cy = _m(120 + side * 1.0, 156)[0], MOUTH_C[1]
    y0 = cy - 1.1
    el(d, ((cx - 5.9 * S), y0 * S, (cx + 5.9 * S), (y0 + 8.6) * S), fill=TONGUE)
    d.line([(cx, y0 * S), (cx, (y0 + 7.0) * S)], fill=(226, 108, 122, 200), width=max(1, int(round(1.4 * S))))


def drool(d):
    dy = MOUTH_C[1] - 138.6
    poly(d, [(105.9 * S, (143.9 + dy) * S), (110.3 * S, (150.1 + dy) * S), (101.4 * S, (150.1 + dy) * S)],
         (168, 214, 255, 235))
    el(d, (100.6 * S, (144.7 + dy) * S, 111.2 * S, (156.3 + dy) * S), fill=(168, 214, 255, 235))
    el(d, (134.3 * S, 104.8 * S, 141.4 * S, 112.8 * S), fill=(168, 214, 255, 210))


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
    for side, sx in ((1, 50.8), (-1, 143.2)):
        poly(d, [(sx * S, 113.7 * S), ((sx + side * 5.3) * S, 121.7 * S), ((sx - side * 2.7) * S, 122.6 * S)],
             (150, 214, 255, 240))


def s_crying(d):
    brows(d, "sad")
    eye(d, 88, 120, lid=0.62, look=(0, 4))
    eye(d, 152, 120, lid=0.62, look=(0, 4))
    for cx in EYE_CX:
        d.rounded_rectangle([(cx - 3.7) * S, 127.9 * S, (cx + 3.7) * S, 145.6 * S], radius=3.2 * S,
                            fill=(150, 214, 255, 230))
        el(d, ((cx - 4.3) * S, 136.8 * S, (cx + 4.3) * S, 149.2 * S), fill=(150, 214, 255, 230))
    blush(d, strength=1.1)
    if TALK_MODE:
        mouth(d, "open")
    else:
        d.ellipse(_m(107, 146, 133, 168), fill=MOUTH)
        el(d, _m(110, 158, 130, 166), fill=TONGUE)


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
        d.line(_m(108, 156, 132, 156), fill=MOUTH, width=int(2.2 * S))


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
        el(d, _m(114, 150, 126, 162), fill=MOUTH)
    heart(d, 146.7, 94.1, 7.1)
    heart(d, 157.4, 76.4, 5.3)


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
        d.arc(_m(107, 146, 133, 164), 20, 160, fill=MOUTH, width=int(2.4 * S))
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
    el(d, _m(112, 148, 128, 165), fill=MOUTH)


def s_talk_open(d):
    brows(d, "normal")
    eye(d, 88, 118, rx=29, ry=33)
    eye(d, 152, 118, rx=29, ry=33)
    blush(d, strength=0.7)
    el(d, _m(110, 148, 130, 172), fill=MOUTH)
    el(d, _m(114, 163, 126, 170), fill=TONGUE)


REGISTRY = {
    "neutral": s_neutral,
    "listening": s_listening,
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
MOUTH_W = sc(MOUTH_BOX[2] - MOUTH_BOX[0])
MOUTH_H = sc(MOUTH_BOX[3] - MOUTH_BOX[1])
MOUTH_OX = sc(MOUTH_BOX[0])
MOUTH_OY = sc(MOUTH_BOX[1])


# 身体相对头像左上角的偏移（输出坐标）：横向居中，纵向必须落在「裁窗上缘」这条线上。
# BODY_CROP 的上缘就是下巴（cast 侧由 CHIN_Y_TARGET 反算出来的那一行），所以它的输出位置
# 只能由同一个 ly() 推回去，不能写死大肥鱼的源图行号 620：五个角色的下巴都在屏 y=127，
# 身体却钉在 y=100，脖子整整高出 27px 把嘴盖住——屏上就是「嘴长在下巴下面、脖子上面」。
# 下巴接缝：身体上缘比下巴再高多少屏幕像素。零就是两个图刚好在一条线上，
# 横向切断面（尤其是横切过道具浅色描边的情况）会直接露在脖子上下。
BODY_JOIN_OVERLAP = 6.0


def _body_oy():
    return sc((BODY_CROP[1] - HEAD_CROP[1]) * HEAD_K + HEAD_OY)


BODY_OX, BODY_OY = (OUT - BODY_W) // 2, _body_oy()
# 本脚本在 tools/ 下，产物要落到项目根的 main/display/ 与 _emoji_out/
HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


# ---------------------------------------------------------------- 差分表情（角色模式）
# 角色立绘的睫毛、虹膜渐变、皮肤过渡是手绘的，代码画不出来——这就是角色版表情
# 一直像「贴了张假脸」的根本原因。角色模式换一套画法：五官不重绘。
#   睁眼/闭嘴 = 原画那只眼、那张嘴原样贴回，这一路零重绘
#   闭眼      = 眼墨抹平后画一条上睑线。抹的范围只到眼墨膨胀一圈，从画面边缘
#               伸进来的刘海（bangs 角色）一律留着，否则头发上会开个洞
#   情绪      = 眼睛姿态 + 嘴型 + 一个符号（怒气纹/汗滴/爱心/Zzz），不画眉毛：
#               这几张立绘的眉毛本来被头发压着，硬画就是在头发上浮两条线
DIFF_EYE_EX, DIFF_EYE_EY = 1.26, 1.38      # 抠眼外扩：要含上眼睑粗线和睫毛尖
DIFF_MOUTH_EX, DIFF_MOUTH_EY = 1.30, 1.55
DIFF_INK_GAP = 26        # 与皮肤色差超过它才算「墨」
DIFF_LASH = (58, 40, 46, 255)
# 上睑姿态：dy_in/dy_out 是内、外眼角的纵向偏移，bulge 是中间拱起量，都以 ry 为单位
DIFF_LIDS = {
    "closed": (0.06, 0.06, 0.55),
    "sleepy": (0.02, 0.02, 0.34),
    "sad": (-0.18, 0.30, 0.30),
    "angry": (0.34, -0.26, 0.06),
}
# 半闭档：第 4 项 cut 是「只抹掉上眼的比例」，留一点原瞳孔在下面，眼神才活着。
# sad/angry 用整抹是为了让上睑线成为唯一的形，半闭档用来看得见眼睛
for _k, _c in (("half", 0.26), ("half2", 0.40), ("half3", 0.54)):
    DIFF_LIDS[_k] = (0.10, 0.10, 0.42, _c)
for _k in list(DIFF_LIDS):
    DIFF_LIDS[_k] = DIFF_LIDS[_k] + (0.0,) if len(DIFF_LIDS[_k]) == 3 else DIFF_LIDS[_k]

# 22 个表情 → (眼睛姿态, 嘴型, 符号)。眼睛姿态给 tuple 就是左右不同（眨眼）。
# 差分画法表达不了爱心眼/瞳孔偏移，这类降级成闭眼+符号；宁可少一个效果，
# 也不要程序凭空画一颗爱心瞳孔——那正是上一版丑的来源。
CAST_STATES = {
    "neutral": ("open", "keep", ""),
    "listening": ("open", "keep", ""),
    "happy": ("open", "big", ""),
    "laughing": ("closed", "big", ""),
    "funny": ("closed", "big", "tongue"),
    "sad": ("sad", "keep", "sad"),
    "angry": ("code_half", "closed", "angry"),
    "crying": ("sad", "o", "tear"),
    "loving": ("open", "keep", "love"),
    "embarrassed": ("open", "keep", "shy"),
    "surprised": ("code", "o", ""),
    "shocked": ("code", "o", "sad"),
    "thinking": ("half", "keep", "ask"),
    "winking": (("closed", "code"), "keep", ""),
    "cool": ("open", "keep", "shade"),
    "relaxed": ("half2", "keep", ""),
    "delicious": ("half3", "big", ""),
    "kissy": ("open", "o", ""),
    "confident": ("code_soft", "keep", "smirk"),
    "sleepy": ("sleepy", "keep", "sleep"),
    "silly": ("closed", "big", "tongue"),
    "confused": ("open", "keep", "ask"),
}

_DIFF_CACHE = {}


def _diff_alpha(w, h, soft=1.4):
    m = Image.new("L", (w, h), 0)
    ImageDraw.Draw(m).ellipse([0, 0, w - 1, h - 1], fill=255)
    return m.filter(ImageFilter.GaussianBlur(soft))


def _ink_centroid(ink, w, h):
    """墨迹重心；没墨迹就退回贴片中心。"""
    if ink is not None and ink.any():
        ys, xs = np.nonzero(ink)
        return float(xs.mean()), float(ys.mean())
    return w / 2.0, h / 2.0


def diff_mouth_sprite():
    """嘴贴片：抠原图嘴，缩放到 cast_config 定的目标宽。

    原图嘴折到 162 屏上只剩 3px（painter 原画嘴宽 34px），按原比例搬等于没嘴，
    所以宽度跟眼距走，五个角色才一样读得出。
    """
    if "mouth" in _DIFF_CACHE:
        return _DIFF_CACHE["mouth"]
    head = _cast_src()
    mx, my, mhw = _CAST["src_mouth"]
    ex, ey = mhw * DIFF_MOUTH_EX, mhw * 0.62 * DIFF_MOUTH_EY
    box = (int(mx - ex), int(my - ey), int(mx + ex), int(my + ey))
    sp0 = head.crop(box)
    ink0 = _diff_ink(sp0, tuple(_CAST["colors"]["skin"])[:3])
    if ink0.any():
        sx, sy = _ink_centroid(ink0, sp0.width, sp0.height)
        src_c = (box[0] + sx, box[1] + sy)
    else:
        src_c = (mx, my)
    tw = max(6, int(round(_CAST["mouth"][2] * DIFF_MOUTH_EX * S)))
    th = max(4, int(round(tw * sp0.height / float(sp0.width))))
    sp = sp0.resize((tw, th), Image.LANCZOS)
    sp.putalpha(_diff_alpha(tw, th))
    res = (sp, _CAST["mouth"][0] * S, _CAST["mouth"][1] * S, src_c)
    _DIFF_CACHE["mouth"] = res
    return res


def diff_base():
    """4x 画布：只有原画头部，五官保持原样。和 head_layer() 同一套落点。"""
    img = canvas()
    _paste_head(img, head_image(), int(round(HEAD_OX * S)), int(round(HEAD_OY * S)))
    return img


def _diff_dist(a, c):
    return (((a - np.asarray(c, np.float32)) ** 2).sum(axis=-1)) ** 0.5


def _diff_mask(keep, blur=1.0):
    return Image.fromarray((keep * 255).astype(np.uint8), "L").filter(
        ImageFilter.GaussianBlur(blur))


def diff_ctx(canvas):
    """两眼之间那块皮肤的中位色 + 眼半径（4x 画布像素，按原画眼尺度）。"""
    cx = (_CAST["eye_l"][0] + _CAST["eye_r"][0]) / 2.0 * S
    cy = ((_CAST["eye_l"][1] + _CAST["eye_r"][1]) / 2.0 + 13) * S
    box = (int(cx - 5 * S), int(cy - 3 * S), int(cx + 5 * S), int(cy + 3 * S))
    box = (max(0, box[0]), max(0, box[1]),
           min(canvas.width, box[2]), min(canvas.height, box[3]))
    a = np.array(canvas.crop(box)).astype(np.float32)
    m = a[..., 3] > 250
    skin = tuple(int(v) for v in np.median(a[..., :3][m], axis=0)) if m.any() else (250, 226, 216)
    return {"skin": skin,
            "bangs": bool(_CAST.get("bangs", False)),
            "hair_pred": _CAST.get("hair"),
            "rx": _CAST["src_eye_l"][2] * DIFF_EYE_EX * HEAD_K * S,
            "ry": _CAST["src_eye_l"][3] * DIFF_EYE_EY * HEAD_K * S}

def _diff_ink(sp, skin):
    a = np.array(sp).astype(np.float32)
    return (_diff_dist(a[..., :3], skin) > DIFF_INK_GAP) & (a[..., 3] > 200)



def _diff_bez(p0, p1, p2, n=30):
    t = np.linspace(0.0, 1.0, n)[:, None]
    pts = ((1 - t) ** 2 * np.array(p0, np.float32)
           + 2 * (1 - t) * t * np.array(p1, np.float32)
           + t ** 2 * np.array(p2, np.float32))
    return [tuple(p) for p in pts]



def diff_eye_state(out, ckey, skey, kind, ctx, side):
    """一只眼的表情姿态。open 直接返回：底板就是原画，什么都不做才最像原图。"""
    if kind == "open":
        return
    if kind.startswith("code"):
        # 原画那双眼是眯着的，情绪强的表情光靠眼睑线动不起来。抹掉原画眼，用 build()
        # 同一套 eye() 画回来：EYE_CX/rx/ry 两边同源，落点和原画眼重叠，不会跳位。
        # lid 分级是「表情强度」，但上限不再是全睁：原画那双眼是上睑压低、虹膜深色，
        # 画成 1.0 的圆眼白就是屏上一块贴纸（book/apple 眼径本身是归一的 1.9 倍，尤其明显）。
        # 现在最高只给 0.86，眼睛竖向露出的高度跟着原画走，情绪靠嘴型和符号担。
        lid = {"code": 0.86, "code_soft": 0.74, "code_half": 0.64}.get(kind, 0.86)
        # cut=0 才是「整只眼都抹掉」：cut>0 走的是「只抹上面那截、下半瞳孔留着」那支，
        # 下半径会被砍到 0.87 倍，原画眼的下半圈留在那儿，画上新眼就是一圈灰环（book/apple 最明显）
        # ex 给 1.0：抹的椭圆和随后画的眼白椭圆完全重合，抹痕正好被新眼盖住。
        # flat=False 同理：矢量眼比原画眼矮（lid 压了上睑），上方必然露出一条抹痕，
        # 平涂那块皮肤比周围亮一档，放大看是一条白带；逐行从两侧取真肤色才接得上。
        _erase_eye_on(out, ckey, skey, 0.0, ex=1.0, flat=False)
        _, _, srx, _ = _CAST[skey]
        nrm = _CAST[ckey][2]
        # 原画眼半径 × HEAD_K 才是它落在画布上的真实大小；归一眼径只是「五个角色统一」的产物。
        # 抹与画必须同一档尺度：抹 1.25 倍而画 1.0 倍，就是在眼周留一圈磨平的皮肤
        # （屏上读成「眼睛浮在一块白补丁上」）；反过来画大了则变成「贴纸大眼」。
        scale = (srx * HEAD_K / nrm) * 1.0 if nrm > 0 else 1.0
        eye(ImageDraw.Draw(out), 88 if side > 0 else 152, _CAST[ckey][1], lid=lid,
            scale=scale, hl=0.55, full_iris=True)
        return
    dy_in, dy_out, bulge, cut = DIFF_LIDS[kind]
    _erase_eye_on(out, ckey, skey, cut)

    cx, cy = _CAST[ckey][0] * S, _CAST[ckey][1] * S
    half = ctx["rx"] * 0.72
    x_in, x_out = cx + side * half, cx - side * half
    y_in, y_out = cy + ctx["ry"] * dy_in, cy + ctx["ry"] * dy_out
    pts = _diff_bez((x_out, y_out), ((x_in + x_out) / 2.0, cy - ctx["ry"] * bulge),
                    (x_in, y_in))
    ImageDraw.Draw(out).line(pts, fill=DIFF_LASH,
                             width=max(2, int(round(ctx["ry"] * 0.17))), joint="curve")


def diff_mouth_state(out, mouth, kind, ctx):
    # keep 也要搬：原画的嘴不在统一位置上（星野偏左 9 逻辑、墨言低 27 逻辑），
    # 留着原嘴不动，五个角色并排就是「有的嘴贴下颚、有的嘴歪一边」。
    # 搬的是原画那张嘴本身（同样的形状和颜色），只改落点。
    sp, cx, cy, src_c = mouth
    _erase_mouth_on(out, src_c)
    w, h = sp.size
    ink = _diff_ink(sp, ctx["skin"])
    # 贴片墨迹重心对准目标点，而不是贴片矩形中心：裁框是围绕手填的 src_mouth 切的，
    # 嘴在框里偏几个像素，按框中心贴就等于把这份偏差原样搬到脸上。
    ix, iy = _ink_centroid(ink, w, h)
    ox, oy = int(round(cx - ix)), int(round(cy - iy))
    wipe = Image.new("RGBA", (w, h), tuple(ctx["skin"]) + (255,))
    wipe.putalpha(_diff_mask(ink, 1.0))
    out.alpha_composite(wipe, (ox, oy))
    if kind == "closed":
        return
    lay = sp.copy()
    lay.putalpha(_diff_mask(ink, 1.0))
    nw, nh = w, h
    if kind == "big":
        nw, nh = int(w * 1.50), int(h * 1.95)
    elif kind == "o":
        nw, nh = int(w * 0.86), int(h * 1.75)
    lay = lay.resize((max(2, nw), max(2, nh)), Image.LANCZOS)
    # 放大也以墨迹重心为锚，否则「笑」的嘴会一边变大一边往下沉
    kx, ky = lay.width / float(w), lay.height / float(h)
    out.alpha_composite(lay, (int(round(cx - ix * kx)), int(round(cy - iy * ky))))


def _diff_zshape(d, x, y, s, col, lw):
    d.line([(x, y), (x + s, y)], fill=col, width=lw)
    d.line([(x + s, y), (x, y + s)], fill=col, width=lw)
    d.line([(x, y + s), (x + s, y + s)], fill=col, width=lw)


def diff_symbol(out, kind, ctx):
    """情绪交给眼睛姿态 + 嘴型 + 这一个小符号。纯符号不是五官，画出来不假。"""
    if not kind:
        return
    elx, ely = _CAST["eye_l"][0] * S, _CAST["eye_l"][1] * S
    erx, ery = _CAST["eye_r"][0] * S, _CAST["eye_r"][1] * S
    mcx, mcy = _CAST["mouth"][0] * S, _CAST["mouth"][1] * S
    rx, ry = ctx["rx"], ctx["ry"]
    lw = max(2, int(round(rx * 0.14)))
    d = ImageDraw.Draw(out)
    if kind == "angry":
        x, y = erx + rx * 0.95, ery - ry * 1.05
        col = (226, 92, 96, 235)
        for dx, dy in ((-2.0, -0.8), (0.6, -2.0), (2.4, 0.4)):
            sx, sy = x + dx * lw, y + dy * lw
            d.line([(sx, sy + lw * 1.7), (sx + lw * 1.4, sy)], fill=col, width=lw)
    elif kind in ("sad", "tear"):
        x = (erx + rx * 0.9) if kind == "sad" else (elx - rx * 0.95)
        y = (ery - ry * 1.0) if kind == "sad" else (ely + ry * 0.9)
        col = (150, 205, 235, 235)
        r = lw * 1.8
        d.polygon([(x, y - r * 1.5), (x - r, y + r * 0.5), (x, y + r),
                   (x + r, y + r * 0.5)], fill=col)
    elif kind == "love":
        col = (255, 132, 168, 240)
        s = lw * 2.1
        x, y = erx + rx * 1.0, ery - ry * 1.35
        d.ellipse([x - s, y - s, x, y], fill=col)
        d.ellipse([x, y - s, x + s, y], fill=col)
        d.polygon([(x - s, y - s * 0.45), (x + s, y - s * 0.45), (x, y + s * 1.1)], fill=col)
    elif kind == "sleep":
        col = (196, 214, 244, 235)
        x, y = erx + rx * 0.85, ery - ry * 1.5
        _diff_zshape(d, x, y, lw * 2.0, col, lw)
        _diff_zshape(d, x + lw * 2.6, y - lw * 3.0, lw * 1.4, col, max(2, lw - 1))
    elif kind == "ask":
        col = (232, 226, 240, 235)
        x, y = erx + rx * 1.0, ery - ry * 1.5
        r = lw * 1.7
        d.arc([x - r, y - r * 1.4, x + r, y + r * 0.6], 200, 30, fill=col, width=lw)
        d.line([(x + r * 0.25, y + r * 0.4), (x, y + r * 1.2)], fill=col, width=lw)
        d.ellipse([x - lw, y + r * 1.9, x + lw, y + r * 1.9 + lw * 2], fill=col)
    elif kind == "shy":
        box = _CAST.get("blush_box")
        col = (255, 150, 172, 96)
        if box:
            d.ellipse([box[0] * S, box[2] * S, box[1] * S, box[3] * S], fill=col)
        else:
            d.ellipse([elx - rx, ely + ry * 1.5, elx + rx, ely + ry * 3.2], fill=col)
            d.ellipse([erx - rx, ery + ry * 1.5, erx + rx, ery + ry * 3.2], fill=col)
    elif kind == "tongue":
        col = (255, 142, 152, 245)
        s = lw * 2.4
        d.ellipse([mcx - s, mcy + s * 0.2, mcx + s, mcy + s * 1.9], fill=col)
    elif kind == "smirk":
        col = (58, 40, 46, 220)
        x0 = mcx + _CAST["mouth"][2] * S * 1.1
        d.arc([x0 - lw * 2.4, mcy - lw * 2.2, x0 + lw * 2.4, mcy + lw * 2.2],
              250, 20, fill=col, width=lw)
    elif kind == "shade":
        # 墨镜：两片深色镜片 + 鼻梁桥 + 一道高光斜线。盖住原眼就是墨镜该有的样子
        col = (34, 30, 42, 238)
        hi = (176, 198, 232, 130)
        ww, hh = rx * 0.98, ry * 0.78
        for cx, cy in ((elx, ely), (erx, ery)):
            d.rounded_rectangle([cx - ww, cy - hh, cx + ww, cy + hh],
                                radius=int(hh * 0.75), fill=col)
            d.line([(cx - ww * 0.6, cy + hh * 0.45), (cx - ww * 0.05, cy - hh * 0.5)],
                   fill=hi, width=max(2, int(lw * 0.8)))
        d.line([((elx + erx) / 2.0, ely), ((elx + erx) / 2.0 + 1, ery)], fill=col,
               width=max(2, lw))


def cast_face(name, eye=None, mouth=None, extra=None):
    """角色模式的整脸，返回 240 图（与 build() 出口同尺寸）。

    22 个表情全部落到「眼睛姿态 + 嘴型 + 符号」这三维上，画法一套，
    区别只是参数——这样每个新表情的边际成本是一个表项，不是一段绘图代码。
    """
    ek, mk, ex = CAST_STATES.get(name, CAST_STATES["neutral"])
    if eye is not None:
        ek = eye
    if mouth is not None:
        mk = mouth
    if extra is not None:
        ex = extra
    img = diff_base()
    ctx = diff_ctx(img)
    for side, ckey, skey in ((1, "eye_l", "src_eye_l"), (-1, "eye_r", "src_eye_r")):
        if isinstance(ek, tuple):
            kind = ek[0] if side == 1 else ek[1]
        else:
            kind = ek
        diff_eye_state(img, ckey, skey, kind, ctx, side)
    diff_mouth_state(img, diff_mouth_sprite(), mk, ctx)
    diff_symbol(img, ex, ctx)
    return img.resize((SZ, SZ), Image.LANCZOS)



def _emoji_name_of(fn):
    """fn → 表情名。角色模式下 blink/口型只拿到函数对象，得反查回名字才能查
    CAST_STATES（两个圈存的是同一批函数对象，用 is 比对，不靠 __name__）。"""
    for n, f in list(REGISTRY.items()) + list(ANIM.items()):
        if f is fn:
            return n
    return "neutral"


def rgba(name, fn, talk=False):
    # 角色脸走哪条管线：抠原画五官做差分，还是像大肥鱼一样用代码画。
    # 原画眼球小、五官在立绘里只有十来像素；162px 屏上抠下来就是一坨蓝色斑，
    # 而代码画的矢量眼自带高光/睫毛/眼白，同一尺寸下读得出「大眼Q版」。
    if _CAST is not None and not CAST_CODE_FACE:
        return add_outer_line(cast_face(name).resize((OUT, OUT), Image.LANCZOS))
    global TALK_MODE
    TALK_MODE = talk
    try:
        img = build(fn)
    finally:
        TALK_MODE = False
    return add_outer_line(img.resize((OUT, OUT), Image.LANCZOS))


def mouth_patch(fn):
    if _CAST is not None and not CAST_CODE_FACE:
        # 口型贴片：同一条 cast_face 管线换个嘴型再裁嘴框，底色和脸同源，
        # 不像单独画一张嘴贴上去会露一圈色差
        img = cast_face(_emoji_name_of(fn), eye="open", mouth="big")
        return img.crop(MOUTH_BOX).resize((MOUTH_W, MOUTH_H), Image.LANCZOS)
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
    if _CAST is not None and not CAST_CODE_FACE:
        # 本来就闭眼的表情（happy/laughing…）闭上后和原图一样，差异为空，
        # blink_patches 会自己跳过它们，不必在这里特判
        return add_outer_line(
            cast_face(_emoji_name_of(fn), eye="closed").resize((OUT, OUT), Image.LANCZOS))
    global BLINK_MODE, BLINK_STYLE
    BLINK_MODE, BLINK_STYLE = True, style
    try:
        img = build(fn)
    finally:
        BLINK_MODE, BLINK_STYLE = False, "flat"
    # 必须和 rgba() 走同一套外轮廓：少了这圈，睁眼版有轮廓、闭眼版没有，
    # 整圈外轮廓全被算成「睁闭眼差异」，眨眼贴片从 72x42 涨到 104x110（+750KB flash）
    return add_outer_line(img.resize((OUT, OUT), Image.LANCZOS))


def diff_bbox(a, b, threshold=None):
    d = ImageChops.difference(a.convert("RGB"), b.convert("RGB"))
    t = BLINK_DIFF_MIN if threshold is None else threshold
    d = d.point(lambda v: 255 if v > t else 0)
    return d.getbbox()


# ---------------------------------------------------------------- 底图 + 表情补丁
# 22 张全脸 1.86MB，其中 71% 的像素跟 neutral 逐字节相同（量出来的，不是猜的）。
# 放大到 162 时「每表情存一张全脸」直接超分区 1.5MB；只存差异块，总占用反而比 120 的全脸方案更小。
BASE_NAME = "neutral"
PATCH_PAD = 2        # 输出像素。差分边界正好压在描边抗锯齿上，紧贴裁切会把那圈切掉
PATCH_DIFF_MIN = 8


def face_bbox(a, b):
    """RGBA 四通道都要比：只比 RGB 会漏掉「底图透明、这张脸有色」的像素（眼神高光、汗滴）。"""
    d = ImageChops.difference(a, b)
    ch = d.split()
    m = ch[0]
    for c in ch[1:]:
        m = ImageChops.lighter(m, c)
    m = m.point(lambda v: 255 if v > PATCH_DIFF_MIN else 0)
    return m.getbbox()


def face_patches(items):
    """返回 {name: (补丁图, (x, y, w, h))}，底图自己不出补丁。
    补丁是整块裁切而不是只挑差异像素——理由和眼神贴片一样：眼皮/嘴型一变轮廓，
    只按差异交换会在边界留一圈旧描边的抗锯齿白毛刺。"""
    base = items[BASE_NAME]
    out = {}
    for name, im in items.items():
        bb = face_bbox(base, im)
        if bb is None:
            continue
        x0 = max(0, bb[0] - PATCH_PAD) & ~1
        y0 = max(0, bb[1] - PATCH_PAD) & ~1
        x1 = min(OUT, bb[2] + PATCH_PAD)
        y1 = min(OUT, bb[3] + PATCH_PAD)
        # 宽高取偶：LVGL 按 2 字节 stride 拷 RGB565，奇数宽最后一列会踩到下一行
        if (x1 - x0) & 1:
            x1 = min(OUT, x1 + 1)
        if (y1 - y0) & 1:
            y1 = min(OUT, y1 + 1)
        out[name] = (im.crop((x0, y0, x1, y1)), (x0, y0, x1 - x0, y1 - y0))
    return out


def shadow_map(items, body_img):
    """投影：脸+身体的 alpha 合成一个剪影，模糊后下沉几个像素垫在身后。
    小人从 120 长到 162 后几乎顶满屏，没投影它就是张纸贴在背景上。
    只存 A8（1 字节/像素），颜色由 C 侧 recolor 给。"""
    w, h = OUT, BODY_OY + BODY_H
    m = Image.new("L", (w, h), 0)
    face = items[BASE_NAME].split()[3]
    m.paste(face, (0, 0), face)
    b = body_img.split()[3]
    m.paste(b, (BODY_OX, BODY_OY), b)
    # 投影别怕小：亮度给低一点、模糊收窄，小人几乎顶满屏时大团黑雾只会把角色泣没
    m = m.filter(ImageFilter.GaussianBlur(OUT * 0.035))
    # 光从上方来，影往下落：不偏移的投影看着像发光
    out = Image.new("L", (w, h), 0)
    out.paste(m, (0, int(OUT * 0.028)))
    return out.point(lambda v: int(v * 0.45))


def edge_vignette(w, h, depth, strength):
    """四边向内的线性压暗，拼成矩形暗角：LVGL 没有径向渐变，一张 240x240 A8 要 57KB，
    四条 24px 的边只要 23KB，屏幕这么小看不出是四条拼的。"""
    v = Image.new("L", (w, h), 0)
    px = v.load()
    for y in range(h):
        for x in range(w):
            d = min(x, y, w - 1 - x, h - 1 - y)
            if d < depth:
                px[x, y] = int(strength * (1.0 - d / float(depth)))
    return v



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


def swap_patch(a, b, box, threshold=None):
    """把 a→b 真正变了的像素从 b 抠出来，其余 alpha=0。
    阈值可调：眨眼用 24 挡头发抖动，眼神必须调到 6——
    旧瞳孔边缘是抗锯齿渐变，差 20 上下的像素过不了 24，瞳孔一挪就会带一圈旧边。"""
    t = BLINK_DIFF_MIN if threshold is None else threshold
    ac, bc = a.crop(box), b.crop(box)
    w, h = ac.size
    out = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    pa, pb, po = ac.load(), bc.load(), out.load()
    for y in range(h):
        for x in range(w):
            p, q = pa[x, y], pb[x, y]
            if max(abs(q[k] - p[k]) for k in range(4)) > t:
                po[x, y] = q
    return out


def blink_patch(a, b, box):
    """贴片只承载「睁眼→闭眼真正变化」的像素，其余一律 alpha=0。
    头发/眉毛/腮红/皮肤两版完全一样，贴片一碰它们就出事：写进头发像素会在染色层下方闪回原蓝，
    留空又会把刘海下缘啃出锯齿和一条缝。只交换差异像素，眼睛以外的地方谁都别动。"""
    return swap_patch(a, b, box)


def blink_hair_patches(closed, box):
    """眨眼时贴片会带出「睁眼被眼球盖住、闭眼才露出的刘海」，这些像素在睁眼版遮罩里是空的，
    于是闪回原蓝。这里给每个会眨的表情再出一张闭眼版遮罩的局部裁片，贴片上按发色染一遍。"""
    if box is None:
        return {}
    out = {}
    for name, img in closed.items():
        out[name] = hair_masks({name: img})[name].crop(box)
    return out


# ---------------------------------------------------------------- 聆听眼神
# 一张静脸撑不起「在听」：真人听人说话，眼睛一直在对方脸上移动。
# 120px 的脸上瞳孔挑不到 3px 就白挑，所以横向给到 6~10 逻辑像素（屏上 3~5px）；
# 纵向不能超 5：眼白 ry=36、虹膜高 30.2，再多就把眼白露出去了。
# lid<1 是眼皮微垂——「边听边琢磨」比「瞪着眼」更像在听
GAZE_SPECS = (
    ("gaze_aside", (6, -1), 1.0),     # 视线扫向一侧
    ("gaze_think", (-2, 4), 0.86),    # 眼皮微垂往下看：在琢磨
    ("gaze_wide", (-2, -2), 1.05),    # 眼睛微微睁大：听到重点的那一下
)
# 虹膜边缘是抗锯齿渐变，过不了眨眼的 24。用 24 换眼神，瞳孔一挑就带一圈旧边
GAZE_DIFF_MIN = 6


def replace_region(a, b, box, hair=None):
    """整块拿 b 的像素换掉 a 在 box 内的内容，不按差异挑。
    眼神贴片必须这么做：眼皮/眼白一变形，旧眼睑描边的抗锯齿像素就跟新版对不上，
    只按差异交换会在眼周留一圈白毛刺（试过，比不换还难看）。
    头发像素必须留空：贴片里的原蓝发丝会盖住染色层，一挑眼神额头就闪回蓝色。"""
    bc = b.crop(box)
    w, h = bc.size
    out = bc.copy()
    po = out.load()
    if hair is not None:
        ph = hair.crop(box).load()
        for y in range(h):
            for x in range(w):
                if ph[x, y] > 8:
                    po[x, y] = (0, 0, 0, 0)
    return out


def diff_gaze_patches(base, hair_mask):
    """角色模式的眼神帧。

    原画瞳孔是死的：抠出来平移会跟底下的原瞳孔双影，整眼平移会在眼白上留错位。
    能做且不假的是眼皮：三帧上眼睑依次垂一点，轮播起来是「眼睛在动」而不是
    「一张贴图在跳」——人听人说话时眼睑本来就在微微起伏。
    """
    imgs = [add_outer_line(cast_face("listening", eye=k).resize((OUT, OUT), Image.LANCZOS))
            for k in ("half", "half2", "half3")]
    box = None
    for img in imgs:
        df = diff_bbox(base, img, GAZE_DIFF_MIN)
        if df is None:
            continue
        box = df if box is None else (min(box[0], df[0]), min(box[1], df[1]),
                                      max(box[2], df[2]), max(box[3], df[3]))
    if box is None:
        return [], None
    box = (box[0] & ~1, box[1] & ~1, box[2] + (box[2] & 1), box[3] + (box[3] & 1))
    return [replace_region(base, img, box, hair_mask) for img in imgs], box


def gaze_patches(base, hair_mask):
    """眼神贴片：只存眼睛相对原生 listening 脸变了的那块像素。
    用独立包围盒——眼皮微垂会改整个眼睛轮廓，套眨眼的 bbox 会把眼皮裁掉半边。"""
    if _CAST is not None and not CAST_CODE_FACE:
        return diff_gaze_patches(base, hair_mask)
    imgs = [rgba(name, listening_face(look, lid)) for name, look, lid in GAZE_SPECS]
    box = None
    for img in imgs:
        df = diff_bbox(base, img, GAZE_DIFF_MIN)
        if df is None:
            continue
        box = df if box is None else (min(box[0], df[0]), min(box[1], df[1]),
                                      max(box[2], df[2]), max(box[3], df[3]))
    if box is None:
        return [], None
    box = (box[0] & ~1, box[1] & ~1, box[2] + (box[2] & 1), box[3] + (box[3] & 1))
    return [replace_region(base, img, box, hair_mask) for img in imgs], box


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


def a8_alias(pairs, base=None):
    """返回 {别名: 承载字节的那个名字}，值等于自己就不进 dict。

    表情只换五官、五官补丁不碰发区，所以 22 张换发色遮罩内容差异 ≤1.1% 像素，
    且差异全落在补丁矩形内（染完肉眼不可分，参考 _share_mask_vis.py 的并排图）。
    传 base 就全部指向 base 那一张，不传就按内容哈希精确合并。
    """
    if base:
        alias = {name: base for name, _img in pairs if name != base}
        if len(alias) == len(pairs):
            raise RuntimeError("share base %r 不在列表里" % base)
        return alias
    canon, alias = {}, {}
    for name, img in pairs:
        key = (img.size, img.tobytes())
        if key in canon:
            alias[name] = canon[key]
        else:
            canon[key] = name
    return alias


def export_png(out, items):
    for name, img in items.items():
        bg = Image.new("RGB", (OUT, OUT), (12, 12, 16))
        bg.paste(img, (0, 0), img)
        bg.save(os.path.join(out, name + ".png"))
        # 带 alpha 的同一张图：上面那张压了深色底，任何「脸是方块/剪影被切」的
        # 离线核对都会把底色误读成实心像素。诊断一律读这个 _rgba_ 版本。
        img.save(os.path.join(out, "_rgba_" + name + ".png"))


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


def dsc_a8(name, w, h, data_of=None):
    return ("\nconst lv_image_dsc_t %s = {\n"
            "    .header.magic = LV_IMAGE_HEADER_MAGIC,\n"
            "    .header.cf = LV_COLOR_FORMAT_A8,\n"
            "    .header.flags = 0,\n"
            "    .header.w = %d,\n"
            "    .header.h = %d,\n"
            "    .header.stride = %d,\n"
            "    .data_size = %d,\n"
            "    .data = %s_map,\n};\n" % (name, w, h, w, w * h, data_of or name))


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


def export_c(items, patches, mouths, blinks, bbox, hair, blink_hair, body_img, arm_imgs, gazes, gbox,
             shadow, shade_img, body_hair, body_hair_shade):
    # 角色模式下产物落到 _emoji_out/<key>/，绝不砸固件里大肥鱼那份 emoji_assets.c
    hdr_dir = _OUT_C_DIR or os.path.join(HERE, "main", "display")
    os.makedirs(hdr_dir, exist_ok=True)
    hdr = os.path.join(hdr_dir, "emoji_assets.h")
    src = os.path.join(hdr_dir, "emoji_assets.c")
    names = list(items.keys())
    bw = (bbox[2] - bbox[0]) if bbox else 0
    bh = (bbox[3] - bbox[1]) if bbox else 0
    gw = (gbox[2] - gbox[0]) if gbox else 0
    gh = (gbox[3] - gbox[1]) if gbox else 0
    with open(hdr, "w", encoding="utf-8") as f:
        f.write("// 由 tools/gen_emoji_assets.py 生成，勿手改\n")
        f.write("#pragma once\n\n#include \"lvgl.h\"\n\n")
        f.write("#define EMOJI_W %d\n#define EMOJI_H %d\n" % (OUT, OUT))
        f.write("#define MOUTH_W %d\n#define MOUTH_H %d\n" % (MOUTH_W, MOUTH_H))
        f.write("#define MOUTH_OX %d\n#define MOUTH_OY %d\n" % (MOUTH_OX, MOUTH_OY))
        f.write("#define BLINK_W %d\n#define BLINK_H %d\n" % (bw, bh))
        f.write("#define BLINK_OX %d\n#define BLINK_OY %d\n\n" % (bbox[0], bbox[1]))
        # 身体：独立一张，尺寸和相对头像左上角的偏移都在这里定，C 侧不写魔法数
        f.write("#define EMOJI_BODY_W %d\n#define EMOJI_BODY_H %d\n" % (BODY_W, BODY_H))
        f.write("#define EMOJI_BODY_OX %d\n#define EMOJI_BODY_OY %d\n" % (BODY_OX, BODY_OY))
        # 整只小人的纵向占高：文字/天气/气泡全部按这个让位，别再用 EMOJI_H
        f.write("#define EMOJI_TOTAL_H %d\n\n" % (BODY_OY + BODY_H))
        # 投影：整只小人的模糊剪影，A8，只有背景层在它下面
        f.write("#define EMOJI_SHADOW_W %d\n#define EMOJI_SHADOW_H %d\n\n" % shadow.size)
        f.write("extern const lv_image_dsc_t emoji_shadow;\n")
        f.write("#define EMOJI_ARM_FRAMES %d\n" % len(arm_imgs))
        f.write("extern const lv_image_dsc_t emoji_body;\n")
        # 身体图上的下半截长发：+2 张 A8（遮罩 + 阴影），尺寸同 body
        f.write("extern const lv_image_dsc_t emoji_body_hair;\nextern const lv_image_dsc_t emoji_body_hair_shade;\n")
        for i in range(len(arm_imgs)):
            f.write("extern const lv_image_dsc_t emoji_arm_%d;\n" % i)
        f.write("// 表情表：patch 是相对底图 emoji_neutral 的差异块，NULL 表示这张就是底图本身。\n"
                "// C 侧只显示一张底图，切表情 = 换补丁的 src/pos，不用为 22 张脸各存一份全图\n")
        f.write("typedef struct {\n    const char * name;\n    const lv_image_dsc_t * patch;\n"
                "    int16_t px, py, pw, ph;\n"
                "    const lv_image_dsc_t * mouth;\n    const lv_image_dsc_t * blink;\n"
                "    const lv_image_dsc_t * blink_hair;\n"
                "    const lv_image_dsc_t * hair;\n} emoji_asset_t;\n\n")
        f.write("extern const emoji_asset_t emoji_table[];\nextern const int emoji_table_count;\n")
        # 眼神贴片不是表情：是同一张 listening 脸的眼睛局部变体，C 侧按节奏轮播
        if gbox:
            f.write("#define GAZE_COUNT %d\n" % len(gazes))
            f.write("#define GAZE_W %d\n#define GAZE_H %d\n" % (gw, gh))
            f.write("#define GAZE_OX %d\n#define GAZE_OY %d\n" % (gbox[0], gbox[1]))
            for i in range(len(gazes)):
                f.write("extern const lv_image_dsc_t emoji_gaze_%d;\n" % i)
        for name in names:
            f.write("extern const lv_image_dsc_t emoji_%s_hair;\n" % name)
        f.write("// 换发色时的阴影层：全表情共用一张（头发在底板里，五官补丁不碰发区）\n")
        f.write("extern const lv_image_dsc_t emoji_hair_shade;\n")
        f.write("extern const lv_image_dsc_t emoji_%s;\n" % BASE_NAME)
        for name in names:
            if name in patches:
                f.write("extern const lv_image_dsc_t emoji_patch_%s;\n" % name)
            if name in mouths:
                f.write("extern const lv_image_dsc_t emoji_%s_mouth;\n" % name)
            if name in blinks:
                f.write("extern const lv_image_dsc_t emoji_%s_blink;\n" % name)
                f.write("extern const lv_image_dsc_t emoji_%s_blink_hair;\n" % name)
    with open(src, "w", encoding="utf-8") as f:
        f.write("// 由 tools/gen_emoji_assets.py 生成，勿手改\n")
        f.write("#include \"emoji_assets.h\"\n\n")
        _hair_base = "emoji_%s_hair" % BASE_NAME
        _bh_base = "emoji_%s_blink_hair" % BASE_NAME
        bh_alias = a8_alias([("emoji_%s_blink_hair" % n, blink_hair[n]) for n in names if n in blinks], _bh_base)
        hair_alias = a8_alias([("emoji_%s_hair" % n, hair[n]) for n in names], _hair_base)
        f.write(c_array("emoji_body", to_rgb565a8(body_img)))
        f.write(c_array("emoji_body_hair", to_a8(body_hair)))
        f.write(c_array("emoji_body_hair_shade", to_a8(body_hair_shade)))
        for i, im in enumerate(arm_imgs):
            f.write(c_array("emoji_arm_%d" % i, to_rgb565a8(im)))
        f.write(c_array("emoji_shadow", to_a8(shadow)))
        f.write(c_array("emoji_" + BASE_NAME, to_rgb565a8(items[BASE_NAME])))
        for name, (pimg, _box) in patches.items():
            f.write(c_array("emoji_patch_" + name, to_rgb565a8(pimg)))
        for name in names:
            if name in mouths:
                f.write(c_array("emoji_" + name + "_mouth", to_rgb565a8(mouths[name])))
            if name in blinks:
                f.write(c_array("emoji_" + name + "_blink", to_rgb565a8(blinks[name])))
                if "emoji_" + name + "_blink_hair" not in bh_alias:
                    f.write(c_array("emoji_" + name + "_blink_hair", to_a8(blink_hair[name])))
        for name in names:
            if "emoji_" + name + "_hair" not in hair_alias:
                f.write(c_array("emoji_" + name + "_hair", to_a8(hair[name])))
        f.write(c_array("emoji_hair_shade", to_a8(shade_img)))
        for i, im in enumerate(gazes):
            f.write(c_array("emoji_gaze_%d" % i, to_rgb565a8(im)))
        f.write(dsc_a8("emoji_shadow", shadow.size[0], shadow.size[1]))
        f.write(dsc("emoji_" + BASE_NAME, OUT, OUT))
        for name, (pimg, _box) in patches.items():
            w, h = pimg.size
            f.write(dsc("emoji_patch_" + name, w, h))
        for name in names:
            if name in mouths:
                f.write(dsc("emoji_" + name + "_mouth", MOUTH_W, MOUTH_H))
            if name in blinks:
                f.write(dsc("emoji_" + name + "_blink", bw, bh))
                f.write(dsc_a8("emoji_" + name + "_blink_hair", bw, bh, bh_alias.get("emoji_" + name + "_blink_hair")))
        for name in names:
            f.write(dsc_a8("emoji_" + name + "_hair", OUT, OUT, hair_alias.get("emoji_" + name + "_hair")))
        f.write(dsc_a8("emoji_hair_shade", OUT, OUT))
        f.write(dsc("emoji_body", BODY_W, BODY_H))
        f.write(dsc_a8("emoji_body_hair", BODY_W, BODY_H))
        f.write(dsc_a8("emoji_body_hair_shade", BODY_W, BODY_H))
        for i in range(len(arm_imgs)):
            f.write(dsc("emoji_arm_%d" % i, BODY_W, BODY_H))
        for i in range(len(gazes)):
            f.write(dsc("emoji_gaze_%d" % i, gw, gh))
        f.write("\nconst emoji_asset_t emoji_table[] = {\n")
        for name in names:
            talk = "&emoji_%s_mouth" % name if name in mouths else "NULL"
            blink = "&emoji_%s_blink" % name if name in blinks else "NULL"
            blink_hair = "&emoji_%s_blink_hair" % name if name in blinks else "NULL"
            if name in patches:
                _pimg, (px, py, pw, ph) = patches[name]
                f.write("    {\"%s\", &emoji_patch_%s, %d, %d, %d, %d, %s, %s, %s, &emoji_%s_hair},\n"
                        % (name, name, px, py, pw, ph, talk, blink, blink_hair, name))
            else:
                f.write("    {\"%s\", NULL, 0, 0, 0, 0, %s, %s, %s, &emoji_%s_hair},\n"
                        % (name, talk, blink, blink_hair, name))
        f.write("};\n\nconst int emoji_table_count = sizeof(emoji_table) / sizeof(emoji_table[0]);\n")
    return src, os.path.getsize(src)


# 每个 slot 必须恰好这么多 dsc：emoji_pack.c 里 n_dsc != emoji_dsc_count 就拒绑，
# 而 emoji_dsc_count 是固件骨架（main/display/emoji_assets.c）里数组的长度。
# 差分表情下 patch/blink 数量会因角色而异（有的表情恰巧和底图一样，face_bbox 跳过它），
# 所以不猜最大值，统一跳到这个上限：改这个常量必须重跑 --pack 并拷骨架，
# 否则固件骨架和 slot 对不上，屏上就是一张图也不出来。
DSC_CAP = 120


def _write_pack(out, items, patches, mouths, blinks, bbox, hair, blink_hair,
                    body_img, arm_imgs, gazes, gbox, shadow, shade_img,
                body_hair, body_shade, bw, bh, gw, gh):
    """把这份角色的全部像素摊平成一个 slot，C 侧只剩骨架。

    dsc 顺序由这里定，emoji_dsc_slots[] 也由这里产出——C 侧不维护第二份顺序表，
    少一个两边对不上的机会。
    """
    import _pack_assets as PK
    names = list(items.keys())
    blobs, dscs = [], []

    def add(sym, kind, w, h, data):
        blobs.append((sym, kind, w, h, data))
        dscs.append(sym)
        return len(dscs) - 1

    add("emoji_shadow", 0, shadow.size[0], shadow.size[1], to_a8(shadow))
    add("emoji_body", 1, BODY_W, BODY_H, to_rgb565a8(body_img))
    add("emoji_body_hair", 0, BODY_W, BODY_H, to_a8(body_hair))
    add("emoji_body_hair_shade", 0, BODY_W, BODY_H, to_a8(body_shade))
    for i, im in enumerate(arm_imgs):
        add("emoji_arm_%d" % i, 1, BODY_W, BODY_H, to_rgb565a8(im))
    add("emoji_" + BASE_NAME, 1, OUT, OUT, to_rgb565a8(items[BASE_NAME]))
    pidx = {}
    for name, (pimg, _box) in patches.items():
        pidx[name] = add("emoji_patch_" + name, 1, pimg.size[0], pimg.size[1], to_rgb565a8(pimg))
    midx, bidx, bhidx = {}, {}, {}
    base_bh = to_a8(blink_hair[BASE_NAME]) if BASE_NAME in blink_hair else None
    for name in names:
        if name in mouths:
            midx[name] = add("emoji_%s_mouth" % name, 1, MOUTH_W, MOUTH_H, to_rgb565a8(mouths[name]))
        if name in blinks:
            bidx[name] = add("emoji_%s_blink" % name, 1, bw, bh, to_rgb565a8(blinks[name]))
            bhidx[name] = add("emoji_%s_blink_hair" % name, 0, bw, bh, base_bh)
    hidx = {}
    base_hair = to_a8(hair[BASE_NAME])
    for name in names:
        hidx[name] = add("emoji_%s_hair" % name, 0, OUT, OUT, base_hair)
    add("emoji_hair_shade", 0, OUT, OUT, to_a8(shade_img))
    for i, im in enumerate(gazes):
        add("emoji_gaze_%d" % i, 1, gw, gh, to_rgb565a8(im))

    faces = []
    for name in names:
        px = py = pw = ph = 0
        pi = PK.NULL_IDX
        if name in patches:
            _pimg, (px, py, pw, ph) = patches[name]
            pi = pidx[name]
        faces.append((px, py, pw, ph, pi,
                      midx.get(name, PK.NULL_IDX), bidx.get(name, PK.NULL_IDX),
                      bhidx.get(name, PK.NULL_IDX), hidx[name]))

    target = DSC_CAP
    if len(dscs) > target:
        raise SystemExit("角色 %s 有 %d 个 dsc，超过 DSC_CAP=%d：把 DSC_CAP 加大并重跑全量"
                         % (os.path.basename(out), len(dscs), target))
    pad_data = to_rgb565a8(Image.new("RGBA", (1, 1), (0, 0, 0, 0)))
    i = 0
    while len(dscs) < target:
        add("emoji_pad_%d" % i, 1, 1, 1, pad_data)
        i += 1
    if i:
        print("pad %d 个占位 dsc -> %d" % (i, target))

    payload = PK.slot_payload(blobs, faces)
    pkdir = os.path.join(out, "_pack")
    os.makedirs(pkdir, exist_ok=True)
    slot = os.path.join(pkdir, os.path.basename(out) + ".slot")
    with open(slot, "wb") as f:
        f.write(payload)

    macros = [
        ("EMOJI_W", OUT), ("EMOJI_H", OUT),
        ("MOUTH_W", MOUTH_W), ("MOUTH_H", MOUTH_H),
        ("MOUTH_OX", MOUTH_OX), ("MOUTH_OY", MOUTH_OY),
        ("BLINK_W", bw), ("BLINK_H", bh),
        ("BLINK_OX", bbox[0] if bbox else 0), ("BLINK_OY", bbox[1] if bbox else 0),
        ("EMOJI_BODY_W", BODY_W), ("EMOJI_BODY_H", BODY_H),
        ("EMOJI_BODY_OX", BODY_OX), ("EMOJI_BODY_OY", BODY_OY),
        ("EMOJI_TOTAL_H", BODY_OY + BODY_H),
        ("EMOJI_SHADOW_W", shadow.size[0]), ("EMOJI_SHADOW_H", shadow.size[1]),
        ("EMOJI_ARM_FRAMES", len(arm_imgs)),
        ("GAZE_COUNT", len(gazes)), ("GAZE_W", gw), ("GAZE_H", gh),
        ("GAZE_OX", gbox[0] if gbox else 0), ("GAZE_OY", gbox[1] if gbox else 0),
    ]
    with open(os.path.join(pkdir, "emoji_assets.h"), "w", encoding="utf-8") as f:
        f.write(PK.skeleton_h(macros, dscs, names, len(gazes)))
    with open(os.path.join(pkdir, "emoji_assets.c"), "w", encoding="utf-8") as f:
        f.write(PK.skeleton_c(dscs, names, [(n,) for n in names]))
    print("pack %s: %d dsc, %d faces -> %s (%d B)"
          % (os.path.basename(out), len(dscs), len(faces), slot, len(payload)))


# 大肥鱼的全部参数快照：apply_cast(None) 靠它精确还原，不能省。
# 放在末尾是因为要等上面所有常量（含 MOUTH_C/MOUTH_BOX）都定义完。
_DEFAULT_BASE_PNG = BASE_PNG
_DEFAULT_HEAD_CROP = HEAD_CROP
_DEFAULT_HEAD_K = HEAD_K
_DEFAULT_HEAD_OX = HEAD_OX
_DEFAULT_HEAD_OY = HEAD_OY
_DEFAULT_BODY_CROP = BODY_CROP
_DEFAULT_BODY_OX = BODY_OX
_DEFAULT_BODY_OY = BODY_OY
_DEFAULT_EYE_CX = EYE_CX
_DEFAULT_EYE_CY = EYE_CY
_DEFAULT_HAIR_EXCL = HAIR_EXCL
_DEFAULT_MOUTH_C = MOUTH_C
_DEFAULT_MOUTH_S = MOUTH_S
_DEFAULT_MOUTH_BOX = MOUTH_BOX
_DEFAULT_MOUTH_W = MOUTH_W
_DEFAULT_MOUTH_H = MOUTH_H
_DEFAULT_MOUTH_OX = MOUTH_OX
_DEFAULT_MOUTH_OY = MOUTH_OY
_DEFAULT_COLORS = {
    "SKIN": SKIN, "SKIN_SH": SKIN_SH, "LINE": LINE,
    "IRIS_TOP": IRIS_TOP, "IRIS_BOT": IRIS_BOT,
    "MOUTH": MOUTH, "TONGUE": TONGUE, "BLUSH": BLUSH, "OUTLINE": OUTLINE,
}

# 角色模式下的 C 产物目录（None = 直接写固件里的 main/display）
_OUT_C_DIR = None
# --pack：像素写进 assets 分区的 slot，C 只剩骨架（app 分区装不下多角色）
_PACK = False


def _cli_cast():
    """解析 --cast <key>：把该角色的参数注入，并把全部产物改道到 _emoji_out/<key>/。

    返回角色 key（None = 内置大肥鱼）。产物目录必须分开，否则跑一个角色就把
    固件里大肥鱼那套 emoji_assets.c 覆盖掉，下次编译具就静静变成了别人。
    """
    global _OUT_C_DIR, _PACK, CAST_CODE_FACE
    import sys
    key = None
    argv = sys.argv[1:]
    _PACK = "--pack" in argv
    for i, a in enumerate(argv):
        if a == "--cast" and i + 1 < len(argv):
            key = argv[i + 1]
        elif a.startswith("--cast="):
            key = a.split("=", 1)[1]
        elif a == "--code-face" and i + 1 < len(argv):
            CAST_CODE_FACE = argv[i + 1].lower() not in ("0", "false", "no")
        elif a.startswith("--code-face="):
            CAST_CODE_FACE = a.split("=", 1)[1].lower() not in ("0", "false", "no")
    # 两条管线的产物必须分目录：同 key 同路径会互相覆盖，A/B 变成「谁后跑谁」。
    # pack 例外——slot 是 mk_assets_bin.py 按 <key>/_pack/<key>.slot 固定路径取货的，
    # 加后缀它会去空目录里找，报「没有 slot」而不是报错，静默出旧尺寸的 assets。
    suffix = "_orig" if (not CAST_CODE_FACE and not _PACK) else ""
    if not key:
        out = os.path.join(HERE, "_emoji_out", "_base" + suffix)
        if _PACK:
            # pack 时大肥鱼也不碰固件里的 emoji_assets.c，产物全走 _emoji_out
            _OUT_C_DIR = out
        return None, out
    import cast_config
    apply_cast(cast_config.get(key))
    out = os.path.join(HERE, "_emoji_out", key + suffix)
    _OUT_C_DIR = out
    return key, out



if __name__ == "__main__":
    _cast_key, out = _cli_cast()
    os.makedirs(out, exist_ok=True)
    if _cast_key:
        print("=== cast: %s ===" % _cast_key)
        print("HEAD_CROP=%s HEAD_K=%.4f EYE_CX=%s EYE_CY=%.1f MOUTH_C=%s" % (
            HEAD_CROP, HEAD_K, EYE_CX, EYE_CY, MOUTH_C))
    # 必须比 items 先开：脸图的外圈线在生成时就按「身体盖住的地方不描」剪一下
    _body_for_skip = body()
    FACE_EDGE_SKIP = face_edge_skip(_body_for_skip)
    items = {}
    for name, fn in list(REGISTRY.items()) + list(ANIM.items()):
        items[name] = rgba(name, fn)
    mouths = {name: mouth_patch(fn) for name, fn in REGISTRY.items()}
    blinks, bbox, closed = blink_patches(items)
    hair = hair_masks(items)
    patches = face_patches(items)
    gazes, gbox = gaze_patches(items["listening"], hair["listening"])
    blink_hair = blink_hair_patches(closed, bbox)
    for n, (pimg, box) in patches.items():
        pimg.save(os.path.join(out, "_patch_%s.png" % n))
    for n, im in hair.items():
        im.save(os.path.join(out, "_hair_%s.png" % n))
    export_png(out, items)
    for name, img in mouths.items():
        img.save(os.path.join(out, "_mouth_%s.png" % name))
    for name, img in blinks.items():
        img.save(os.path.join(out, "_blink_%s.png" % name))
    for i, im in enumerate(gazes):
        im.save(os.path.join(out, "_gaze_%d.png" % i))
    sheet(out, list(REGISTRY.keys()))
    talk_sheet(out, items, mouths)
    body_img = body()
    body_img.save(os.path.join(out, "_body.png"))
    arm_imgs = [_arm_layer(a) for a in ARM_DEGS]
    for i, im in enumerate(arm_imgs):
        im.save(os.path.join(out, "_arm_%d.png" % i))
    shadow = shadow_map(items, body_img)
    shadow.save(os.path.join(out, "_shadow.png"))
    shade_img = hair_shade(items, hair)
    shade_img.save(os.path.join(out, "_hair_shade.png"))
    body_hair = body_hair_mask(body_img)
    body_hair.save(os.path.join(out, "_body_hair.png"))
    body_shade = body_hair_shade(body_img, body_hair)
    body_shade.save(os.path.join(out, "_body_hair_shade.png"))
    bh_hit = sum(1 for y in range(BODY_H) for x in range(BODY_W) if body_hair.load()[x, y])
    print("body hair mask: %d px (%.1f%% of body)" % (bh_hit, 100.0 * bh_hit / (BODY_W * BODY_H)))
    src, size = export_c(items, patches, mouths, blinks, bbox, hair, blink_hair, body_img, arm_imgs,
                         gazes, gbox, shadow, shade_img, body_hair, body_shade)
    bw, bh = (bbox[2] - bbox[0], bbox[3] - bbox[1]) if bbox else (0, 0)
    total = (OUT * OUT * 3 + sum(im.size[0] * im.size[1] * 3 for im, _b in patches.values())
             + len(mouths) * MOUTH_W * MOUTH_H * 3 + len(blinks) * bw * bh * 4
             + len(items) * OUT * OUT + BODY_W * BODY_H * 5 + shadow.size[0] * shadow.size[1]
             + OUT * OUT)
    print("base %dx%d + %d patches (avg %.1fKB) -> %.0f KB total"
          % (OUT, OUT, len(patches),
             sum(im.size[0] * im.size[1] * 3 for im, _b in patches.values()) / 1024.0 / max(1, len(patches)),
             total / 1024.0))
    print("generated %d faces + %d mouth patches + %d blink patches -> %s"
          % (len(items), len(mouths), len(blinks), out))
    print("mouth patch %dx%d at (%d,%d)" % (MOUTH_W, MOUTH_H, MOUTH_OX, MOUTH_OY))
    print("blink patch %dx%d at (%d,%d), faces: %s" % (bw, bh, bbox[0], bbox[1], ",".join(blinks)))
    if gbox:
        print("gaze patches %d x %dx%d at (%d,%d)"
              % (len(gazes), gbox[2] - gbox[0], gbox[3] - gbox[1], gbox[0], gbox[1]))
    print("body %dx%d at (%d,%d), total height %d" % (BODY_W, BODY_H, BODY_OX, BODY_OY, BODY_OY + BODY_H))
    print("wrote %s (%.0f KB source, %.0f KB binary)" % (src, size / 1024.0, total / 1024.0))
    if _PACK:
        _gw = (gbox[2] - gbox[0]) if gbox else 0
        _gh = (gbox[3] - gbox[1]) if gbox else 0
        _write_pack(out, items, patches, mouths, blinks, bbox, hair, blink_hair,
                    body_img, arm_imgs, gazes, gbox, shadow, shade_img,
                    body_hair, body_shade, bw, bh, _gw, _gh)
