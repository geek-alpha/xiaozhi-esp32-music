"""从 esp32_music.cc 提取 RADIO_STATIONS，复刻 StationScore / IsGenericRadioRequest /
StationWantScore 做表驱动校验：点名要播对，泛称不许擅自播，需求词要能挑出对口台。"""
import re
import sys
import io

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8")
SRC = r"D:\AI\tmp_xz_music\main\boards\common\esp32_music.cc"

GENERIC = ["广播电台", "收音机", "电台", "广播", "radio", "listen", "music",
           "音乐", "听", "放", "点", "个", "首", "一", "下", "来", "我", "想", "要", "台",
           "随便", "什么", "啥", "好听", "帮我", "给我", "现在", "就", "吧", "啊", "呢", "了", "的", "些"]

src = open(SRC, encoding="utf-8").read()
block = src.split("static const RadioStation RADIO_STATIONS[] = {")[1].split("\n};")[0]
ROWS = re.findall(r'\{"([^"]*)",\s*"([^"]*)",\s*"([^"]*)"\s*,\s*\n\s*"([^"]*)",\s*"([^"]*)"\}', block)
assert len(ROWS) >= 30, f"只解析到 {len(ROWS)} 个台，表格式变了？"


def is_generic(station):
    rest = station.lower()
    changed = True
    while changed:
        changed = False
        for w in GENERIC:
            if w in rest:
                rest = rest.replace(w, "")
                changed = True
    return len(rest.encode()) < 2


def score(want, aliases, name):
    kw = want.lower()
    if len(kw.encode()) < 2:
        return 0
    nm = name.lower()
    if nm == kw:
        return 1000
    best = 0
    if kw in nm:
        best = len(kw.encode())
    if nm in kw:
        best = max(best, len(nm.encode()))
    for a in aliases.split("|"):
        a = a.lower()
        if len(a.encode()) >= 2 and a in kw:
            best = max(best, 900 if a == kw else len(a.encode()))
    return best


def want_score(want, aliases, name, scene):
    best = score(want, aliases, name) * 10
    w = want.lower()
    for tag in scene.split("|"):
        if len(tag.encode()) >= 3 and tag.lower() in w:
            best = max(best, len(tag.encode()) * 10)
    return best


def resolve(station):
    """PlayRadio 的解析结果：None = 播不了（泛称/查不到）。"""
    if is_generic(station):
        return None
    best, best_score = None, 0
    for aliases, name, _url, _scene, _desc in ROWS:
        s = score(station, aliases, name)
        if s > best_score:
            best, best_score = name, s
    return best


def browse(want, k=6):
    scored = [(want_score(want, a, n, sc), i, n) for i, (a, n, _u, sc, _d) in enumerate(ROWS)]
    hits = [x for x in scored if x[0] > 0]
    if not hits:
        # C++ 侧同样兜底：字面没对上就退回前 k 个，清单绝不为空
        return [n for _a, n, _u, _sc, _d in ROWS[:k]]
    hits.sort(key=lambda x: (-x[0], x[1]))
    return [n for _s, _i, n in hits[:k]]


RESOLVE_CASES = [
    # 泛称一律不播：选择权归用户，不能让设备替他定
    ("", None), ("放广播", None), ("听电台", None), ("放个电台听听", None),
    ("随便放点音乐", None), ("我想听点东西", None), ("来点音乐", None),
    # 点名要精确
    ("北京新闻广播", "北京新闻广播"), ("放广州新闻电台", "广州新闻电台"),
    ("广州", "广州新闻电台"), ("新闻", "中国之声"),
    ("浙江音乐", "浙江音乐调频"), ("怀旧", "怀旧金曲964"), ("年代", "年代音乐1022"),
    ("芒果", "芒果音乐台"), ("欧美音乐", "欧美音乐88.7"), ("珠江", "珠江之声"),
    ("深圳交通", "深圳交通频率"), ("重庆音乐", "重庆音乐广播"), ("重庆", "重庆音乐广播"),
    ("中国之声", "中国之声"), ("广东", "广东音乐之声"), ("星空", "星空电台"),
    ("山东音乐", "山东经典音乐广播"), ("辽宁", "辽宁经典音乐广播"),
    ("上海", "上海沸点100音乐广播"), ("海南", "海南音乐广播"),
    ("不存在的台", None),
]

BROWSE_CASES = [
    # 只说需求、没点名：设备端靠 scene 兜底（真正语义判断在云端大模型）
    ("想听点适合学习的", "经典音乐FM90.5"),
    ("想听点不吵的", "经典音乐FM90.5"),
    ("工作时想听点纯音乐", "经典音乐FM90.5"),
    ("开车路上听的", "四川交通广播FM101.7"),
    ("堵车烦死了", "四川交通广播FM101.7"),
    ("睡前想安静点", "星空电台"),
    ("想听点怀旧老歌", "华语金曲500"),
    ("想知道发生什么大事", "华语金曲500"),  # 字面认不出，走 fallback：清单非空、交给云端判断
]

fail = 0
for want, expect in RESOLVE_CASES:
    got = resolve(want)
    if got != expect:
        fail += 1
        print(f"FAIL resolve {want!r} -> {got} (expect {expect})")
    else:
        print(f"OK   resolve {want!r} -> {got}")

for want, expect in BROWSE_CASES:
    got = browse(want)
    if not got or got[0] != expect:
        fail += 1
        print(f"FAIL browse  {want!r} -> {got[:3]} (expect首位 {expect})")
    else:
        print(f"OK   browse  {want!r} -> {got[:3]}")

total = len(RESOLVE_CASES) + len(BROWSE_CASES)
print(f"\n{len(ROWS)} 个台，{total} 条用例，失败 {fail}")
sys.exit(1 if fail else 0)
