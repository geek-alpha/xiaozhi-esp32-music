# -*- coding: utf-8 -*-
"""发版：把 build/xiaozhi.bin 挂到 OTA 清单上。

用法：
    py -3 tools/release_ota.py            # 生成清单与本地留档 bin

清单里的 url 指向 GitHub Release 资产（经 ghproxy 加速），所以要先手工建好
对应 tag 的 release 并上传同名 bin，否则设备会 404。

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
RELEASE_BASE = "https://ghproxy.net/https://github.com/%s/releases/download" % REPO
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
    name = "xiaozhi-music-v%s-app-ota.bin" % version
    dst = os.path.join("ota", name)
    shutil.copyfile(build_bin, dst)

    for old in os.listdir("ota"):
        if old.startswith("xiaozhi-music-app-ota-%s-" % version) and old != name:
            os.remove(os.path.join("ota", old))
            print("清理旧包 %s" % old)

    manifest = {"firmware": {"version": version,
                            "url": "%s/v%s/%s" % (RELEASE_BASE, version, name),
                            "force": 0}}
    io.open(MANIFEST, "w", encoding="utf-8", newline="\n").write(
        json.dumps(manifest, indent=2, ensure_ascii=False) + "\n")
    print("版本 %s -> %s (sha256[:8]=%s, %d 字节)" % (version, dst, digest, os.path.getsize(dst)))
    print("清单 %s" % MANIFEST)
    print("上传资产名应为 %s（release tag v%s）" % (name, version))
    return 0


if __name__ == "__main__":
    sys.exit(main())
