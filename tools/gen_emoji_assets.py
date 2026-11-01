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
# 屏只有 240 高，162 是「脸占满 + 给 HUD 留 78px（天气 3 行 72px）」的上限：
# 再大就得把天气压回 2 行，上 170 后连对话的最小可读高度都被吃掉
OUT = 162
K = OUT / float(SZ)


def sc(v):
    """逻辑单位 → 输出像素。尺寸/坐标一律过这里，别写死 //2。"""
    return int(round(v * K))

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
        elif style == "listen":
            # 专注：比 raised 再抬 8px，两端略收。差距必须按屏上像素算——
            # 6px 的微抬在 120px 的脸上根本看不见，不能拿「微妙」当「自然」
            d.arc([(cx - 17) * S, 50 * S, (cx + 17) * S, 74 * S], 192, 348, fill=LINE, width=int(3 * S))
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
    elif kind == "attentive":
        # 专注听：只有一点点弧。嘴张成 o 加高眉一凑就是「啊？」——
        # 惊讶和倾听的区别就在嘴唇松不松，而安静地听人是闭着嘴的
        d.arc([110 * S, 150 * S, 130 * S, 166 * S], 25, 155, fill=MOUTH, width=int(3.4 * S))
    elif kind == "smile":
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


# ---- Q 版小身体：独立一张静态图，垫在头像下面 -------------------------
# 不摊进 21 张脸里去：身体对每个表情都完全一样，画进每张脸等于把这张图复制 21 份（+450KB flash），
# 而且会让眨眼贴片机制多背一份“睁眼/闭眼完全相同的像素”——diff_bbox 靠的就是这些像素完全相同。
BODY_LW, BODY_LH = 180, 118   # 逻辑画布，跟头像共用同一套刻度和线宽
BODY_W, BODY_H = sc(BODY_LW), sc(BODY_LH)   # 输出像素，跟 OUT 同比例缩
SUIT = (88, 110, 158, 255)      # 衣服主色：科技蓝灰，固定色不跟 accent 走，衣服该是衣服自己的颜色
SUIT_DK = (64, 82, 124, 255)
SUIT_LT = (120, 148, 202, 255)
COLLAR = (170, 210, 255, 255)
SHOE = (44, 54, 86, 255)


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


