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


def _hair_blue(p):
    r, g, b, a = p
    return a > 40 and b - r >= HAIR_BR_MIN and b >= HAIR_B_MIN


def _hair_loose(p):
    r, g, b, a = p
    return a > 40 and b - r >= HAIR_LOOSE_BR and b >= HAIR_LOOSE_B


def _hair_bright(p):
    """发束外缘那道亮边。它紧贴尾巴内缘、又在几何允许带外，原来整条漏染，
    换发色后发束右缘留一条原蓝（用户报的「鱼尾那边一块没变色」）。
    它 b>=160/b-r>=80，尾巴本体 b~120/b-r~54，两者在这条线上分得开——
    所以只拿它当 allowed（不当 seed），重建能沿亮边长出去，进不了尾巴内部。"""
    r, g, b, a = p
    return a > 40 and b >= HAIR_BRIGHT_B and b - r >= HAIR_BRIGHT_BR


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


def brows(d, style="normal"):
    # 眉毛在参考图里是被刘海半盖着的短弧，y 112~120 才露在发际下方。
    # 跟着眼睛下移后不能再往下一分：眼顶在 120.2，压上去就被眼白盖掉
    for cx in EYE_CX:
        inner = cx > 96
        w = 2.3 * S
        if style == "angry":
            y1 = 100.7 if inner else 103.0
            y2 = 103.0 if inner else 100.7
            d.line([((cx - 6.6) * S, y1 * S), ((cx + 6.6) * S, y2 * S)], fill=LINE, width=int(w))
        elif style == "sad":
            y1 = 103.0 if inner else 100.7
            y2 = 100.7 if inner else 103.0
            d.line([((cx - 6.6) * S, y1 * S), ((cx + 6.6) * S, y2 * S)], fill=LINE, width=int(w))
        elif style == "listen":
            d.arc([(cx - 6.6) * S, 94.1 * S, (cx + 6.6) * S, 103.9 * S], 192, 348,
                  fill=LINE, width=int(w))
        elif style == "raised":
            d.arc([(cx - 6.6) * S, 92.4 * S, (cx + 6.6) * S, 102.1 * S], 195, 345,
                  fill=LINE, width=int(w))
        else:
            d.arc([(cx - 6.6) * S, 96.8 * S, (cx + 6.6) * S, 105.7 * S], 212, 328,
                  fill=LINE, width=int(2.8 * S))


BLINK_MODE = False  # True 时所有睁眼改画闭眼，用来出眨眼贴片
BLINK_STYLE = "flat"


EYE_CX = (74.4, 118.1)   # 从参考底板实测：原图眼心 310/492 × k=0.24
EYE_CY = 117.6


def _eye_cx(cx):
    """眼距归一。21 张脸里的 cx 一律写死 88/152（内距 4 逻辑），缩到屏上两眼粘成一团；
    逐个调用点改容易漏一个，直接在入口掰到 84/156。"""
    return EYE_CX[0] if cx < 120 else EYE_CX[1]


