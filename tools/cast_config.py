# -*- coding: utf-8 -*-
"""角色表情系统的参数表。

一个角色 = 一张「五官填平的立绘」+ 一组坐标/颜色/发色判据。
21 种表情由 gen_emoji_assets.py 的同一套画法产出，各角色只换参数。

坐标一律按【立绘原图像素】填，由 _mk() 换算到 240 逻辑画布——手算这一步出错
会表现为「眼睛画在头发上」，而且五个角色要算五遍，必须交给代码。

换算的锚点从「身体图第一行」改成了「头宽占画布的比例」：
  身体图在输出图上叠在 y=100，输出 162 对应画布 240
  但按 chin_y 反推 k 时，头宽每角色只有 56%，身体比头宽，屏上就是“脸小、歪”。
  现在 k = 0.72*240 / head_crop 宽，头的 x 位置改用两眼中心对齐画布中心。
"""
import os

HERE = os.path.dirname(os.path.abspath(__file__))
CAST_SRC = os.path.join(HERE, "_cast_src")

BODY_OY_LOGIC = 148.1     # 输出 y=100 → 画布 y

# 已试过按「头高统一」定 k（TARGET_HEAD_H=168）：头一放大就暴露原画的
# 头大眼小比例，五个角色全变成大头娃娃，比缩小更难看。k 还是按 crop 宽定。
TARGET_HEAD_H = 168.0
HEAD_RATIO = 0.80
# 眼睛中心在画布上的高度：与大肥鱼 s_neutral 里的 118 对齐。
# 只对头宽不对眼高的话，头型高的角色眼睛会挂到下巴上。
EYE_Y_TARGET = 132.0
# 眼距归一：原画两眼间距从 27.6 到 46.8 屏像素，差 1.7 倍——同一个屏上，
# 有的角色一脸宽眼、有的两眼挤在中间，读起来不像一套里的人。眼距是脸的骨架，
# 先把它钉到同一档，剩下的差异交给发型和瞳色。
EYE_GAP_TARGET = 50.4     # 逻辑像素（屏上 34px）
# 嘴宽同样不跟眼距等比：跟眼距走的话，眼距一归一嘴还是分大小。
# 直接钉目标宽度（逻辑全宽，屏上 17px），五个角色嘴一样大。
MOUTH_W_TARGET = 25.2
# 下巴在画布上的高度（逻辑）：屏幕 127px。眼中心在 132 逻辑，眼→下巴固定 56.2
# 逻辑 = 38 屏像素。脸的下段高度一致，嘴和下唇到下巴的留白才会一致。
CHIN_Y_TARGET = 188.2
# 嘴在「眼睛中心 → 下巴尖」上的比例位置。
# 大肥鱼那张被认可自然的脸：眼中心 117.6、下巴 168、嘴 138.6 → 0.42。
# 早先 0.52 掉到脸下缘、0.20 直接把嘴顶进眼睛下缘（嘴比眼白底还高 1 逻辑），
# 屏上就是「五官挤在一条带上、下半张脸空一块」。回到 0.42 才和大肥鱼同构。
MOUTH_DY = 0.42
# 眼睛尺寸归一：原画眼球大小差 1.9 倍（book 半宽 24.9 逻辑 vs 大肥鱼 13.2），
# 照原画等比缩出来墨言的眼睛占掉半张脸，屏上读成「贴纸眼」而不是人脸。
# 只把量级拉到基准，左右眼近大远小的比例本身保留。
EYE_RX_TARGET = 13.2
# 头（连头发）在画布上的统一宽度。身体盒是固件写死的 162x81（240x120 逻辑），
# 头占它的 0.69 是 Q 版里头顶不压肩的比例。先统一头，五官才开始谈对齐。
TARGET_HEAD_W = 165.0


