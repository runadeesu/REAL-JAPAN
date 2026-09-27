# 秋津国 (Akitsu) — the fictional country

**Everything in this world is invented.** 秋津国 is a fictional country modelled on the look and
feel of Japan (GTA-style curation): no real place, shop, company, railway, timetable or person is
depicted. Names, readings, fares and service patterns are game values. The real-data world
(PLATEAU Shibuya) is separate and unchanged.

It replaces the earlier fictional island 千景島 (v0.4). The island's city became the country's
capital 千景 with its layout kept; the country grew around it.

## Geography (pipeline/country/nation.py)

| Feature | Design |
|---|---|
| Extent | main island about 50 x 38 km, a southern island 10 x 7 km, the capital's islet |
| Coast | hand-drawn outline, roughened procedurally: rocky coasts, a ria coast in the north-west, the long straight sandy beach of the southern plain, the capital's sea walls kept exactly |
| Relief | ranges along warped crest lines (秋津山脈 spine up to ~1,600 m, 千景山地, 朝凪山地, the hills around 紫苑), a concave stratovolcano 焔岳 (1,850 m, summit crater, radial gullies), the southern island's cone 南島山, flat alluvial plains (千景平野, 雪見平野, 紫苑盆地), dendritic valleys cut along the computed drainage network |
| Water | five rivers with concave long profiles, meandering inside V-shaped valleys; the lake 焔湖 |
| Land cover | forest on slopes (runtime canopy in the client), rice paddies on plains and valley floors, upland fields near villages, grass on the volcano's skirt, bare scoria above its tree line, beaches |
| Climate | snow potential: the north coast beyond the spine and high ground; lies in winter only (season from the game date) |

## Towns

| Town | Style | Notes |
|---|---|---|
| 千景 (capital) | the island city | unchanged layout: loop line, main station, tower, temple, bay, airport |
| 千景北 | suburb | the Shinkansen junction |
| 紫苑 | old capital | strict north–south grid of machiya streets in a basin; temples on the eastern hills, a shrine with a tunnel of torii |
| 朝凪 | port city | downtown between the sea and a steep range, container port with gantry cranes, castle on the hill, car ferry |
| 湯の沢温泉 | hot-spring town | inns and hotels along the river in a mountain valley |
| 雪見 | snow country | steep metal roofs (snow on them in winter) on the north coast |
| 島ノ浦 | island harbour | ferries and the southern airport |
| villages | farm villages | farmhouses with heavy roofs and sheds among the fields |

## Transport

| Mode | Services (game values) |
|---|---|
| 千景環状線 | the capital's elevated loop (as before) |
| 秋津本線 | conventional main line 西ヶ丘 (west side of the capital) — 山麓 — 稲穂 — 高瀬 — 紫苑 — 楓 — 朝凪 (6-car trains, up to 120 km/h, curve limits) |
| 秋津新幹線 | 千景中央 — 千景北 — 紫苑 — 新朝凪 (8 cars, up to 300 km/h) |
| 雪見新幹線 | 千景北 — 湯の沢温泉 — (long tunnel under the spine) — 雪見 |
| Roads | national roads between the towns (two lanes with paved shoulders outside the towns, sidewalks inside), mountain pass road, the volcano skyline road |
| Air | 千景空港 ⇄ 南島空港 (regional jet: take-off, cruise, approach and landing on the map) |
| Sea | 千景港 ⇄ 台場 (harbour ferry), 千景港 ⇄ 島ノ浦港 (high-speed ferry), 朝凪港 ⇄ 島ノ浦港 (car ferry) |

Rail profiles are grade-limited (bridging narrow valleys, tunnelling through ridges, level through
stations; viaducts clear the streets in towns); tunnel mouths have portals, the ground is cut away
there and dug into cuttings on the approaches. Fares are distance-based game values paid by IC
card at the end of a ride.

## Data (pipeline/cook_country.py → game/data/world/country)

* `cells/*.rjcell|rjdet` — per JIS 3rd-mesh cell (about 1.1 x 0.9 km): terrain, buildings, street
  detail, ground raster; the RJDET gains a land-cover section (RGBA weights: forest, paddy, field,
  bare) for the client's rural materials and forest canopy. Rural cells use a 1024 px ground raster.
* `far_height.bin`, `far_color.png`, `far_blds.bin`, `far.txt` — the far view (whole country on a
  lat/lon grid, 1/24 of a cell) drawn beyond the streamed cells.
* `snow.png` — snow potential.
* `rail.txt` (lines with names, stations, readings), `transport.txt` (airports, piers, ferry routes),
  `roads.rjroad`, `residents.csv`, `client.txt` (cells, place names, readings, POIs).

## Not implemented (honest list)

* Level crossings: all railways run on viaducts, embankments, in cuttings or tunnels.
* Expressways; road tunnels (mountain roads follow the ground with cuts and fills).
* Individual trees in the countryside (forests are a canopy surface; street and park trees are single trees).
* Snow falling (snow lies on the ground in winter; the weather shows rain only).
* Interiors of the regional towns' buildings; people and traffic outside the area around the player.
