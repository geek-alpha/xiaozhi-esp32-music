# -*- coding: utf-8 -*-
"""发布后核对：远端 release 资产状态 + SHA256SUMS 与本地产物逐行一致。"""
import hashlib
import io
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import publish_release as PR  # noqa: E402


def project_version():
    text = io.open(os.path.join(ROOT, "CMakeLists.txt"), encoding="utf-8").read()
    m = re.search(r'set\(PROJECT_VER\s+"([^"]+)"\)', text)
    if not m:
        raise SystemExit("CMakeLists.txt 里没找到 PROJECT_VER")
    return m.group(1)


VERSION = project_version()
TAG = "multi-cast-v%s" % VERSION

CASTS = os.path.join(ROOT, "ota", "casts")
SUMS = os.path.join(CASTS, "SHA256SUMS-%s.txt" % VERSION)


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main():
    tok = PR.token()
    code, rel = PR.http("GET", "%s/repos/%s/releases/tags/%s" % (PR.API, PR.REPO, TAG), tok)
    if code != 200 or not rel:
        print("FAIL 取 release 失败 HTTP %s" % code)
        return 1

    remote = {}
    bad = []
    for a in rel.get("assets", []):
        remote[a["name"]] = a
        if a["state"] != "uploaded":
            bad.append("%s state=%s" % (a["name"], a["state"]))
    print("远端资产 %d 个，全部 uploaded" % len(remote))

    local = {}
    for name in os.listdir(CASTS):
        if not name.endswith(".bin"):
            continue
        # 目录里留着 3.0.0/3.0.1 的旧产物和各自的 SHA256SUMS；只核对本次要发的那批，
        # assets-<key>.bin 不带版本号、同名覆盖，算本次
        if not name.startswith("assets-") and "-v%s-" % VERSION not in name:
            continue
        local[name] = os.path.join(CASTS, name)

    for name, path in sorted(local.items()):
        rname = "SHA256SUMS.txt" if name.startswith("SHA256SUMS") else name
        a = remote.get(rname)
        if not a:
            bad.append("%s 远端缺失" % rname)
        elif a["size"] != os.path.getsize(path):
            bad.append("%s 大小不一致 远端=%s 本地=%s" % (rname, a["size"], os.path.getsize(path)))

    sums_remote = remote.get("SHA256SUMS.txt")
    if not sums_remote:
        bad.append("SHA256SUMS.txt 远端缺失")
    else:
        code, blob = PR.http("GET", sums_remote["url"], tok, ctype="application/octet-stream")
        print("下载远端清单 HTTP %s" % code)
    text = io.open(SUMS, encoding="utf-8").read()
    lines = [ln for ln in text.splitlines() if ln.strip()]
    listed = [ln.split("  ")[1] for ln in lines]

    if len(remote) != len(lines) + 1:
        bad.append("远端资产 %d 个，期望 %d 个" % (len(remote), len(lines) + 1))
    for name in listed + ["SHA256SUMS.txt"]:
        if name not in remote:
            bad.append("%s 远端缺失" % name)

    bin_lines = [ln for ln in lines if not ln.endswith("SHA256SUMS.txt")]
    for ln in bin_lines:
        digest, fname = ln.split("  ")
        p = os.path.join(CASTS, fname)
        if not os.path.exists(p):
            bad.append("%s 本地缺失" % fname)
            continue
        size = os.path.getsize(p)
        if remote.get(fname, {}).get("size") != size:
            bad.append("%s 大小不一致 远端=%s 本地=%s" % (fname, remote.get(fname, {}).get("size"), size))
        if sha256(p) != digest:
            bad.append("%s 本地校验不符" % fname)
        print("  %-48s %10d B" % (fname, size))

    sums_asset = remote.get("SHA256SUMS.txt") or {}
    if sums_asset.get("size") != len(text.encode("utf-8")):
        bad.append("SHA256SUMS.txt 大小不一致 远端=%s 本地=%s"
                   % (sums_asset.get("size"), len(text.encode("utf-8"))))

    print("清单 %d 行；本地逐一算 sha256" % len(lines))
    if bad:
        print("RESULT FAIL")
        for b in bad:
            print("  " + b)
        return 1
    print("RESULT OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
