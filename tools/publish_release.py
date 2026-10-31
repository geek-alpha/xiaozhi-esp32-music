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


def ensure_release(tok, version):
    tag = "v%s" % version
    status, rel = http("GET", "%s/repos/%s/releases/tags/%s" % (API, REPO, tag), tok, timeout=60)
    if status == 200 and rel:
        print("复用已存在的 release %s (id=%s)" % (tag, rel["id"]))
        return rel
    status, rel = http("POST", "%s/repos/%s/releases" % (API, REPO), tok,
                       {"tag_name": tag, "name": tag,
                        "body": "小智音乐 %s\n\nOTA 清单经 gh-proxy.com 加速，设备端直连升级。" % tag,
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

    digest = hashlib.sha256(open(bin_path, "rb").read()).hexdigest()
    sums = os.path.join("ota", "SHA256SUMS-%s.txt" % version)
    io.open(sums, "w", encoding="utf-8", newline="\n").write(
        "%s  %s\n" % (digest, os.path.basename(bin_path)))

    tok = token()
    rel = ensure_release(tok, version)
    upload(tok, rel, bin_path, os.path.basename(bin_path))
    upload(tok, rel, sums, "SHA256SUMS.txt")
    print("sha256=%s" % digest)
    return 0


if __name__ == "__main__":
    sys.exit(main())
