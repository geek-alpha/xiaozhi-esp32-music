"""抓蜻蜓电台地区页，提取 (广播台id, 台名)。用法: python scrape_qtfm.py 3 5 7 ..."""
import re, sys, json, urllib.request

UA = {"User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 Chrome/120 Safari/537.36"}
RE_RADIO = re.compile(r'href="/radios/(\d+)"[^>]*>\s*<img[^>]*alt="([^"]+)"')


def page(area, p=1):
    url = f"https://www.qtfm.cn/radiopage/{area}/{p}"
    req = urllib.request.Request(url, headers=UA)
    return urllib.request.urlopen(req, timeout=15).read().decode("utf-8", "ignore")


if __name__ == "__main__":
    seen = {}
    for area in sys.argv[1:]:
        for p in (1, 2):
            try:
                html = page(area, p)
            except Exception as e:
                print(f"# area {area}/{p} ERR {e}", file=sys.stderr)
                break
            found = RE_RADIO.findall(html)
            if not found:
                break
            for rid, name in found:
                seen.setdefault(rid, name.strip())
    for rid, name in seen.items():
        print(json.dumps({"id": rid, "name": name}, ensure_ascii=False))