def eye(d, cx, cy, rx=23, ry=16, lid=1.0, look=(0, 0), heart=False, big=1.0):
    cx = _eye_cx(cx)
    cy = EYE_CY
    # 眼型直接锁死：调用点里散着 rx=29/ry=33、rx=31/ry=36 的旧值，
    # 那是「竖椭圆大眼」，缩到屏上眼白连成镜片。参考图是横宽、高只有宽的 0.7，
    # 而且虹膜几乎填满眼白（只留一圈窄白）。逐个改 21 个调用点必漏，在这里一刀切。
    # 20 是「眼宽占脸宽 0.34」算出来的，跟参考图一致；再大就贴到鬁发上了
    # big 只给惊讶/震惊用：这两个表情用「瞑目」做信号，参考图里眼睛会大一圈
    # 从参考底板实测：眼宽 110 / 眼高 90（原图 px）× k=0.24 → rx 13.2 / ry 10.8。
    # 宽高比 1.22，比上一版 24/21 更圆；再放大 1.8 倍就把整个头读成「一双眼」
    rx, ry = 13.2 * big, 10.8 * big
    if BLINK_MODE:
        closed_eye(d, cx, cy, BLINK_STYLE)
        return
    cy2 = cy + ry * (1 - lid)
    ry2 = max(ry * lid, 1.0)
    el(d, ((cx - rx) * S, (cy2 - ry2) * S, (cx + rx) * S, (cy2 + ry2) * S), fill=WHITE)
    if lid > 0.4:
        # 虹膜/眼白 = 0.82：参考图虹膜几乎填满眼白，只留一圈窄白。
        # 不能给到 0.9：瞳孔还要靠 look 在眼白里平移，余量小于偏移量就会露出白角
        ixr = rx * 0.82
        iyr = ry2 * 0.80
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
            el(d, ((icx - 7.5) * S, (icy - 9.3) * S, (icx - 0.6) * S, (icy - 1.9) * S), fill=(255, 118, 148, 255))
            el(d, ((icx + 0.6) * S, (icy - 9.3) * S, (icx + 7.5) * S, (icy - 1.9) * S), fill=(255, 118, 148, 255))
            poly(d, [((icx - 7.5) * S, (icy - 4.3) * S), ((icx + 7.5) * S, (icy - 4.3) * S),
                     (icx * S, (icy + 9.3) * S), (icx * S, (icy - 4.3) * S)], (255, 118, 148, 255))
        el(d, ((icx - ixr * 0.64) * S, (icy - iyr * 0.68) * S,
               (icx - ixr * 0.12) * S, (icy - iyr * 0.16) * S), fill=(255, 255, 255, 250))
        el(d, ((icx + ixr * 0.18) * S, (icy + iyr * 0.32) * S,
               (icx + ixr * 0.54) * S, (icy + iyr * 0.68) * S), fill=(255, 255, 255, 175))
    # 上眼睑：参考图那道睫毛线是整张脸最重的一笔，4.6 逻辑宽缩到屏上是 1px，
    # 读不出来，必须给到 6.4
    d.arc([(cx - rx) * S, (cy2 - ry2) * S, (cx + rx) * S, (cy2 + ry2) * S], 182, 358,
          fill=LINE, width=int(3.0 * S))
    # 睫毛尖朝外：左眼朝左、右眼朝右。原来两边都朝左，右眼看上去像被拉歪
    sgn = -1 if cx < 96 else 1
    poly(d, [((cx + sgn * rx * 0.96) * S, (cy2 - ry2 * 0.24) * S),
             ((cx + sgn * rx * 1.36) * S, (cy2 - ry2 * 0.64) * S),
             ((cx + sgn * rx * 0.66) * S, (cy2 - ry2 * 0.94) * S)], LINE)


def closed_eye(d, cx, cy, kind="happy"):
    cx = _eye_cx(cx)
    # cy 必须在入口掰到 EYE_CY：这一支原来直接用调用点传的 118，
    # 而睁眼那边被 EYE_CY 抬到 140——眨眼贴片会整块错位，眉毛被算进差异区
    cy = EYE_CY
    if kind == "happy":
        d.arc([(cx - 14.2) * S, (cy - 5.3) * S, (cx + 14.2) * S, (cy + 14.2) * S], 190, 350,
              fill=LINE, width=int(3.0 * S))
    elif kind == "flat":
        # 平静闭眼：眼睫小幅下弯，直线会像两道横杠
        d.arc([(cx - 13.3) * S, (cy - 5.3) * S, (cx + 13.3) * S, (cy + 6.2) * S], 15, 165,
              fill=LINE, width=int(2.8 * S))
    elif kind == "sad":
        d.arc([(cx - 13.8) * S, (cy - 2.7) * S, (cx + 13.8) * S, (cy + 14.2) * S], 20, 160,
              fill=LINE, width=int(2.8 * S))
    # 闭着的眼角也要有睫毛，否则睁眼时的睫毛尖会凭空消失
    sgn = -1 if cx < 96 else 1
    d.line([((cx + sgn * 12.4) * S, (cy - 1.1) * S), ((cx + sgn * 16.9) * S, (cy - 4.4) * S)],
           fill=LINE, width=int(2.0 * S))


