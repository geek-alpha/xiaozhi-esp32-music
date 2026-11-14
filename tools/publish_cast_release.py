# -*- coding: utf-8 -*-
"""多角色线独立发布：tag multi-cast-v<ver>，与单角色主线 v<ver> 完全隔离。

单角色主线：tag v2.0.38，资产 xiaozhi-music-v2.0.38-full.bin / -app-ota.bin。
这条线：tag multi-cast-v3.0.0，资产 xiaozhi-music-v3.0.0-<key>-full.bin。
tag 前缀不同、版本号不同段（2.x 对 3.x），两边发布互不覆盖；
make_latest=false，主线仍是仓库的 Latest，不会被这条线顶掉。

用法：
    py -3 tools/publish_cast_release.py             # 用 ota/casts 现成产物
    py -3 tools/publish_cast_release.py --rebuild   # 先跑 mk_cast_bins.py 再发布
"""
import glob
import hashlib
import io
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import publish_release as PR  # noqa: E402

KEYS = ["base", "hoop", "painter", "phone", "apple", "book"]
OUT_DIR = os.path.join(ROOT, "ota", "casts")
TAG_PREFIX = "multi-cast-v"


def release_body(version, tag):
    rows = "\n".join("| `%s` | `xiaozhi-music-v%s-%s-full.bin` |" % (k, version, k) for k in KEYS)
    return "\n".join([
        "## 多角色版固件 %s" % tag,
        "",
        "这是**独立于单角色主线的版本**：源码在 `multi-cast` 分支，tag 带 `multi-cast-` 前缀，",
        "主线 `main`（v2.0.38 那套单角色）的开发与发布不会流进这里，这里的发布也不会覆盖主线任何资产。",
        "",
        "目标硬件：**星智 Cube 1.54 (WiFi)**（`xingzhi-cube-1.54tft-wifi`，esp32s3，16MB flash）。",
        "",
        "### 一只板子一只角色：六个整机镜像，烧 0x0",
        "",
        "| 角色 | 资产 |",
        "| --- | --- |",
        rows,
        "",
        "开机就是对应的那只角色，不用再连 MCP 换。",
        "",
        "> ⚠ **v3.0.2 必须整机烧 0x0，或同时烧 app 0x100000 + assets 0xb00000**：",
        "> 这次修复的一半落在 assets 分区（贴片顺序改成全量占位），OTA 只写 app、不覆盖 assets，",
        "> 只 OTA 会拿新 app 配旧 assets，错位照旧。本版 OTA 清单仍停在 3.0.1，不会自动升级。",
        "",
        "### 只换角色、不重烧 app：烧 0xb00000",
        "",
        "`assets-<key>.bin`（六份，key 同上表）只占 assets 分区，app / 语音模型 / 分区表一律不动：",
        "",
        "```",
        "esptool.py --chip esp32s3 -p <PORT> -b 460800 write_flash 0xb00000 assets-<key>.bin",
        "```",
        "",
        "### 整机烧录（一条命令）",
        "",
        "```",
        "esptool.py --chip esp32s3 -p <PORT> -b 460800 write_flash 0x0 xiaozhi-music-v%s-<key>-full.bin" % version,
        "```",
        "",
        "Windows 下别把 COM 号写死：USB-Serial/JTAG 口每次插拔可能换号，写死会报 port is busy。",
        "",
        "### 与主线相比改了什么",
        "",
        "- v3.0.2 修换角色后重启回默认：`SetCharacter` 成功后把 key 写进 NVS `display/cast`，开机在 `emoji_pack_ensure()` 之后按它回绑（分区里没有该 key 就删记录退回 base）",
        "- v3.0.2 修换角色后身体旁闪黑块：贴片索引与固件骨架错位（hoop 的 gaze 位置装的是 162x162 头发 A8 遮罩，被当彩色图画成黑块）。生成器改为每个表情固定占位、缺图用透明垫片顶住，顺序从此与「缺哪张」无关",
        "- v3.0.2 修眨眼越界与刘海暗影：眼睑高度改用当前角色自己的 `blink_h`（全局 BLINK_H 只等于最后打包的 apple 28），闭眼刘海遮罩跟随眼睑一起收",
        "- assets 从「单角色内嵌大表」改成按角色分槽的资产包，运行时挂载，多角色共用一份 app",
        "- 立绘修复：虹膜逐行裁进眼白（抬头不再把虹膜画到刘海）、`iris_h` 归一、头宽统一 165、嘴位与嘴线宽随脸缩放",
        "- v3.0.1 嘴位自校准：不再信手填坐标，按嘴贴片墨迹重心反算原嘴真实中心，抹嘴/贴嘴都以重心为准；"
        "五个角色的嘴统一到双眼轴 x=120、眼下 20 逻辑 y=152，不再压在下巴上",
        "- v3.0.1 底板换回原立绘：旧版底板是「五官填平版」，残留淡眼影，open/keep 表情会露出两道灰眼睛",
        "- v3.0.1 虹膜留一线眼白，强情绪表情不再像戴彩色镜片",
        "- v3.0.1 强情绪表情（惊 / 自信 / 怒 / 眨眼）的矢量眼收回原画脸型：上眼睑压到 0.86/0.74/0.64、"
        "虹膜横向填满眼白不再露一圈白、高光减半，眼周抹痕改按行取真肤色（book/apple 尤其明显）",
        "- 版本号 3.x 高于主线的 2.0.38：设备端 OTA 不会把这条线当成降级拦住",
        "",
        "### 校验",
        "",
        "```",
        "certutil -hashfile <文件> SHA256",
        "```",
        "",
        "对照 `SHA256SUMS.txt`（六个整机镜像 + 六个 assets 分区映像都在里面）。",
    ])


