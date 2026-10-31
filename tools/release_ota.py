# -*- coding: utf-8 -*-
"""发版：把 build/xiaozhi.bin 挂到 OTA 清单上，并清 jsDelivr 缓存。

用法：
    py -3 tools/release_ota.py            # 只生成清单与 bin
    py -3 tools/release_ota.py --purge    # 生成后再清 CDN 缓存

版本号取自 CMakeLists.txt 的 PROJECT_VER，必须与 bin 内固件版本一致，
否则设备会把它当成「永远有新版」反复刷机。
"""
import hashlib
import io
import json
import os
import re
import shutil
import sys
import urllib.request

REPO = "geek-alpha/xiaozhi-esp32-music"
BRANCH = "main"
CDN = "https://cdn.jsdelivr.net/gh/%s@%s/" % (REPO, BRANCH)
MANIFEST = "ota/xiaozhi-music-ota.json"


def project_version() -> str:
    text = io.open("CMakeLists.txt", encoding="utf-8").read()
    m = re.search(r'set\(PROJECT_VER\s+"([^"]+)"\)', text)
    if not m:
        raise SystemExit("CMakeLists.txt 里没找到 PROJECT_VER")
    return m.group(1)


def main() -> int:
    version = project_version()
    build_bin = os.path.join("build", "xiaozhi.bin")
    if not os.path.exists(build_bin):
        raise SystemExit("缺少 %s，先跑 idf.py build" % build_bin)

    digest = hashlib.sha256(open(build_bin, "rb").read()).hexdigest()[:8]
    name = "xiaozhi-music-app-ota-%s-%s.bin" % (version, digest)
    dst = os.path.join("ota", name)
    shutil.copyfile(build_bin, dst)

    for old in os.listdir("ota"):
        if old.startswith("xiaozhi-music-app-ota-%s-" % version) and old != name:
            os.remove(os.path.join("ota", old))
            print("清理旧包 %s" % old)

    manifest = {"firmware": {"version": version, "url": CDN + dst.replace("\\", "/"), "force": 0}}
    io.open(MANIFEST, "w", encoding="utf-8", newline="\n").write(
        json.dumps(manifest, indent=2, ensure_ascii=False) + "\n")
    print("版本 %s -> %s (%d 字节)" % (version, dst, os.path.getsize(dst)))
    print("清单 %s" % MANIFEST)

    if "--purge" in sys.argv:
        url = "https://purge.jsdelivr.net/gh/%s@%s/%s" % (REPO, BRANCH, MANIFEST)
        try:
            with urllib.request.urlopen(url, timeout=30) as r:
                print("purge %s -> %s" % (MANIFEST, r.status))
        except Exception as e:
            print("purge 失败（清单可能仍有 12h 缓存）: %s" % e)
    return 0


if __name__ == "__main__":
    sys.exit(main())
