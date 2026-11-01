# -*- coding: utf-8 -*-
"""把 build/ 下的各段拼成可烧到 0x0 的全量镜像（OTA 包不能烧 0x0）。

布局取自 build/flash_args（build 系统自己生成，不手抄），分段之间用 0xFF 填充，
与 esptool merge_bin 结果一致。产物：ota/xiaozhi-music-v<ver>-full.bin

用法：
    py -3 tools/make_full_bin.py
"""
import hashlib
import io
import os
import re
import sys

APP_DESC_MAGIC = b"\x32\x54\xcd\xab"
PT_MAGIC = b"\xaa\x50"


def project_version() -> str:
    text = io.open("CMakeLists.txt", encoding="utf-8").read()
    m = re.search(r'set\(PROJECT_VER\s+"([^"]+)"\)', text)
    if not m:
        raise SystemExit("CMakeLists.txt 里没找到 PROJECT_VER")
    return m.group(1)


def parse_flash_args(path):
    """返回 (flash_kwargs, [(offset, 文件路径), ...])，路径相对 build/。"""
    segments = []
    kwargs = {}
    for line in io.open(path, encoding="utf-8"):
        line = line.strip()
        if not line:
            continue
        if line.startswith("--"):
            for key, val in re.findall(r"--([\w-]+)\s+(\S+)", line):
                kwargs[key] = val
            continue
        parts = line.split(None, 1)
        if len(parts) != 2:
            raise SystemExit("flash_args 行无法解析: %r" % line)
        segments.append((int(parts[0], 16), parts[1].strip()))
    if not segments:
        raise SystemExit("flash_args 里没有分段")
    return kwargs, sorted(segments)


def main() -> int:
    version = project_version()
    build = "build"
    args_path = os.path.join(build, "flash_args")
    if not os.path.exists(args_path):
        raise SystemExit("缺少 %s，先跑 idf.py build" % args_path)

    kwargs, segments = parse_flash_args(args_path)

    chunks = []
    total = 0
    for offset, rel in segments:
        src = os.path.join(build, rel)
        if not os.path.exists(src):
            raise SystemExit("缺少分段 %s" % src)
        blob = open(src, "rb").read()
        chunks.append((offset, blob))
        total = max(total, offset + len(blob))

    image = bytearray(b"\xff" * total)
    for offset, blob in chunks:
        image[offset:offset + len(blob)] = blob

    if image[0x8000:0x8002] != PT_MAGIC:
        raise SystemExit("自检失败：0x8000 不是分区表 magic")
    if image[0x100000] != 0xE9:
        raise SystemExit("自检失败：0x100000 不是 app 镜像头 0xE9")
    desc = image.find(APP_DESC_MAGIC, 0x100000, 0x100000 + 0x100)
    if desc < 0:
        raise SystemExit("自检失败：app 段找不到 app_desc magic")
    bin_ver = bytes(image[desc + 0x10:desc + 0x30]).split(b"\x00")[0].decode("utf-8", "replace")
    if bin_ver != version:
        raise SystemExit("自检失败：app_desc 版本 %r != PROJECT_VER %r" % (bin_ver, version))

    os.makedirs("ota", exist_ok=True)
    name = "xiaozhi-music-v%s-full.bin" % version
    dst = os.path.join("ota", name)
    with open(dst, "wb") as f:
        f.write(image)

    digest = hashlib.sha256(image).hexdigest()
    print("全量镜像 %s (%d 字节, 内含 app 版本 %s)" % (dst, len(image), bin_ver))
    print("flash: %s" % " ".join("--%s %s" % (k, v) for k, v in sorted(kwargs.items())))
    print("sha256=%s" % digest)
    return 0


if __name__ == "__main__":
    sys.exit(main())
