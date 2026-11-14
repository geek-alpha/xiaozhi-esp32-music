"""按当前 git 分支把 sdkconfig 的 CONFIG_OTA_URL 对齐到该分支自己的清单。

sdkconfig 在 .gitignore 里，切分支不会回滚，导致在 multi-cast 分支配过的
清单地址被单人版 main 分支沿用，烧出来的单人版开机把自己 OTA 成多角色版。
编译前跑这个脚本，保证固件里写死的 OTA 地址永远属于当前分支。
"""
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BASE = "https://gh-proxy.com/https://raw.githubusercontent.com/geek-alpha/xiaozhi-esp32-music"
MANIFESTS = {
    "main": f"{BASE}/main/ota/xiaozhi-music-ota.json",
    "multi-cast": f"{BASE}/multi-cast/ota/multi-cast-ota.json",
}


def main() -> int:
    branch = subprocess.run(
        ["git", "rev-parse", "--abbrev-ref", "HEAD"],
        cwd=ROOT, capture_output=True, text=True,
    ).stdout.strip()
    url = MANIFESTS.get(branch)
    if url is None:
        print(f"[ota-url] 分支 {branch!r} 未登记清单，sdkconfig 保持原样")
        return 0

    cfg = ROOT / "sdkconfig"
    with open(cfg, "r", encoding="utf-8", newline="") as f:
        text = f.read()
    new, hits = re.subn(r"^CONFIG_OTA_URL=[^\r\n]*", f'CONFIG_OTA_URL="{url}"', text, flags=re.M)
    if hits != 1:
        print(f"[ota-url] 期望命中 1 行，实际 {hits} 行，放弃改动")
        return 1
    if new == text:
        print(f"[ota-url] {branch} 已对齐: {url}")
        return 0
    with open(cfg, "w", encoding="utf-8", newline="") as f:
        f.write(new)
    print(f"[ota-url] {branch} 改写为: {url}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
