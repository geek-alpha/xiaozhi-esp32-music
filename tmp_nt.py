# -*- coding: utf-8 -*-
import io, json

d = json.load(io.open("nt_post.json", encoding="utf-8", errors="replace"))
r = d.get("result") or {}
songs = r.get("songs") or []
print("count:", len(songs))
for x in songs:
    arts = "/".join(a["name"] for a in x.get("artists") or [])
    print(x["id"], x["name"], "|", arts, "|", x.get("duration"))
