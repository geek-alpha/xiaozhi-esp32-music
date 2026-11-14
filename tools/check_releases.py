"""Dump GitHub release list for the xiaozhi-esp32-music repo (audit tags vs published releases)."""
import json
import urllib.request
from pathlib import Path

API = "https://api.github.com/repos/geek-alpha/xiaozhi-esp32-music/releases?per_page=30"


def main() -> None:
    # urllib hits CERTIFICATE_VERIFY_FAILED behind the local proxy, so curl output is the primary source
    local = Path(__file__).resolve().parent.parent / "rel.json"
    if local.exists():
        data = json.loads(local.read_text(encoding="utf-8"))
    else:
        req = urllib.request.Request(API, headers={"User-Agent": "xz-audit"})
        with urllib.request.urlopen(req, timeout=40) as resp:
            data = json.loads(resp.read().decode("utf-8"))
    print("release count:", len(data))
    for rel in data:
        print(
            f"== {rel['tag_name']} | name={rel['name']} | published={rel['published_at']} | draft={rel['draft']}"
            f" | target={rel.get('target_commitish')}"
        )
        for asset in rel.get("assets", []):
            print(f"   - {asset['name']}  {asset['size']}  dl={asset['download_count']}")


if __name__ == "__main__":
    main()
