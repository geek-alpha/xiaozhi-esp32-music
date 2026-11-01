# -*- coding: utf-8 -*-
"""把当前版本的 bin 发布到 GitHub Release（幂等：release/资产已存在则替换）。

用法：
    py -3 tools/publish_release.py

版本号取自 CMakeLists.txt 的 PROJECT_VER，资产名与 release_ota.py 生成的清单严格对应：
    tag v<ver> / xiaozhi-music-v<ver>-app-ota.bin

HTTP 走系统 curl：本机 Python 的 urllib 直连 api.github.com 会报
CERTIFICATE_VERIFY_FAILED: Missing Authority Key Identifier（系统证书链问题），curl 正常。
"""
import hashlib
import io
import json
import os
import re
import subprocess
import sys
import tempfile
import urllib.parse

REPO = "geek-alpha/xiaozhi-esp32-music"
API = "https://api.github.com"


def token() -> str:
    tok = os.environ.get("GITHUB_TOKEN") or os.environ.get("GH_TOKEN")
    if not tok:
        raise SystemExit("缺少 GITHUB_TOKEN 环境变量")
    return tok.strip()


def project_version() -> str:
    text = io.open("CMakeLists.txt", encoding="utf-8").read()
    m = re.search(r'set\(PROJECT_VER\s+"([^"]+)"\)', text)
    if not m:
        raise SystemExit("CMakeLists.txt 里没找到 PROJECT_VER")
    return m.group(1)


def http(method, url, tok, body=None, ctype="application/vnd.github+json", timeout=600):
    """返回 (status_code, 解析后的响应体)。"""
    tmp_out = os.path.join(tempfile.gettempdir(), "_gh_release_resp.json")
    args = ["curl", "-sS", "-m", str(timeout), "-o", tmp_out, "-w", "%{http_code}",
            "-X", method,
            "-H", "Authorization: Bearer %s" % tok,
            "-H", "Accept: application/vnd.github+json",
            "-H", "User-Agent: xiaozhi-release"]
    if body is not None:
        args += ["-H", "Content-Type: %s" % ctype]
        if isinstance(body, bytes):
            tmp_body = os.path.join(tempfile.gettempdir(), "_gh_release_body.bin")
            with open(tmp_body, "wb") as f:
                f.write(body)
            args += ["--data-binary", "@%s" % tmp_body]
        else:
            args += ["-d", json.dumps(body)]
    args.append(url)

    p = subprocess.run(args, capture_output=True, text=True, encoding="utf-8", errors="replace")
    code = (p.stdout or "").strip()
    if not code.isdigit():
        raise SystemExit("curl 调用失败 %s %s: %s" % (method, url, (p.stderr or "").strip()[:400]))
    try:
        payload = json.load(io.open(tmp_out, encoding="utf-8")) if os.path.getsize(tmp_out) else None
    except ValueError:
        payload = None
    return int(code), payload


def release_body(version):
    full = "xiaozhi-music-v%s-full.bin" % version
    app = "xiaozhi-music-v%s-app-ota.bin" % version
    return "\n".join([
        "## 小智音乐固件 v%s" % version,
        "",
        "目标硬件：**星智 Cube 1.54 (WiFi)**（`xingzhi-cube-1.54tft-wifi`，esp32s3，16MB flash），ESP-IDF v5.4。",
        "",
        "### 两个包，别搞混",
        "",
        "| 资产 | 起始地址 | 用途 |",
        "| --- | --- | --- |",
        "| `%s` | **0x0** | 全量镜像：bootloader + 分区表 + otadata + 语音模型 + app，新板或刷坏了的板子用这个 |" % full,
        "| `%s` | 0x100000 | 纯 app 包，只给设备端 OTA 用，**不能**烧到 0x0 |" % app,
        "",
        "> v2.0.4 只发了 app 包，按「烧到 0x0」会起不来，设备端分区表也会被写坏——这就是上一版烧不进去的根因。",
        "",
        "### 全量烧录（一条命令）",
        "",
        "```",
        "esptool.py --chip esp32s3 -p <PORT> -b 460800 write_flash 0x0 %s" % full,
        "```",
        "",
        "仓库里的 `scripts/flash.sh` 会自己找最新的 full 包，端口留空交给 esptool 自动探测。",
        "Windows 下别把 COM 号写死进脚本：板子的 USB-Serial/JTAG 口每次插拔可能换号（曾经写死 COM19、实际枚举成 COM22，esptool 直接报 port is busy）。",
        "",
        "### 校验",
        "",
        "```",
        "certutil -hashfile %s SHA256" % full,
        "```",
        "",
        "对照 `SHA256SUMS.txt`（两个包都在里面）。",
    ])


def ensure_release(tok, version, body):
    tag = "v%s" % version
    status, rel = http("GET", "%s/repos/%s/releases/tags/%s" % (API, REPO, tag), tok, timeout=60)
    if status == 200 and rel:
        print("复用已存在的 release %s (id=%s)" % (tag, rel["id"]))
        st, patched = http("PATCH", "%s/repos/%s/releases/%s" % (API, REPO, rel["id"]), tok,
                           {"body": body})
        if st == 200:
            print("更新 release 说明 (HTTP %s)" % st)
            return patched
        return rel
    status, rel = http("POST", "%s/repos/%s/releases" % (API, REPO), tok,
                       {"tag_name": tag, "name": tag, "body": body,
                        "draft": False, "prerelease": False})
    if status not in (200, 201):
        raise SystemExit("建 release 失败 HTTP %s: %s" % (status, rel))
    print("已建 release %s (id=%s)" % (tag, rel["id"]))
    return rel


def upload(tok, rel, path, name):
    for a in rel.get("assets", []):
        if a["name"] == name:
            code, _ = http("DELETE", "%s/repos/%s/releases/assets/%s" % (API, REPO, a["id"]), tok)
            print("删除旧资产 %s (HTTP %s)" % (name, code))
    blob = open(path, "rb").read()
    url = rel["upload_url"].split("{")[0] + "?name=%s" % urllib.parse.quote(name)
    status, info = http("POST", url, tok, blob, "application/octet-stream")
    if status not in (200, 201):
        raise SystemExit("上传 %s 失败 HTTP %s: %s" % (name, status, info))
    print("上传 %s (%d 字节) -> %s" % (name, len(blob), info["browser_download_url"]))


def main() -> int:
    version = project_version()
    bin_path = os.path.join("ota", "xiaozhi-music-v%s-app-ota.bin" % version)
    if not os.path.exists(bin_path):
        raise SystemExit("缺少 %s，先跑 tools/release_ota.py" % bin_path)

    subprocess.run([sys.executable, os.path.join("tools", "make_full_bin.py")], check=True)
    full_path = os.path.join("ota", "xiaozhi-music-v%s-full.bin" % version)
    if not os.path.exists(full_path):
        raise SystemExit("缺少 %s" % full_path)

    sums = os.path.join("ota", "SHA256SUMS-%s.txt" % version)
    lines = []
    for p in (full_path, bin_path):
        digest = hashlib.sha256(open(p, "rb").read()).hexdigest()
        lines.append("%s  %s\n" % (digest, os.path.basename(p)))
        print("%s %s" % (digest[:16], os.path.basename(p)))
    io.open(sums, "w", encoding="utf-8", newline="\n").write("".join(lines))

    tok = token()
    rel = ensure_release(tok, version, release_body(version))
    for p in (full_path, bin_path):
        upload(tok, rel, p, os.path.basename(p))
    upload(tok, rel, sums, "SHA256SUMS.txt")
    return 0


if __name__ == "__main__":
    sys.exit(main())
