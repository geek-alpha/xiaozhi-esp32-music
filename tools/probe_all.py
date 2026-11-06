"""并发探活蜻蜓 64k MP3 直链。输入 qt_all.jsonl，输出通过清单。"""
import json, sys, time, urllib.request
from concurrent.futures import ThreadPoolExecutor

UA = {"User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64)"}
SECS = 2.0


def probe(item):
    rid = item["id"]
    url = f"https://lhttp.qtfm.cn/live/{rid}/64k.mp3"
    t0 = time.time()
    try:
        r = urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=8)
        first = time.time() - t0
        ctype = r.headers.get("Content-Type", "?")
        got, end = 0, time.time() + SECS
        while time.time() < end:
            b = r.read(4096)
            if not b:
                break
            got += len(b)
        r.close()
        dt = time.time() - t0 - first
        kbs = got / dt / 1024 if dt > 0.2 else 0
        ok = "mpeg" in ctype and kbs >= 12 and first < 3.0
        return {"id": rid, "name": item["name"], "ok": ok, "ctype": ctype,
                "first": round(first, 2), "kbs": round(kbs, 1)}
    except Exception as e:
        return {"id": rid, "name": item["name"], "ok": False, "err": str(e)[:60]}


if __name__ == "__main__":
    items = [json.loads(l) for l in open(sys.argv[1], encoding="utf-8") if l.strip()]
    res = []
    with ThreadPoolExecutor(max_workers=16) as ex:
        for i, r in enumerate(ex.map(probe, items), 1):
            res.append(r)
            if i % 50 == 0:
                print(f"# {i}/{len(items)}", file=sys.stderr, flush=True)
    ok = [r for r in res if r["ok"]]
    ok.sort(key=lambda x: -x["kbs"])
    for r in ok:
        print(json.dumps(r, ensure_ascii=False))
    print(f"# 通过 {len(ok)}/{len(res)}", file=sys.stderr)
