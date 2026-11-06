# -*- coding: utf-8 -*-
import io, json, urllib.request, urllib.parse

songmid = "0039MnYb0qxYhV"
data = {
    "req_0": {
        "module": "vkey.GetVkeyServer",
        "method": "CgiGetVkey",
        "param": {
            "guid": "10000",
            "songmid": [songmid],
            "songtype": [0],
            "uin": "0",
            "loginflag": 1,
            "platform": "20",
        },
    }
}
url = "https://u.y.qq.com/cgi-bin/musicu.fcg?format=json&data=" + urllib.parse.quote(
    json.dumps(data, separators=(",", ":"))
)
req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0", "Referer": "https://y.qq.com/"})
raw = urllib.request.urlopen(req, timeout=15).read().decode("utf-8", "replace")
d = json.loads(raw)
r = d["req_0"]["data"]
m = r["midurlinfo"][0]
print("purl:", m.get("purl"))
print("result:", m.get("result"))
print("sip0:", (r.get("sip") or [""])[0])

purl = m.get("purl") or ""
if purl:
    full = (r.get("sip") or ["http://dl.stream.qqmusic.qq.com/"])[0] + purl
    print("FULL_URL:", full[:160])
    r2 = urllib.request.Request(full, headers={"User-Agent": "Mozilla/5.0"}, method="GET")
    try:
        resp = urllib.request.urlopen(r2, timeout=15)
        chunk = resp.read(2048)
        print("audio_http:", resp.status, "first_bytes:", chunk[:4].hex(), "len:", len(chunk))
    except Exception as e:
        print("audio_fetch_fail:", repr(e)[:200])