def _mk(name, title, src, size, head_crop, eye_l, eye_r, mouth, colors,
        hair, blush=None, hair_loose=None, brow=True, bangs=False, face=None,
        head_w_src=None):
    """eye_*: (cx, cy, rx, ry) 原图象素；mouth: (cx, cy, half_w) 原图象素。

    head_w_src：原图「眼行不透明段」的头宽（连头发）。缩放比按它定，不按 head_crop
    宽：crop 是手填的，裁得紧的角色（星野 348/502）头在屏上就比别人大一整圈，
    裁得松的（紫苑 488/488）就小一圈——五个角色头顶大小不一样就是这么来的。
    brow=False：原画里眉毛被鬁发盖住的角色，不画眉毛。
    bangs=True：刘海真的垂到眼睛上，做闭眼时要保住那块头发（否则头发上会开洞）。
    其余角色刘海在额头上方，闭眼时整只眼可以放心抹平。
    """
    hx0, hy0, hx1 = head_crop
    k = (TARGET_HEAD_W / float(head_w_src) if head_w_src
         else HEAD_RATIO * 240.0 / float(hx1 - hx0))
    # 以两眼中心对齐画布中心，而不是 crop 居中：hoop 的 crop 中心比立绘中心
    # 偏右 54px，按 crop 居中脸就整张歪到左边
    eye_mid = (eye_l[0] + eye_r[0]) / 2.0
    head_ox = 240.0 / 2.0 - (eye_mid - hx0) * k
    # 纵向同样对齐：眼睛中心落回 EYE_Y_TARGET。head_oy 允许为负——头放大后
    # 头顶本来就该出画布一点，脸才占得满；早先 max(0) 夹住它，等于把「脸只占
    # 六成宽」焊死，屏上眼睛缩到 17px 就是一堆糊点。
    eye_cy = (eye_l[1] + eye_r[1]) / 2.0
    head_oy = EYE_Y_TARGET - eye_cy * k
    # 下巴不手填、也不按嘴推：直接钉在屏幕上的同一行。手填过的下巴从 372 到 400
    # 各说各话，眼→下巴在屏上就变成 26 到 39 像素——同一屏里有的角色脸长有的脸扁，
    # 嘴自然一会儿压在眼下沿、一会儿掉到脖子边。脸的下段统一了，嘴才谈得上对齐。
    hy1 = int(round((CHIN_Y_TARGET - head_oy) / k + hy0))
    hy1 = min(hy1, size[1])
    head_crop = (hx0, hy0, hx1, hy1)

    def lx(x):
        return (x - hx0) * k + head_ox

    def ly(y):
        return (y - hy0) * k + head_oy

    el = (lx(eye_l[0]), ly(eye_l[1]), eye_l[2] * k, eye_l[3] * k)
    er = (lx(eye_r[0]), ly(eye_r[1]), eye_r[2] * k, eye_r[3] * k)
    # 量级归一到 EYE_RX_TARGET：两只眼各自的 rx/ry 比例关系不动，只乘一个公共系数，
    # 所以近大远小还在，但不会出现「这个角色眼睛是那个的两倍」
    rx_avg = (el[2] + er[2]) / 2.0
    e_sc = EYE_RX_TARGET / rx_avg if rx_avg > 0 else 1.0
    el = (el[0], el[1], el[2] * e_sc, el[3] * e_sc)
    er = (er[0], er[1], er[2] * e_sc, er[3] * e_sc)
    # 眼距归一：只动 x，眼色/瞳位/眼睛大小都不碰。中心不变，所以 head_ox 不用重算。
    eye_mid_x = (el[0] + er[0]) / 2.0
    gap = er[0] - el[0]
    g_sc = EYE_GAP_TARGET / gap if gap > 0 else 1.0
    el = (eye_mid_x + (el[0] - eye_mid_x) * g_sc, el[1], el[2], el[3])
    er = (eye_mid_x + (er[0] - eye_mid_x) * g_sc, er[1], er[2], er[3])
    # mo[2] = smile 嘴的目标逻辑宽度，gen 侧 MOUTH_S = 它/24（smile 跨 24 旧基准单位）
    # 嘴的横向不用原画坐标：立绘里人脸是微侧、抱道具的手臂会把身体推偏，
    # 嘴在源图上经常偏离两眼中心十几像素（墨言偏左 12px）。脸是按眼睛对齐的，
    # 嘴再跟着原画偏，屏上就是「五官不在一条中轴上」——用户原话是「不对称」。
    # 两眼中心本来就落在画布正中（head_ox 就是为这件事解的），所以 x 直接取 120。
    # 嘴的纵向也不照抄原画：原画里 chibi 的嘴紧贴在鼻尖下面，缩到 162px 的脸上
    # 就变成「眼睛下面 7 个像素处一个小洞」，读起来像第三个鼻孔。
    # 按「眼睛中心 → 下巴尖」这条线取比例放，嘴才落在该在的位置：
    # 太靠上会撞鼻子，太靠下会掉到脖子上，0.52 是五个角色都不出错的位置。
    eye_cy_log = ly(eye_cy)
    chin_log = ly(hy1)
    mouth_y = eye_cy_log + (chin_log - eye_cy_log) * MOUTH_DY
    mo = (240.0 / 2.0, mouth_y, MOUTH_W_TARGET)
    bb = None
    if blush:
        (bc1, bc2, by0, by1) = blush
        bb = (lx(bc1), lx(bc2), ly(by0), ly(by1))
    return {
        "title": title or name,
        "head_src": os.path.join(CAST_SRC, src),
        "head_crop": head_crop,
        "head_k": k,
        "head_ox": head_ox,
        "head_oy": head_oy,
        "body_src": os.path.join(CAST_SRC, src),
        "body_crop": (0, hy1, size[0], size[1]),
        # 人物中轴（原图 x）：头和身体必须按同一根轴对齐，否则「头在正中、
        # 身子歪一边」。眼睛是唯一可靠的锚，立绘里抱道具的手臂会把轮廓推偏。
        "body_axis": (eye_l[0] + eye_r[0]) / 2.0,
        "eye_l": el,
        "eye_r": er,
        "mouth": mo,
        # 原图像素份的坐标单独留一份。上面 eye_*/mouth 是 _mk() 换算过的画布逻辑
        # 坐标，抠立绘贴片时要的是原图坐标，靠反算既绕又容易把「全宽/半宽」搞混
        # （嘴的 half_w 就在这里被目标宽覆盖掉了）。两个坐标系并存，谁用哪个写清楚。
        "src_eye_l": tuple(eye_l),
        "src_eye_r": tuple(eye_r),
        "src_mouth": tuple(mouth),
        "blush_box": bb,
        "colors": colors,
        "hair": hair,
        "hair_loose": hair_loose or hair,
        "brow": brow,
        "bangs": bangs,
        "face": face,
    }


