# -*- coding: utf-8 -*-
import io, re

s = io.open("kw_gbk.txt", encoding="utf-8", errors="replace").read()
hit = re.search(r"'HIT':'(\d+)'", s)
print("HIT:", hit.group(1) if hit else "NA")
print("DC_TARGETID count:", s.count("'DC_TARGETID':'"))
print("DURATION count:", s.count("'DURATION':'"))

for m in re.finditer(r"'DC_TARGETID':'(\d+)'", s):
    seg = s[m.end():m.end() + 6000]
    d = re.search(r"'DURATION':'(\d+)'", seg)
    n = re.search(r"'SONGNAME':'([^']*)'", seg)
    a = re.search(r"'ARTIST':'([^']*)'", seg)
    print(m.group(1), d.group(1) if d else "-", (n.group(1) if n else "-")[:24], (a.group(1) if a else "-")[:24])

print("--- head 600 ---")
print(s[:600])
