# -*- coding: utf-8 -*-
"""给 LVGL 9 软件渲染器补上「文字描边」。

managed_components/ 在 .gitignore 里，组件重装或换机器时手改的源码会丢，所以把改动
固化成这个脚本，随时能重打。

LVGL 9.2 没有 text outline 样式属性（lv_obj_set_style_text_outline 不存在），描边只能
落在字形混合处：把同一张字形 alpha 掩模往 8 个方向各偏移 1px 叠一遍，再画正体。边色按
字色亮度自动取，浅字配深边、深字配白边，light/dark 两套主题都不用改应用层代码。

用法:
  python tools/patch_lvgl_text_outline.py           # 打补丁（已打过就跳过）
  python tools/patch_lvgl_text_outline.py --check    # 只看状态，不动文件
  python tools/patch_lvgl_text_outline.py --revert   # 还原成上游原样
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
TARGET = os.path.join(ROOT, "managed_components", "lvgl__lvgl", "src", "draw", "sw",
                      "lv_draw_sw_letter.c")

VARS_OLD = """/**********************
 *  STATIC VARIABLES
 **********************/

/**********************
 *  GLOBAL VARIABLES"""

VARS_NEW = """/**********************
 *  STATIC VARIABLES
 **********************/
/* 文字描边：LVGL 9 没有原生描边样式，在字形正体之前拿同一张 alpha 掩模往 8 个方向
 * 各叠一次，得到 1px 实边。颜色按字色亮度自动取，浅字配深边、深字配白边，两套主题都不糊 */
static int32_t glyph_outline_width = 1;
static const int8_t glyph_outline_dirs[8][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}, {-1, -1}, {1, -1}, {-1, 1}, {1, 1}};

/**********************
 *  GLOBAL VARIABLES"""

DRAW_OLD = """                    blend_dsc.mask_res = LV_DRAW_SW_MASK_RES_CHANGED;

                    lv_draw_sw_blend(draw_unit, &blend_dsc);"""

DRAW_NEW = """                    blend_dsc.mask_res = LV_DRAW_SW_MASK_RES_CHANGED;

                    if(glyph_outline_width > 0) {
                        lv_draw_sw_blend_dsc_t outline_dsc = blend_dsc;
                        outline_dsc.color = lv_color_luminance(glyph_draw_dsc->color) >= 140 ? lv_color_hex(0x101010)
                                                                                         : lv_color_hex(0xFFFFFF);
                        for(uint32_t dir = 0; dir < 8; dir++) {
                            int32_t dx = (int32_t)glyph_outline_dirs[dir][0] * glyph_outline_width;
                            int32_t dy = (int32_t)glyph_outline_dirs[dir][1] * glyph_outline_width;
                            lv_area_t outline_coords = *glyph_draw_dsc->letter_coords;
                            lv_area_t outline_mask = mask_area;
                            lv_area_move(&outline_coords, dx, dy);
                            lv_area_move(&outline_mask, dx, dy);
                            outline_dsc.blend_area = &outline_coords;
                            outline_dsc.mask_area = &outline_mask;
                            lv_draw_sw_blend(draw_unit, &outline_dsc);
                        }
                    }

                    lv_draw_sw_blend(draw_unit, &blend_dsc);"""

PATCHES = [(VARS_OLD, VARS_NEW), (DRAW_OLD, DRAW_NEW)]


def read():
    with open(TARGET, encoding="utf-8") as fp:
        return fp.read()


def write(text):
    with open(TARGET, "w", encoding="utf-8", newline="") as fp:
        fp.write(text)


def state(text):
    """返回 'patched' / 'clean' / 'broken'（锚点对不上，多半是 LVGL 升级换了实现）"""
    hits = sum(1 for _, new in PATCHES if new in text)
    if hits == len(PATCHES):
        return "patched"
    raws = sum(1 for old, _ in PATCHES if old in text)
    if hits == 0 and raws == len(PATCHES):
        return "clean"
    return "broken"


def apply(direction):
    text = read()
    st = state(text)
    if direction == "patch" and st == "patched":
        print("已打过补丁，跳过:", TARGET)
        return 0
    if direction == "revert" and st == "clean":
        print("本来就是原样，跳过:", TARGET)
        return 0
    pairs = PATCHES if direction == "patch" else [(new, old) for old, new in PATCHES]
    for src, dst in pairs:
        if src not in text:
            print("锚点不匹配，没动文件。LVGL 可能升级换了实现，需要人工核对:", TARGET)
            return 3
        text = text.replace(src, dst, 1)
    write(text)
    print(("已打补丁: " if direction == "patch" else "已还原: ") + TARGET)
    return 0


def main(argv):
    if not os.path.exists(TARGET):
        print("找不到目标文件，先跑一次 idf.py reconfigure 拉组件:", TARGET)
        return 2
    if "--check" in argv:
        st = state(read())
        print({"patched": "已打补丁", "clean": "原样(未打)", "broken": "锚点异常(被改过又不完整)"}[st], TARGET)
        return 0 if st == "patched" else 1
    if "--revert" in argv:
        return apply("revert")
    return apply("patch")


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