# 五个角色的配色按各自立绘取色：瞳色是角色辨识度的核心，不能统一
_C_PAINTER = {"skin": (252, 227, 215, 255), "skin_sh": (240, 200, 190, 255),
              "line": (58, 44, 68, 255), "iris_top": (34, 62, 132, 255),
              "iris_bot": (238, 156, 186, 255), "mouth": (226, 112, 126, 255),
              "tongue": (255, 142, 152, 255), "blush": (250, 158, 178, 255)}
_C_HOOP = {"skin": (252, 238, 232, 255), "skin_sh": (238, 210, 205, 255),
           "line": (70, 56, 60, 255), "iris_top": (36, 96, 176, 255),
           "iris_bot": (150, 214, 246, 255), "mouth": (222, 118, 132, 255),
           "tongue": (250, 148, 158, 255), "blush": (250, 172, 184, 255)}
_C_PHONE = {"skin": (247, 226, 218, 255), "skin_sh": (232, 198, 192, 255),
            "line": (48, 38, 42, 255), "iris_top": (150, 26, 34, 255),
            "iris_bot": (232, 96, 92, 255), "mouth": (204, 106, 118, 255),
            "tongue": (238, 130, 140, 255), "blush": (244, 156, 168, 255)}
_C_APPLE = {"skin": (250, 229, 219, 255), "skin_sh": (236, 200, 190, 255),
            "line": (62, 46, 44, 255), "iris_top": (62, 66, 132, 255),
            "iris_bot": (176, 146, 214, 255), "mouth": (220, 116, 128, 255),
            "tongue": (248, 146, 156, 255), "blush": (248, 164, 176, 255)}
_C_BOOK = {"skin": (248, 234, 224, 255), "skin_sh": (234, 206, 198, 255),
           "line": (44, 42, 46, 255), "iris_top": (72, 128, 34, 255),
           "iris_bot": (188, 224, 96, 255), "mouth": (196, 132, 128, 255),
           "tongue": (238, 146, 146, 255), "blush": (244, 170, 172, 255)}

