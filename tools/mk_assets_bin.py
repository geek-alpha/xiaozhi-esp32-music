# -*- coding: utf-8 -*-
"""把各角色打好的 slot 合成一张 assets 分区映像，并导出各角色的几何表。

分区表里 assets 是独立分区，OTA 不动它；换/加角色只需要重烧这一个分区，
不用整机重刷。slot 之间 64KB 对齐，就是为了单角色重烧只碰自己那一段。

几何表是这次修「换了角色眨眼/眼神贴到脸中间」的产物：各角色眼位不同，
眨眼贴片从 34 起（大肥鱼）到 32/38/50/52/56 都有、宽 54~78 不等，视线贴片
GAZE_OX 也从 38 到 60。这些值原本只有大肥鱼那一份宏，别的角色只能用错。
"""
import io
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT = os.path.join(ROOT, "_emoji_out")
FW = os.path.join(ROOT, "main", "display")

sys.path.insert(0, HERE)
import _pack_assets as PK  # noqa: E402

# 顺序即 slot 顺序；大肥鱼排第一，固件默认起它
CASTS = ["base", "hoop", "painter", "phone", "apple", "book"]

# 与 emoji_geom_t 的字段顺序一一对应，C 侧照这个顺序初始化
GEOM_FIELDS = ["EMOJI_W", "EMOJI_H",
               "MOUTH_W", "MOUTH_H", "MOUTH_OX", "MOUTH_OY",
               "BLINK_W", "BLINK_H", "BLINK_OX", "BLINK_OY",
               "GAZE_COUNT", "GAZE_W", "GAZE_H", "GAZE_OX", "GAZE_OY",
               # 身体/投影尺寸和落点也跟角色走：下巴在哪，身体就从哪开始接。
               # 写成全局宏只能对准一个人（大肥鱼下巴 y=100，其余五个都是 127）。
               "EMOJI_BODY_W", "EMOJI_BODY_H", "EMOJI_BODY_OX", "EMOJI_BODY_OY",
               "EMOJI_SHADOW_W", "EMOJI_SHADOW_H", "EMOJI_TOTAL_H"]


def cast_dir(key):
    return os.path.join(OUT, "_base" if key == "base" else key)


def slot_path(key):
    return os.path.join(cast_dir(key), "_pack", "_base.slot" if key == "base" else key + ".slot")


def read_geom(key):
    """读该角色 --pack 时导出的宏。缺字段说明骨架是新旧版本混搭，直接报错别猜。"""
    p = os.path.join(cast_dir(key), "_pack", "emoji_assets.h")
    if not os.path.exists(p):
        raise SystemExit("缺少 %s 的骨架头 %s（先跑 gen_emoji_assets.py --cast %s --pack）" % (key, p, key))
    src = io.open(p, encoding="utf-8").read()
    vals = dict((m.group(1), int(m.group(2)))
                for m in re.finditer(r"^#define\s+(\w+)\s+(-?\d+)\s*$", src, re.M))
    miss = [f for f in GEOM_FIELDS if f not in vals]
    if miss:
        raise SystemExit("%s 的骨架头缺字段 %s" % (key, ",".join(miss)))
    return [vals[f] for f in GEOM_FIELDS]


GEOM_H = """// 由 tools/mk_assets_bin.py 生成，勿手改
//
// 每个角色的贴片几何。脸图统一 162x162，但眼位/嘴位各不相同：
// 眨眼贴片宽 54~78、x 起点 32~56，视线贴片 x 起点 38~60。
// 固件按当前绑定的角色取这一份，不再用大肥鱼那套全局宏。
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *key;
    int16_t emoji_w, emoji_h;
    int16_t mouth_w, mouth_h, mouth_ox, mouth_oy;
    int16_t blink_w, blink_h, blink_ox, blink_oy;
    int16_t gaze_count, gaze_w, gaze_h, gaze_ox, gaze_oy;
    int16_t body_w, body_h, body_ox, body_oy;
    int16_t shadow_w, shadow_h, total_h;
} emoji_geom_t;

// 顺序与 assets 分区 slot 表一致（同一个 CASTS 列表产出），下标可直接当 slot 号
extern const emoji_geom_t emoji_geom_table[];
extern const int emoji_geom_count;

// 按 key 找几何。一个角色一个固件包时，分区 slot 0 是谁都不定，
// 按下标取会拿别人的眼位画眨眼，只能按名字查。找不到返回 NULL。
const emoji_geom_t *emoji_geom_find(const char *key);

#ifdef __cplusplus
}
#endif
"""


