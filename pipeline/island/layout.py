"""千景島 (Chikage-jima): a FICTIONAL island inspired by Tokyo.

Hand-designed macro layout (GTA-style curation): coastline, districts, river, arterial roads,
railway loop, landmarks. Everything below is invented; no real place, shop or person is depicted.
Coordinates: island-local metres, x = east, y = north, origin at ORIGIN (open Pacific, no real land).
"""

from __future__ import annotations

ORIGIN = (34.60, 140.40)  # lat, lon of the local frame (ocean south-east of the Boso peninsula)
NAME_JA, NAME_EN = "千景島（架空）", "Chikage Island (fictional)"

# Coastline, counter-clockwise.
COAST = [
    (-3750, -300), (-3600, -1500), (-3100, -2400), (-2200, -2750), (-1100, -2550), (-300, -2450),
    (600, -2150), (1300, -1880), (2000, -1760), (2700, -2080), (3000, -2350), (3350, -2300),
    (3550, -1500), (3700, 400), (3300, 1700), (2300, 2700), (900, 3100), (-600, 3200),
    (-2200, 3000), (-3300, 2200), (-3600, 800),
]
# Airport on reclaimed land off the south-west coast (空港), one 2,000 m runway.
AIRPORT = [(-3950, -1750), (-1900, -2350), (-1650, -3150), (-3850, -2650)]
RUNWAY = ((-3650, -2350), (-1880, -2880), 45.0)  # threshold A, threshold B, width
TERMINAL = (-2700, -2250)
# Ferry terminal on the south-east cape; routes to the islet and off the map.
FERRY_TERMINAL = (3150, -2150)
# (first / last points are the pier heads; the client adds the berthing legs)
FERRY_ROUTES = [[(3150, -2445), (2800, -2600), (2500, -2760)], [(3150, -2445), (3600, -2900), (4300, -3500)]]
# Beach on the east coast (千景海岸).
BEACH = [(3350, 450), (3900, 450), (3700, 1650), (3200, 1600)]
# Artificial island in the bay (台場風), reached by a suspension bridge.
ISLET = [(1450, -2560), (2350, -2560), (2420, -2980), (1500, -3060)]

# District polygons (clipped to land by the generator). style -> street grid + building rules.
DISTRICTS = [
    # name_ja, style, polygon
    ("千景中央", "center", [(-1000, -1500), (700, -1500), (900, -900), (900, -100), (-1000, -100)]),
    ("天望台", "business", [(-1100, -100), (900, -100), (760, 600), (300, 1150), (-1100, 1150)]),
    ("古市", "shitamachi", [(1120, -1360), (3600, -1360), (3750, 300), (3300, 1500), (2300, 1950),
                            (1000, 760), (1080, 100), (1160, -500)]),
    ("西ヶ丘", "residential", [(-3900, -1700), (-1900, -2300), (-600, -2600), (-600, -1500), (-1000, -1500),
                              (-1000, 1150), (-3900, 1150)]),
    ("千景空港", "airport", AIRPORT),
    ("臨海", "bay", [(-600, -2700), (3400, -2700), (3400, -1360), (1120, -1360), (900, -1500), (-600, -1500)]),
    ("台場", "islet", ISLET),
    ("千景山", "mountain", [(-3900, 1150), (300, 1150), (1000, 760), (2300, 1950), (3300, 1500), (3900, 3600),
                            (-3900, 3600)]),
]

# Parks / open spaces (no buildings; trees).
PARKS = [
    ("千景公園", [(-1760, -220), (-1080, -220), (-1080, 720), (-1760, 720)]),
    ("川辺緑地", [(620, 520), (820, 330), (900, 420), (700, 640)]),
    ("臨海公園", [(-400, -2350), (400, -2120), (380, -1880), (-420, -2050)]),
]
# Station forecourt (pedestrian plaza) west of the main station, north-east of the scramble.
PLAZAS = [[(-300, -730), (-200, -730), (-200, -620), (-300, -620)]]

# River from the mountains to the bay: (x, y, width).
RIVER = [(-700, 2650, 12), (-450, 1850, 18), (-100, 1300, 26), (500, 700, 40), (950, 150, 50),
         (1120, -500, 56), (1300, -1200, 60), (1480, -1800, 70)]

