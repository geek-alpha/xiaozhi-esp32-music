# -*- coding: utf-8 -*-
"""一个角色一个固件包。

assets 分区现在装得下 6 个角色，但一台机器就是一只人：把单个角色打成独立的
assets 分区映像，配上同一份 app / bootloader / 分区表，合成能直接烧 0x0 的
整机镜像。烧完开机就是那只角色，不需要再用 MCP 换。

产物（跑一次全出）：
    ota/casts/assets-<key>.bin                        只烧 0xb00000，换角色不动 app
    ota/casts/xiaozhi-music-v<ver>-<key>-full.bin      整机烧 0x0，开机即该角色
    ota/casts/SHA256SUMS-<ver>.txt

app 一份就够：mk_assets_bin.py 生成的几何表含全部角色，固件按 key 找几何，
单角色分区的 slot 0 是谁都能对上。
"""
import hashlib
import io
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import make_full_bin as FULL  # noqa: E402
import mk_assets_bin as MK    # noqa: E402

OUT_DIR = os.path.join(ROOT, "ota", "casts")
ASSETS_MAGIC = b"AST1"


def single_assets(key):
    """把 key 这一个角色打成 assets 映像，返回路径。"""
    dst = os.path.join(ROOT, "build", "assets-%s.bin" % key)
    cmd = [sys.executable, os.path.join(HERE, "mk_assets_bin.py"), dst,
           "--casts", key, "--no-geom"]
    r = subprocess.run(cmd, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    out = r.stdout.decode("utf-8", "replace")
    if r.returncode != 0:
        raise SystemExit("打包 %s 失败:\n%s" % (key, out))
    for line in out.splitlines():
        if "assets.bin" in line:
            print("  " + line.strip())
    return dst


def merge(segments, assets_path):
    """按 flash_args 拼整机镜像，assets 段换成这一个角色的。"""
    chunks = []
    total = 0
    for offset, rel in segments:
        src = assets_path if os.path.basename(rel) == "assets.bin" else os.path.join("build", rel)
        if not os.path.exists(src):
            raise SystemExit("缺少分段 %s" % src)
        blob = io.open(src, "rb").read()
        chunks.append((offset, blob))
        total = max(total, offset + len(blob))
    image = bytearray(b"\xff" * total)
    for offset, blob in chunks:
        image[offset:offset + len(blob)] = blob
    # 自检：分区表 / app 头 / assets magic 三处任一不对，说明拼错了段，别拿出去烧
    if image[0x8000:0x8002] != b"\xaa\x50":
        raise SystemExit("自检失败：0x8000 不是分区表 magic")
    if image[0x100000] != 0xE9:
        raise SystemExit("自检失败：0x100000 不是 app 镜像头 0xE9")
    apos = next((off for off, rel in segments if os.path.basename(rel) == "assets.bin"), None)
    if apos is None:
        raise SystemExit("flash_args 里没有 assets 段，无从替换角色")
    if image[apos:apos + 4] != ASSETS_MAGIC:
        raise SystemExit("自检失败：0x%x 不是 assets magic（烧了也没角色）" % apos)
    return bytes(image), apos


def main(argv):
    keys = [k for k in argv[1:] if not k.startswith("-")] or list(MK.CASTS)
    bad = [k for k in keys if k not in MK.CASTS]
    if bad:
        raise SystemExit("不认识的角色 %s（可选：%s）" % (",".join(bad), ",".join(MK.CASTS)))

    version = FULL.project_version()
    args_path = os.path.join("build", "flash_args")
    if not os.path.exists(os.path.join(ROOT, args_path)):
        raise SystemExit("缺少 build/flash_args，先跑 idf.py build")
    _kwargs, segments = FULL.parse_flash_args(os.path.join(ROOT, args_path))

    os.makedirs(OUT_DIR, exist_ok=True)
    sums = []
    for key in keys:
        print("[%s]" % key)
        assets = single_assets(key)
        image, apos = merge(segments, assets)
        adst = os.path.join(OUT_DIR, "assets-%s.bin" % key)
        blob = io.open(assets, "rb").read()
        io.open(adst, "wb").write(blob)
        fname = "xiaozhi-music-v%s-%s-full.bin" % (version, key)
        fdst = os.path.join(OUT_DIR, fname)
        io.open(fdst, "wb").write(image)
        print("  %-44s %8.2f MB  烧 0x%x" % ("assets-%s.bin" % key, len(blob) / 1048576.0, apos))
        print("  %-44s %8.2f MB  烧 0x0" % (fname, len(image) / 1048576.0))
        sums.append((hashlib.sha256(image).hexdigest(), fname))
        sums.append((hashlib.sha256(blob).hexdigest(), "assets-%s.bin" % key))

    app = os.path.join(OUT_DIR, "xiaozhi-music-v%s-app-ota.bin" % version)
    if os.path.exists(app):
        sums.append((hashlib.sha256(io.open(app, "rb").read()).hexdigest(), os.path.basename(app)))

    spath = os.path.join(OUT_DIR, "SHA256SUMS-%s.txt" % version)
    with io.open(spath, "w", encoding="utf-8", newline="\n") as f:
        for h, name in sums:
            f.write("%s  %s\n" % (h, name))
    print("-" * 60)
    print("%d 个角色 -> %s（清单 %s）" % (len(keys), os.path.relpath(OUT_DIR, ROOT),
                                       os.path.basename(spath)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
