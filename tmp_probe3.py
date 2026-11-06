# -*- coding: utf-8 -*-
import io, re, json

raw = io.open("kw_full.json", "rb").read()
for enc in ("utf-8", "gbk"):
    try:
        s = raw.decode(enc)
        if "'HIT':" in s:
            print("decoded as", enc)
            break
    except UnicodeDecodeError:
        continue
else:
    s = raw.decode("utf-8", "replace")

hit = re.search(r"'HIT':'(\d+)'", s)
print("HIT:", hit.group(1) if hit else "NA")

for m in re.finditer(r"'DC_TARGETID':'(\d+)'", s):
    seg = s[m.end():m.end() + 6000]
    d = re.search(r"'DURATION':'(\d+)'", seg)
    n = re.search(r"'SONGNAME':'([^']*)'", seg)
    a = re.search(r"'ARTIST':'([^']*)'", seg)
    print(m.group(1), d.group(1) if d else "-", (n.group(1) if n else "-")[:30], "|", (a.group(1) if a else "-")[:22])