# 男性角色的五官基准：大肥鱼那套是少女脸（圆眼、虹膜顶满眼白、外眼角翘睫毛、
# 重腮红），套过来就是“女孩子换了个发型”。气质靠这几项拉平：眼高压到 0.88、
# 虹膜不顶满、不要翘睫毛、眉粗而平、腮红几乎不画。
_FACE_MALE = {
    "ry_mul": 0.92,
    "iris_w": 0.84,
    "iris_h": 1.00,
    "lash_w": 2.6,
    "lash_tip": False,
    "lid_w": 2.8,
    "brow_w": 3.4,
    "brow_len": 7.6,
    "brow_flat": 0.58,
    "blush": 0.42,
    "pupil": 1.06,
    "highlight": 0.70,
}

CASTS = {
    # ---- 紫发画家：画笔 + 调色板，紫罗兰卷发，粉蓝渐变瞳 ----
    "painter": _mk(
        "painter", "紫苑", "painter_base.png", (488, 635),
        head_crop=(0, 5, 488),
        eye_l=(192, 274, 40, 26), eye_r=(296, 271, 34, 26.5),
        mouth=(235, 315, 17),
        colors=_C_PAINTER,
        hair=lambda r, g, b: (r - g) > 18 and (b - g) > 18 and r < 200,
        blush=(106, 159, 296, 330),
        bangs=True,   # 紫苑的卷发真的垂到左眼上沿，闭眼时要保住那块头发
        face=_FACE_MALE,
        head_w_src=371,
    ),
    # ---- 奶白发 + 亮蓝瞳，白卫衣，呼啦圈 ----
    "hoop": _mk(
        "hoop", "星野", "hoop_base.png", (502, 635),
        head_crop=(154, 0, 502),
        eye_l=(245, 289, 30, 31), eye_r=(345, 289, 30, 31),
        mouth=(277, 322, 15),
        colors=_C_HOOP,
        hair=lambda r, g, b: r < 242 and abs(r - g) < 14 and abs(g - b) < 14 and (r + g + b) > 460,
        blush=(196, 384, 296, 332),
        brow=False,   # 齐眉长鬁发盖到眼下，原画没露眉毛
        face=_FACE_MALE,
        head_w_src=329,
    ),
    # ---- 银白发 + 红瞳，黑红渐变衫，拿手机 ----
    "phone": _mk(
        "phone", "灼", "phone_base.png", (396, 635),
        head_crop=(0, 0, 396),
        eye_l=(142, 292, 37, 27), eye_r=(285, 292, 40, 27),
        mouth=(232, 318, 22),
        colors=_C_PHONE,
        hair=lambda r, g, b: r < 242 and abs(r - g) < 14 and abs(g - b) < 14 and (r + g + b) > 460,
        blush=(112, 312, 288, 324),
        face=_FACE_MALE,
        head_w_src=395,
    ),
    # ---- 棕发 + 蓝紫瞳，白衬衫 + 斜挎带 + 苹果包 ----
    "apple": _mk(
        "apple", "青苹", "apple_base.png", (377, 634),
        head_crop=(0, 0, 377),
        eye_l=(112, 277, 47, 32), eye_r=(242, 275, 47, 30),
        mouth=(182, 332, 17),
        colors=_C_APPLE,
        hair=lambda r, g, b: r < 200 and r > b and (r - b) > 18 and (r - g) > 6,
        blush=(96, 306, 282, 316),
        face=_FACE_MALE,
        head_w_src=338,
    ),
    # ---- 黑发 + 绿瞳，灰西装马甲，抱蓝本子 ----
    "book": _mk(
        "book", "墨言", "book_base.png", (386, 633),
        head_crop=(0, 0, 386),
        eye_l=(105, 287, 50, 32), eye_r=(240, 285, 50, 30),
        mouth=(160, 345, 15),
        colors=_C_BOOK,
        hair=lambda r, g, b: (r + g + b) < 320,
        blush=(80, 296, 300, 320),
        brow=False,   # 同上：黑发盖到眼下
        face=_FACE_MALE,
        head_w_src=355,
    ),
}


def get(key):
    if key not in CASTS:
        raise KeyError("unknown cast %r, known: %s" % (key, ",".join(CASTS)))
    return CASTS[key]


def keys():
    return list(CASTS)