def blush(d, alpha=1.0, strength=1.0):
    # 腮红必须在眼睛下方一圈：底板上的脸只有 94.6 宽，圆捣出去就画在蓝色头发上。
    # y 141~152 是眼底（144.6）与嘴顶（150）之间唯一容得下它的带
    s = min(strength, 1.2)
    for cx in (67.2, 127.2):
        el(d, ((cx - 8.4 * s) * S, 125.2 * S, (cx + 8.4 * s) * S, 135.0 * S), fill=BLUSH)
        el(d, ((cx - 3.9 * s) * S, 127.9 * S, (cx + 3.9 * s) * S, 133.2 * S),
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


def mouth(d, kind="smile"):
    if TALK_MODE:
        kind = "open"
    elif kind == "attentive":
        # 专注听：只有一点点弧。嘴张成 o 加高眉一凑就是「啊？」——
        # 惊讶和倾听的区别就在嘴唇松不松，而安静地听人是闭着嘴的
        d.arc(_m(111, 148, 129, 162), 25, 155, fill=MOUTH, width=int(2.2 * S))
    elif kind == "smile":
        d.arc(_m(108, 149, 132, 166), 20, 160, fill=MOUTH, width=int(2.4 * S))
    elif kind == "grin":
        d.chord(_m(105, 147, 135, 168), 0, 180, fill=MOUTH)
        d.chord(_m(112, 158, 128, 167), 0, 180, fill=TONGUE)
    elif kind == "open":
        el(d, _m(107, 150, 133, 168), fill=MOUTH)
        el(d, _m(112, 160, 128, 167), fill=TONGUE)
    elif kind == "o":
        # 张嘴必须带舌头：光一个红椭圆夹在两眼下方，无论大小都读成红鼻子。
        # 参考图的 o 嘴其实就是「一个口 + 下面露舌」
        el(d, _m(109, 151, 131, 167), fill=MOUTH)
        el(d, _m(113, 159, 127, 167), fill=TONGUE)
    elif kind == "flat":
        d.line([_m(113, 156)[0], _m(113, 156)[1], _m(127, 156)[0], _m(127, 156)[1]],
               fill=MOUTH, width=int(2.4 * S))
    elif kind == "sad":
        d.arc(_m(109, 152, 131, 167), 200, 340, fill=MOUTH, width=int(2.4 * S))
    elif kind == "wavy":
        pts = []
        for i in range(19):
            t = i / 18.0
            pts.append(_m(109 + 22 * t, 156 + (3.0 if i % 2 else -3.0)))
        d.line(pts, fill=MOUTH, width=int(2.0 * S), joint="curve")
    elif kind == "cat":
        d.arc(_m(106, 146, 120, 160), 20, 160, fill=MOUTH, width=int(2.4 * S))
        d.arc(_m(120, 146, 134, 160), 20, 160, fill=MOUTH, width=int(2.4 * S))


def tear(d, side=-1):
    cx = EYE_CX[0] if side < 0 else EYE_CX[1]
    poly(d, [(cx * S, 125.2 * S), ((cx + 4.4) * S, 133.2 * S), ((cx - 4.4) * S, 133.2 * S)], (150, 214, 255, 255))
    el(d, ((cx - 4.4) * S, 129.6 * S, (cx + 4.4) * S, 143.9 * S), fill=(150, 214, 255, 255))


def sweat(d):
    # 汗滴只能贴在右脸外缘外侧：脸右边界在 147.6，再往里就压在腮红上
    poly(d, [(140.5 * S, 92.4 * S), (147.6 * S, 104.8 * S), (133.4 * S, 104.8 * S)], (140, 210, 255, 255))
    el(d, (133.4 * S, 97.7 * S, 147.6 * S, 115.4 * S), fill=(140, 210, 255, 255))


FACE_EDGE_SKIP = None


def add_outer_line(img, r=1):
    """最外圈：1px 亮实线 + 1px 半透明光晕，把头从深色背景里拔出来。
    逐形状 outline 只能画出每个形状自己的边，脸与头发、头发与外缘的交接处会留断口，
    整张 alpha 外扩再减回原 alpha，才能保证「头 vs 背景」是一条连续的线。
    ring 保留 grown-a 的差值而不是二值化，抗锯齿边缘得到的是一圈渐变实线，不是硬边。

    FACE_EDGE_SKIP 非空时，它不透明的地方不描边：脸底下被身体接住的那段边界归身体负责，
    两边都画就是一条亮线(168,200,246)横在脖子上，缩小到 162px 就是「头身之间一道白缝」。"""
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
    ring_a = ImageChops.subtract(g1, a)
    halo_a = ImageChops.subtract(g2, g1).point(lambda v: int(v * 0.34))
    if FACE_EDGE_SKIP is not None and FACE_EDGE_SKIP.size == img.size:
        keep = ImageChops.invert(FACE_EDGE_SKIP)
        ring_a = ImageChops.multiply(ring_a, keep)
        halo_a = ImageChops.multiply(halo_a, keep)
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


def body():
    """身体直接用参考图身体区，不再手绘女仆装。

    手绘身体最致命的是裙摆：多边形画不出荷叶边的布料褶皱，缩到 80px 高就是
    "梯形机器人 + 两根天线"，怎么调坐标都不像。
    """
    if "img" not in _BODY_CACHE:
        im = Image.open(BASE_PNG).convert("RGBA").crop(BODY_CROP)
        _BODY_CACHE["img"] = im.resize((BODY_W, BODY_H), Image.LANCZOS)
    return _BODY_CACHE["img"]


_HEAD_CACHE = {}


def head_image():
    """参考图头部，按 HEAD_CROP/HEAD_K 缩放到 4x 画布像素。缓存在内存里，21 张脸共用一张。"""
    if "img" not in _HEAD_CACHE:
        im = Image.open(BASE_PNG).convert("RGBA").crop(HEAD_CROP)
        w = int(round(im.width * HEAD_K * S))
        h = int(round(im.height * HEAD_K * S))
        _HEAD_CACHE["img"] = im.resize((w, h), Image.LANCZOS)
        _HEAD_CACHE["size"] = (w, h)
    return _HEAD_CACHE["img"]


def head_layer(d):
    """把参考图头部贴到画布。alpha_composite 不接受负数坐标，
    右侧超出部分由画布自己裁（宁可裁掉鳍耳外缘，也不能让脸偏到一边）。"""
    d._img_ref.alpha_composite(head_image(), (int(round(HEAD_OX * S)), 0))


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
    cx = MOUTH_C[0] + side * MOUTH_S
    el(d, ((cx - 5.9) * S, 137.5 * S, (cx + 5.9) * S, 146.1 * S), fill=TONGUE)
    d.line([(cx * S, 137.5 * S), (cx * S, 144.5 * S)], fill=(226, 108, 122, 200), width=int(1.4 * S))


def drool(d):
    poly(d, [(105.9 * S, 143.9 * S), (110.3 * S, 150.1 * S), (101.4 * S, 150.1 * S)], (168, 214, 255, 235))
    el(d, (100.6 * S, 144.7 * S, 111.2 * S, 156.3 * S), fill=(168, 214, 255, 235))
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


# 身体相对头像左上角的偏移（输出坐标）：横向居中，纵向压住后脑勺下缘。
# BODY_CROP 顶部在原图 y=620，乘上头像同一套 k 就是它该落的绝对位置——
# 之前写死 192 是让旧手绘身体的脖子接到下巴上，现在两边同源，对不上就成双下巴。
BODY_OX, BODY_OY = (OUT - BODY_W) // 2, sc(620 * HEAD_K)
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


def gaze_patches(base, hair_mask):
    """眼神贴片：只存眼睛相对原生 listening 脸变了的那块像素。
    用独立包围盒——眼皮微垂会改整个眼睛轮廓，套眨眼的 bbox 会把眼皮裁掉半边。"""
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


def export_c(items, patches, mouths, blinks, bbox, hair, blink_hair, body_img, arm_imgs, gazes, gbox,
             shadow, shade_img, body_hair, body_hair_shade):
    hdr = os.path.join(HERE, "main", "display", "emoji_assets.h")
    src = os.path.join(HERE, "main", "display", "emoji_assets.c")
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
                f.write(c_array("emoji_" + name + "_blink_hair", to_a8(blink_hair[name])))
        for name in names:
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
                f.write(dsc_a8("emoji_" + name + "_blink_hair", bw, bh))
        for name in names:
            f.write(dsc_a8("emoji_" + name + "_hair", OUT, OUT))
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


if __name__ == "__main__":
    out = os.path.join(HERE, "_emoji_out")
    os.makedirs(out, exist_ok=True)
    # 必须比 items 先开：脸图的外圈线在生成时就按「身体盖住的地方不描」剪一下
    _body_for_skip = body()
    FACE_EDGE_SKIP = Image.new("L", (OUT, OUT), 0)
    _ba = _body_for_skip.split()[3]
    FACE_EDGE_SKIP.paste(_ba, (BODY_OX, BODY_OY), _ba)
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
