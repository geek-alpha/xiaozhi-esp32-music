# -*- coding: utf-8 -*-
"""端到端验 OTA 链路：拉设备真正会读的清单 URL，再按清单里的地址下载 app，
比对远端 sha256 与本地产物。设备侧出问题（清单没推、资产没传、URL 指错）只有这里能抓到。"""
import hashlib
import io
import json
import os
import subprocess
import sys
import tempfile
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MANIFEST = ("https://gh-proxy.com/https://raw.githubusercontent.com/"
            "geek-alpha/xiaozhi-esp32-music/multi-cast/ota/multi-cast-ota.json")


def get(url, timeout=300, use_curl=False):
    if use_curl:
        # urllib 读 gh-proxy 的 chunked 4MB 会卡在 read 上超时，curl 同一地址 22s 就完了
        tmp = os.path.join(tempfile.gettempdir(), "ota_probe.bin")
        subprocess.run(["curl.exe", "-sSL", "--max-time", str(timeout), "--retry", "2",
                        "-o", tmp, url], check=True)
        blob = io.open(tmp, "rb").read()
        os.remove(tmp)
        return blob
    req = urllib.request.Request(url, headers={"User-Agent": "phoenix-ota-check"})
    return urllib.request.urlopen(req, timeout=timeout).read()


def main():
    os.chdir(ROOT)
    text = get(MANIFEST, 120).decode("utf-8")
    print("清单原文：", text.strip().replace("\n", " ")[:400])
    fw = json.loads(text)["firmware"]
    print("清单 version =", fw["version"])
    blob = get(fw["url"], timeout=500, use_curl=True)
    remote = hashlib.sha256(blob).hexdigest()
    local_path = os.path.join("ota", "casts", "xiaozhi-music-v%s-app-ota.bin" % fw["version"])
    if not os.path.exists(local_path):
        print("本地缺 %s" % local_path)
        return 1
    local = hashlib.sha256(io.open(local_path, "rb").read()).hexdigest()
    print("远端(清单 URL) %d B sha256=%s" % (len(blob), remote[:16]))
    print("本地 %d B sha256=%s" % (os.path.getsize(local_path), local[:16]))
    if remote != local:
        print("RESULT MISMATCH")
        return 1
    print("RESULT OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