def collect(version):
    full, assets = [], []
    for k in KEYS:
        p = os.path.join(OUT_DIR, "xiaozhi-music-v%s-%s-full.bin" % (version, k))
        if not os.path.exists(p):
            raise SystemExit("缺少 %s，先跑 tools/mk_cast_bins.py" % p)
        full.append(p)
        a = os.path.join(OUT_DIR, "assets-%s.bin" % k)
        if not os.path.exists(a):
            raise SystemExit("缺少 %s" % a)
        assets.append(a)
    sums = os.path.join(OUT_DIR, "SHA256SUMS-%s.txt" % version)
    if not os.path.exists(sums):
        raise SystemExit("缺少 %s" % sums)
    app = os.path.join(OUT_DIR, "xiaozhi-music-v%s-app-ota.bin" % version)
    if not os.path.exists(app):
        raise SystemExit("缺少 %s，先跑 tools/release_cast_ota.py" % app)
    return full + assets + [app, sums]


def ensure_release(tok, tag, text):
    url = "%s/repos/%s/releases/tags/%s" % (PR.API, PR.REPO, tag)
    status, rel = PR.http("GET", url, tok, timeout=60)
    if status == 200 and rel:
        st, patched = PR.http("PATCH", "%s/repos/%s/releases/%s" % (PR.API, PR.REPO, rel["id"]),
                              tok, {"body": text})
        print("复用已存在的 release %s (id=%s)，说明更新 HTTP %s" % (tag, rel["id"], st))
        return patched
    status, rel = PR.http("POST", "%s/repos/%s/releases" % (PR.API, PR.REPO), tok,
                          {"tag_name": tag, "name": tag, "body": text,
                           "draft": False, "prerelease": False, "make_latest": "false"})
    if status not in (200, 201):
        raise SystemExit("建 release 失败 HTTP %s: %s" % (status, rel))
    print("已建 release %s (id=%s)" % (tag, rel["id"]))
    return rel


def main(argv):
    os.chdir(ROOT)
    version = PR.project_version()
    tag = "%s%s" % (TAG_PREFIX, version)
    if len(argv) > 1 and argv[1] == "--rebuild":
        subprocess.run([sys.executable, os.path.join("tools", "mk_cast_bins.py")], check=True)

    paths = collect(version)
    for p in paths:
        print("%s  %s" % (hashlib.sha256(io.open(p, "rb").read()).hexdigest()[:16], os.path.basename(p)))

    tok = PR.token()
    rel = ensure_release(tok, tag, release_body(version, tag))
    for p in paths:
        name = os.path.basename(p)
        if name.startswith("SHA256SUMS"):
            name = "SHA256SUMS.txt"
        PR.upload(tok, rel, p, name)
    print("完成：%s 共 %d 个资产" % (tag, len(paths)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
