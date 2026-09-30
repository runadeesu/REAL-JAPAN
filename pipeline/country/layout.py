"""Combined layout of 秋津国 (fictional): the capital 千景 (capital.py, as designed for the island) and
the national geography (nation.py), in the names the generator, the special structures and the
cooker use. Everything here is invented.
"""

from __future__ import annotations

from . import capital as C
from . import nation as N

ORIGIN = N.ORIGIN
NAME_JA, NAME_EN = N.NAME_JA, N.NAME_EN

# ---- capital pieces kept as they were ----
SCRAMBLE = C.SCRAMBLE
AIRPORT = C.AIRPORT
RUNWAY = C.RUNWAY
TERMINAL = C.TERMINAL
ISLET = C.ISLET
BEACH = C.BEACH
PLAZAS = C.PLAZAS
VALLEYS = C.VALLEYS
PEAKS = C.PEAKS
MOUNTAIN_ROAD = C.MOUNTAIN_ROAD
BRIDGE_ROADS = set(C.BRIDGE_ROADS) | set(N.BRIDGE_ROADS)
FERRY_TERMINAL = C.FERRY_TERMINAL
SHINKANSEN_DECK = C.SHINKANSEN_DECK

# Districts: the capital's (its forested mountain district is replaced by the national land cover)
# and every other town and village (villages as rough discs).
def _disc(x, y, r, n=14, seed=0):
    import math
    import random
    rnd = random.Random(seed)
    return [(x + r * (0.8 + 0.35 * rnd.random()) * math.cos(2 * math.pi * k / n),
             y + r * (0.8 + 0.35 * rnd.random()) * math.sin(2 * math.pi * k / n)) for k in range(n)]


DISTRICTS = [(n, s, p) for n, s, p in C.DISTRICTS if s != "mountain"]
DISTRICTS += [(n, s, p) for n, en, s, p in N.CITIES]
DISTRICTS += [(n, "village", _disc(x, y, r, seed=k)) for k, (n, en, x, y, r) in enumerate(N.VILLAGES)]
DISTRICT_EN = {n: en for n, en, s, p in N.CITIES}
DISTRICT_EN.update({n: en for n, en, x, y, r in N.VILLAGES})

PARKS = list(C.PARKS)

# Roads: the capital's arterials and the national / local roads.
ARTERIALS = [(n, w, p) for n, w, p in C.ARTERIALS]
NATIONAL_ROADS = [(n, w, p) for n, w, p in N.ROADS]
EXPRESSWAY = N.EXPRESSWAY
EXPRESSWAY_RAMPS = N.EXPRESSWAY_RAMPS
EXPRESSWAY_PA = N.EXPRESSWAY_PA

# Rivers (the capital's 千景川 gets a longer upper course in the country) and lakes.
LAKES = N.LAKES

# Railways: loop line, main line (the island's branch extended), the Shinkansen to the west and
# the one to the snowy north. Stations: (name, x, y, platform heading deg).
RAIL_LINES = [
    dict(kind="loop", name="千景環状線", name_en="Chikage Loop Line", closed=True, pts=list(C.RAIL_LOOP[:-1]),
         stations=list(C.STATIONS[:4])),
    dict(kind="branch", name="秋津本線", name_en="Akitsu Main Line", closed=False,
         pts=list(C.RAIL_BRANCH) + list(N.MAIN_LINE_EXT), stations=list(C.STATIONS[4:]) + list(N.MAIN_LINE_STATIONS)),
    dict(kind="shinkansen", name="秋津新幹線", name_en="Akitsu Shinkansen", closed=False,
         pts=list(C.SHINKANSEN[:7]) + list(N.SHINKANSEN_EXT),
         stations=list(C.SHINKANSEN_STATIONS) + list(N.SHINKANSEN_STATIONS_EXT)),
    dict(kind="shinkansen", name="雪見新幹線", name_en="Yukimi Shinkansen", closed=False, pts=list(N.NORTH_SHINKANSEN),
         stations=list(N.NORTH_SHINKANSEN_STATIONS)),
]
# Rail level above the ground: viaducts in the cities, lower structures in the countryside.
def rail_deck(kind: str, x: float, y: float) -> float:
    in_capital = -4300 < x < 4300 and -3600 < y < 3900
    if kind == "shinkansen":
        return C.SHINKANSEN_DECK if in_capital else 10.0
    if kind == "branch" and not in_capital and not urban(x, y):
        return 0.6  # the main line runs at grade through the countryside (low bank, level crossings)
    return 9.0 if in_capital else 6.0


def urban(x: float, y: float) -> bool:
    """Inside a built-up area (the capital south of its mountains, or another town)."""
    if -4300 < x < 4300 and -3600 < y < 1300:
        return True
    return any(_contains(p, x, y) for _, _, s, p in N.CITIES if s not in ("village",))


def _contains(poly, x, y):
    inside = False
    n = len(poly)
    for i in range(n):
        (x1, y1), (x2, y2) = poly[i], poly[(i + 1) % n]
        if (y1 > y) != (y2 > y) and x < (x2 - x1) * (y - y1) / (y2 - y1) + x1:
            inside = not inside
    return inside


# Airports: (name, runway (A, B, width), terminal, polygon or None)
AIRPORTS = [
    dict(name="千景空港", name_en="Chikage Airport", runway=C.RUNWAY, terminal=C.TERMINAL, reclaimed=C.AIRPORT),
    dict(name=N.SOUTH_AIRPORT_NAME, name_en="Minamijima Airport", runway=N.SOUTH_RUNWAY, terminal=N.SOUTH_TERMINAL,
         reclaimed=None),
]
PIERS = N.PIERS
FERRY_ROUTES = N.FERRY_ROUTES

LANDMARKS = dict(C.LANDMARKS)
LANDMARKS.update(N.LANDMARKS)

# Readings of the place and station names (for signs and the map; ours, as the names are invented).
READINGS = {
    "千景中央": ("ちかげちゅうおう", "Chikage-Chuo"), "天望台": ("てんぼうだい", "Tembodai"), "古市": ("ふるいち", "Furuichi"),
    "臨海": ("りんかい", "Rinkai"), "西ヶ丘": ("にしがおか", "Nishigaoka"), "山麓": ("さんろく", "Sanroku"),
    "千景北": ("ちかげきた", "Chikage-Kita"), "稲穂": ("いなほ", "Inaho"), "高瀬": ("たかせ", "Takase"),
    "紫苑": ("しおん", "Shion"), "楓": ("かえで", "Kaede"), "朝凪": ("あさなぎ", "Asanagi"),
    "新朝凪": ("しんあさなぎ", "Shin-Asanagi"), "湯の沢温泉": ("ゆのさわおんせん", "Yunosawa-Onsen"),
    "雪見": ("ゆきみ", "Yukimi"), "千景": ("ちかげ", "Chikage"), "島ノ浦": ("しまのうら", "Shimanoura"),
    "南島": ("みなみじま", "Minamijima"), "山吹": ("やまぶき", "Yamabuki"), "汐見": ("しおみ", "Shiomi"),
    "岬": ("みさき", "Misaki"), "田原": ("たはら", "Tahara"), "野分": ("のわき", "Nowaki"), "白鷺": ("しらさぎ", "Shirasagi"),
    "東浦": ("ひがしうら", "Higashiura"), "北原": ("きたはら", "Kitahara"), "西浜": ("にしはま", "Nishihama"),
    "台場": ("だいば", "Daiba"), "焔岳": ("ほむらだけ", "Homuradake"), "秋津": ("あきつ", "Akitsu"),
}