def _arm_layer(deg):
    """只有两条手臂的一层，画布和 body 完全同尺寸：C 侧共用同一套偏移宏。
    这一层垫在 body 之下，臂根被躯干盖掉，转到任何角度都不会露缝。"""
    img = Image.new("RGBA", (BODY_LW * S, BODY_LH * S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    ow = int(OUTLINE_W * S)
    for sx in (-1, 1):
        pts = [(BODY_LW - x, y) for x, y in ARM_PTS] if sx > 0 else list(ARM_PTS)
        pivot = (BODY_LW - ARM_PIVOT[0], ARM_PIVOT[1]) if sx > 0 else ARM_PIVOT
        # 右臂是镜像的，同一个角度会往下摆，取反才能跟左边同步抬起
        rr = -deg if sx > 0 else deg
        poly(d, [(x * S, y * S) for x, y in (_rot(p, pivot, rr) for p in pts)],
             SKIN, OUTLINE, ow)
        hx = ARM_HAND_DX if sx < 0 else BODY_LW - ARM_HAND_DX
        hc = _rot((hx, ARM_HAND_Y), pivot, rr)
        el(d, ((hc[0] - ARM_HAND_R) * S, (hc[1] - ARM_HAND_R) * S,
               (hc[0] + ARM_HAND_R) * S, (hc[1] + ARM_HAND_R) * S),
           fill=SKIN, outline=OUTLINE, width=ow)
    # 不做 add_outer_line：手臂是人的一部分，不该自带一圈光。
    # 做过一次，结果是脸两侧拉出两根细天线——那是手臂亮边被脸遮掉一半剩下的残渣
    return img.resize((BODY_W, BODY_H), Image.LANCZOS)


def body():
    """短脖子 + 敦实胶囊躯干 + 两条短腿（手臂在 _arm_layer 里单独一层）。
    三条比例都是算过的，不是拍的：
      · 整只身子（含手）58px < 头 90px —— Q 版靠大头小身子的反差，身体一宽就成冰箱。
      · 躯干 45x40px 敦实，看着是个小人；抽成竖条就变成瘦长成年人。
      · 必须有小短腿：没腿的躯干看着是不倒翁，加了腿才读成“站着的 Q 版小人”。"""
    img = Image.new("RGBA", (BODY_LW * S, BODY_LH * S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    ow = int(OUTLINE_W * S)
    # 脖子：头图盖到绝对 y=107，这里从 96 起，只露出下巴下面一丁点
    d.rounded_rectangle([78 * S, 0, 102 * S, 30 * S], radius=10 * S,
                        fill=SKIN, outline=OUTLINE, width=ow)
    # 腿和鞋：短到只剩两小段，但有没有这两段差别巨大——有腿才站得住。
    # 腿从 86 起画、躯干压到 88，重叠 2 逻辑（1px）只为了不留缝，不能真重叠多——
    # 躯干圆角很大，盖太多会把腿切成只剩鞋面的两根短横线。
    for sx in (-1, 1):
        lx = 58 if sx < 0 else 100
        d.rounded_rectangle([lx * S, 84 * S, (lx + 22) * S, 102 * S], radius=10 * S,
                            fill=SUIT_DK, outline=OUTLINE, width=ow)
        el(d, ((lx - 6) * S, 98 * S, (lx + 28) * S, 118 * S), fill=SHOE, outline=OUTLINE, width=ow)
    # 躯干：圆角大到接近胶囊，Q 版的软感全靠这个弧
    d.rounded_rectangle([45 * S, 20 * S, 135 * S, 88 * S], radius=28 * S,
                        fill=SUIT, outline=OUTLINE, width=ow)
    # 小圆领：只贴脖子根一圈，大了就成围嘴
    d.chord([74 * S, 8 * S, 106 * S, 40 * S], 0, 180, fill=COLLAR)
    # 左侧一条窄受光，不然整块纯色看着像纸片
    d.rounded_rectangle([54 * S, 32 * S, 64 * S, 64 * S], radius=5 * S, fill=SUIT_LT)
    # 腰部横条：把躯干分成上衣/下装，不再是一整个桶
    d.rounded_rectangle([52 * S, 70 * S, 128 * S, 80 * S], radius=5 * S, fill=COLLAR)
    return add_outer_line(img.resize((BODY_W, BODY_H), Image.LANCZOS))


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


# 身体相对头像左上角的偏移（输出坐标）：横向居中，纵向压住后脑勺下缘——
# hair_back 到 y=107，从 100 起画身体，后脑勺正好盖住脖子接缝。
# 必须排在 OUT 之后（横向居中要用它算），不能跟着 body() 一起写在前面
BODY_OX, BODY_OY = (OUT - BODY_W) // 2, sc(192)
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
    m = m.filter(ImageFilter.GaussianBlur(OUT * 0.05))
    # 光从上方来，影往下落：不偏移的投影看着像发光
    out = Image.new("L", (w, h), 0)
    out.paste(m, (0, int(OUT * 0.035)))
    return out.point(lambda v: int(v * 0.72))


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
             shadow):
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
        f.write(dsc("emoji_body", BODY_W, BODY_H))
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
    src, size = export_c(items, patches, mouths, blinks, bbox, hair, blink_hair, body_img, arm_imgs,
                         gazes, gbox, shadow)
    bw, bh = (bbox[2] - bbox[0], bbox[3] - bbox[1]) if bbox else (0, 0)
    total = (OUT * OUT * 3 + sum(im.size[0] * im.size[1] * 3 for im, _b in patches.values())
             + len(mouths) * MOUTH_W * MOUTH_H * 3 + len(blinks) * bw * bh * 4
             + len(items) * OUT * OUT + BODY_W * BODY_H * 3 + shadow.size[0] * shadow.size[1])
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