# Arterial roads: (name, width m, polyline). The five roads meeting at SCRAMBLE form the big crossing.
SCRAMBLE = (-330, -780)
ARTERIALS = [
    ("中央大通り", 40, [(-330, -2150), (-330, -780), (-330, 420), (-260, 1150)]),
    ("西ヶ丘通り", 30, [(-330, -780), (-1000, -720), (-2000, -620), (-3350, -520)]),
    ("臨海通り", 30, [(-330, -780), (60, -1080), (700, -1480), (1500, -1640), (2600, -1760), (3150, -1980)]),
    ("道玄坂", 22, [(-330, -780), (-720, -1120), (-1250, -1620), (-2050, -2250)]),
    ("古市通り", 30, [(-330, -780), (100, -830), (700, -810), (1500, -780), (2600, -700), (3550, -600)]),
    ("天望通り", 36, [(-1780, 260), (-330, 260), (800, 260), (1400, 320), (3050, 420)]),
    ("古市中通り", 20, [(2050, -1300), (2050, 1450)]),
    ("環状北通り", 24, [(-1000, -100), (-1000, 1000), (250, 1000), (700, 600)]),
    ("海岸通り", 16, [(-3350, -520), (-3450, 500), (-3050, 1450)]),
    ("大橋通り", 24, [(1620, -1650), (1650, -1900), (1880, -2520), (1900, -2800)]),
    ("空港通り", 24, [(-2050, -2250), (-2400, -2200), (-2700, -2170)]),
    ("港通り", 20, [(2600, -1760), (2950, -1950), (3150, -2150)]),
]
# Roads that cross open water on bridges (kept over the sea).
BRIDGE_ROADS = {"大橋通り"}
# Mountain road to the summit shrine (switchbacks).
MOUNTAIN_ROAD = ("千景山道路", 8, [(-260, 1150), (-520, 1350), (-230, 1560), (-620, 1720), (-930, 1660),
                                   (-1120, 1900), (-1260, 2060)])

# Railway loop (elevated in the city) and its stations (name, x, y, heading deg of platforms).
RAIL_LOOP = [(-130, -1050), (-130, -380), (-60, 700), (500, 1150), (1300, 900), (1750, 250), (1800, -500),
             (1450, -1450), (800, -1700), (100, -1500), (-130, -1050)]
RAIL_BRANCH = [(-130, -1050), (-600, -1350), (-1500, -1150), (-2300, -900), (-2700, -200), (-2500, 800)]
STATIONS = [
    ("千景中央駅", -130, -700, 0),
    ("天望台駅", -80, 600, 5),
    ("古市駅", 1765, -100, 3),
    ("臨海駅", 800, -1700, 100),
    ("西ヶ丘駅", -2300, -900, 70),
    ("山麓駅", -2560, 650, 10),
]

# Shinkansen (high-speed line): terminal platforms beside 千景中央駅, north through the business
# district, a tunnel under 千景山, 千景北駅 on the north coast, then a sea bridge towards the
# (off-map) mainland. Rail level above the ground in the city (above the loop line).
SHINKANSEN = [(-40, -1250), (-40, -700), (-40, -100), (60, 700), (260, 1250), (420, 1900), (520, 2650),
              (560, 3050), (600, 3900)]
SHINKANSEN_DECK = 16.0
SHINKANSEN_STATIONS = [("千景中央駅（新幹線）", -40, -760, 0), ("千景北駅", 545, 2900, 3)]
# Ferry piers (terminal on the south-east cape, and the islet).
FERRY_PIERS = [(3150, -2325, 180), (2388, -2800, 70)]  # pier root at the coast, heading out to sea

# Landmarks (generated specially).
LANDMARKS = {
    "temple": ("千景寺", 2400, 650),          # main hall + five-storey pagoda + gate
    "shrine": ("千景山神社", -1300, 2150),    # summit shrine with torii
    "tower": ("千景タワー", 450, -300),        # red-and-white lattice broadcast tower
    "wheel": ("台場観覧車", 2150, -2800),      # Ferris wheel on the islet
    "stadium": ("千景スタジアム", -2350, 300),
}

# Mountains: (x, y, height m, radius m).
PEAKS = [(-1300, 2150, 460, 950), (1400, 2350, 330, 750), (-2650, 1950, 260, 650), (300, 2700, 220, 600)]
# Inland valley around the main station (the commercial district sits in a hollow, with slopes).
VALLEYS = [(-250, -800, 12.0, 420)]
