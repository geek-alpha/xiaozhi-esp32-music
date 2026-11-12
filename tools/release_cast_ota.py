# -*- coding: utf-8 -*-
"""多角色线 OTA 清单：写 ota/multi-cast-ota.json，并把 build/xiaozhi.bin 拷成 app-ota 资产。

与主线 tools/release_ota.py 分开：主线写 ota/xiaozhi-music-ota.json（main 分支），
这条线写 ota/multi-cast-ota.json（multi-cast 分支）。两份清单挂在两个分支上，
各自的 CONFIG_OTA_URL 只读自己那份，误 merge 也不会互相顶掉。

用法：
    py -3 tools/release_cast_ota.py
"""
import hashlib
import io
import json
import os
import re
import shutil
import sys

REPO = "geek-alpha/xiaozhi-esp32-music"
BRANCH = "multi-cast"
RELEASE_BASE = "https://gh-proxy.com/https://github.com/%s/releases/download" % REPO
MANIFEST = os.path.join("ota", "multi-cast-ota.json")
OUT_DIR = os.path.join("ota", "casts")


def project_version() -> str:
    text = io.open("CMakeLists.txt", encoding="utf-8").read()
    m = re.search(r'set\(PROJECT_VER\s+"([^"]+)"\)', text)
    if not m:
        raise SystemExit("CMakeLists.txt 里没找到 PROJECT_VER")
    return m.group(1)


def main() -> int:
    os.chdir(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    version = project_version()
    build_bin = os.path.join("build", "xiaozhi.bin")
    if not os.path.exists(build_bin):
        raise SystemExit("缺少 %s，先跑 idf.py build" % build_bin)
    if not os.path.isdir(OUT_DIR):
        os.makedirs(OUT_DIR)

    name = "xiaozhi-music-v%s-app-ota.bin" % version
    dst = os.path.join(OUT_DIR, name)
    shutil.copyfile(build_bin, dst)
    digest = hashlib.sha256(open(build_bin, "rb").read()).hexdigest()[:8]

    tag = "multi-cast-v%s" % version

    manifest = {"firmware": {"version": version,
                             "url": "%s/%s/%s" % (RELEASE_BASE, tag, name),
                             "force": 0}}
    io.open(MANIFEST, "w", encoding="utf-8", newline="\n").write(
        json.dumps(manifest, indent=2, ensure_ascii=False) + "\n")
    fixed = refresh_sums(version, name)
    print("版本 %s -> %s (sha256[:8]=%s, %d 字节)" % (version, dst, digest, os.path.getsize(dst)))
    print("清单 %s（设备只从 %s 分支拉它）" % (MANIFEST, BRANCH))
    print("发布资产名 %s（release tag %s，SHA256SUMS 已%s）"
          % (name, tag, "校正" if fixed else "无对应行可校正"))
    return 0


def refresh_sums(version, name):
    """把 app-ota 那份 sha256 校正进 SHA256SUMS。

    这一步不能省：mk_cast_bins 写 SHA256SUMS 的时候 app-ota 还是上一版的，
    本脚本随后才重新生成它，于是清单里那行校验值是旧的——对着 SHA 核验会永远失败。
    全值就地重算：清单里其余行都是 64 位，混进一个 8 位短 hash 会让 sha256sum -c 判格式错。
    """
    path = os.path.join(OUT_DIR, "SHA256SUMS-%s.txt" % version)
    if not os.path.exists(path):
        return False
    full = hashlib.sha256(open(os.path.join(OUT_DIR, name), "rb").read()).hexdigest()
    lines = io.open(path, encoding="utf-8").read().splitlines()
    out, hit = [], False
    for ln in lines:
        if ln.endswith("  " + name):
            out.append("%s  %s" % (full, name))
            hit = True
        else:
            out.append(ln)
    if not hit:
        out.append("%s  %s" % (full, name))
    io.open(path, "w", encoding="utf-8", newline="\n").write("\n".join(out) + "\n")
    return hit


if __name__ == "__main__":
    sys.exit(main())
