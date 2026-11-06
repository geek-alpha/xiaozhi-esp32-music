"""探测蜻蜓/电台 MP3 直链：content-type、首字节延迟、5 秒实收字节数。"""
import sys, time, json, urllib.request, urllib.error

UA = {"User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64)"}


def probe(url, seconds=5.0):
    t0 = time.time()
    try:
        req = urllib.request.Request(url, headers=UA)
        r = urllib.request.urlopen(req, timeout=10)
        first = time.time() - t0
        ctype = r.headers.get("Content-Type", "?")
        got = 0
        end = time.time() + seconds
        while time.time() < end:
            buf = r.read(4096)
            if not buf:
                break
            got += len(buf)
        dt = time.time() - t0 - first
        r.close()
        return dict(ok=True, ctype=ctype, first=round(first, 2),
                    kbs=round(got / dt / 1024, 1) if dt > 0 else 0)
    except Exception as e:
        return dict(ok=False, err=str(e)[:90])


def probe_urls(urls):
    for u in urls:
        p = probe(u)
        print(json.dumps({"url": u, **p}, ensure_ascii=False), flush=True)


def api(path):
    try:
        req = urllib.request.Request("https://live.qtfm.cn" + path, headers=UA)
        return urllib.request.urlopen(req, timeout=10).read().decode("utf-8", "ignore")
    except Exception as e:
        return "ERR " + str(e)[:120]


if __name__ == "__main__":
    if sys.argv[1] == "api":
        for p in sys.argv[2:]:
            print(p, "->", api(p)[:600])
    else:
        probe_urls(sys.argv[2:])