def write_geom(rows):
    hdr = os.path.join(FW, "emoji_geom.h")
    src = os.path.join(FW, "emoji_geom.c")
    with io.open(hdr, "w", encoding="utf-8", newline="\n") as f:
        f.write(GEOM_H)
    L = ["// 由 tools/mk_assets_bin.py 生成，勿手改", '#include "emoji_geom.h"',
         "#include <string.h>", "",
         "const emoji_geom_t emoji_geom_table[] = {"]
    for key, v in rows:
        L.append('    {"%s", %s},' % (key, ", ".join(str(x) for x in v)))
    L.append("};")
    L.append("const int emoji_geom_count = sizeof(emoji_geom_table) / sizeof(emoji_geom_table[0]);")
    L.append("")
    L.append("const emoji_geom_t *emoji_geom_find(const char *key)")
    L.append("{")
    L.append("    if (key == NULL) {")
    L.append("        return NULL;")
    L.append("    }")
    L.append("    for (int i = 0; i < emoji_geom_count; i++) {")
    L.append("        if (strcmp(emoji_geom_table[i].key, key) == 0) {")
    L.append("            return &emoji_geom_table[i];")
    L.append("        }")
    L.append("    }")
    L.append("    return NULL;")
    L.append("}")
    with io.open(src, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(L) + "\n")
    print("几何表 %d 个角色 -> %s" % (len(rows), os.path.relpath(src, ROOT)))
    return rows


def parse_args(argv):
    """[dst] [--casts a,b] [--no-geom]

    默认：目标 build/assets.bin、打全部角色、重写几何表。
    --casts 只挑几个角色打进这一个 bin（每角色单独一个固件包用），
    几何表照样写全部角色——一份 app 要能配任何单角色包。
    """
    dst = os.path.join(ROOT, "build", "assets.bin")
    keys = list(CASTS)
    write_geometry = True
    i = 1
    while i < len(argv):
        a = argv[i]
        if a == "--casts":
            i += 1
            if i >= len(argv):
                raise SystemExit("--casts 后面要跟角色 key，如 base,hoop")
            keys = [k.strip() for k in argv[i].split(",") if k.strip()]
            bad = [k for k in keys if k not in CASTS]
            if bad:
                raise SystemExit("不认识的角色 %s（可选：%s）" % (",".join(bad), ",".join(CASTS)))
        elif a == "--no-geom":
            write_geometry = False
        else:
            dst = a
        i += 1
    return dst, keys, write_geometry


def main():
    dst, keys, write_geometry = parse_args(sys.argv)
    casts = []
    for key in keys:
        p = slot_path(key)
        if not os.path.exists(p):
            print("跳过 %s（没有 slot: %s）" % (key, p))
            continue
        payload = io.open(p, "rb").read()
        n_dsc, dsc_off, n_face, face_off = struct.unpack_from("<IIII", payload, 0)
        faces = []
        for j in range(n_face):
            f = struct.unpack_from("<hhhhHHHHHH", payload, face_off + j * 20)
            faces.append(f[:9])
        blobs = [None] * n_dsc
        for i in range(n_dsc):
            off, w, h, kind, _a, _b, _c, size = struct.unpack_from("<IHHBBBBI", payload, dsc_off + i * 16)
            blobs[i] = ("#%d" % i, kind, w, h, payload[off:off + size])
        casts.append((key, blobs, faces))
        print("%-8s %7d B  dsc=%-3d face=%d" % (key, len(payload), n_dsc, n_face))
    if not casts:
        print("没有任何 slot，什么都没做")
        return 1
    img = PK.build_pack(casts)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    with io.open(dst, "wb") as f:
        f.write(img)
    if write_geometry:
        write_geom([(key, read_geom(key)) for key in CASTS if os.path.exists(slot_path(key))])
    print("-" * 56)
    print("assets.bin %d B (%.2f MB)  %d 个角色 -> %s" % (len(img), len(img) / 1048576.0, len(casts), dst))
    return 0


if __name__ == "__main__":
    sys.exit(main())
