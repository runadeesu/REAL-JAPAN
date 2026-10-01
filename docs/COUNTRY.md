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
| Relief | ranges along warped crest lines (秋津山脈 spine up to ~1,400 m, 千景山地, 朝凪山地, the hills around 紫苑), a concave stratovolcano 焔岳 (1,850 m, summit crater, radial gullies), the southern island's cone 南島山, flat alluvial plains (千景平野, 雪見平野, 紫苑盆地), dendritic valleys cut along the computed drainage network |
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
| 秋津本線 | conventional main line 西ヶ丘 (west side of the capital) — 山麓 — 稲穂 — 高瀬 — 紫苑 — 楓 — 朝凪 (6-car trains, up to 120 km/h, curve limits); at grade across the flat countryside, with level crossings |
| 秋津新幹線 | 千景中央 — 千景北 — 紫苑 — 新朝凪 (8 cars, up to 300 km/h) |
| 雪見新幹線 | 千景北 — 湯の沢温泉 — (long tunnel under the spine) — 雪見 |
| Roads | national roads between the towns (two lanes with paved shoulders outside the towns, sidewalks inside), mountain pass road, the volcano skyline road; road tunnels and viaducts where a road cannot follow the ground within its grade |
| Expressway | 秋津自動車道 千景西 IC — 稲穂 IC — 紫苑 IC — 朝凪 IC (four lanes with a median barrier, guard rails, tunnels and viaducts; on viaducts through built-up land and over every road and railway it crosses; one two-way slip road per interchange, simplified, with a 320 m acceleration lane where it joins; a toll plaza with ETC barriers on each slip road, an ETC gantry where the slip road is a viaduct; 稲穂 PA on both carriageways with a car park, a shop, toilets, vending machines and a fuel station) |
| Air | 千景空港 ⇄ 南島空港 (regional jet: take-off, cruise, approach and landing on the map) |
| Sea | 千景港 ⇄ 台場 (harbour ferry), 千景港 ⇄ 島ノ浦港 (high-speed ferry), 朝凪港 ⇄ 島ノ浦港 (car ferry) |

Rail profiles are grade-limited (bridging narrow valleys, tunnelling through ridges, level through
stations; viaducts clear the streets in towns); tunnel mouths have portals, the ground is cut away
there and dug into cuttings on the approaches. Outside the towns the main line runs at grade on the
flat and meets roads at level crossings (warning lamps flashing in turn, bell, barriers; road
traffic waits; lowered barriers stop people and the player's car, and a train brakes to a stop short
of a crossing with someone or something on it). Fares are distance-based game values charged by IC
card at the exit gate. Expressway tolls are ETC-style: the entry plaza is recorded and the exit plaza
charges by the distance between them (game values).

Road profiles are grade-limited too (mountain roads 10 %, the expressway 5 %); where the ground
lies more than about 14 m above the profile the road goes into a tunnel (portal, lit tube, walkway),
more than about 9 m below it onto a viaduct (piers, parapets). The ground is left as it is there.
Each free end of a road is at ground level: the profile is capped by a cone of the grade rising from
each end (v0.7.0; before, the expressway could not get down the steep mountain edge above 朝凪 within
5 % and ended, with its slip road, 250 m in the air — now a long tunnel brings it down), and it never
goes below the sea (an inlet is crossed on a viaduct).

### Getting on and off (first person)

Nothing teleports the player aboard any more. Stations: concourse, IC gates (open by default; the
flaps shut when the card is short of the fare), stairs, platforms; walk through an open door of a
stopped train, look at a free seat and press E (or click) to sit, again to stand; walk through the
gangways to the next car; step off through the doors at a stop. Ferries: the gangway laid to the
pier while the ship is alongside, benches on deck. Flights: the passenger stairs at the stand, the
aisle and the seats; stand up after landing and walk down the stairs. Cars and the light aircraft:
look at the door and press E. The crosshair shows an icon and a label for what can be used.

### Other details

* Walk-in shops on the ground floor of about half the small shop buildings in the capital's old
  town and the regional towns (convenience store, cafe, general shop): generic fit-out, no brands
  or names; see-through glazing and doors; buy at the counter (look at it, E or click; the goods
  and prices are game values, what is bought is kept in the save and listed in the phone's wallet).
  `shops.txt` lists each counter and where to stand; `tolls.txt` the toll plazas.
* Residents (12,000) live in the houses of every town, weighted towards the regional towns, and
  mostly work near home (game assumptions, not statistics).
* Single trees (conifers and broadleaf, from the land-cover forest mask) within about 100 m of the
  player; the canopy surface beyond.
* Snow falls instead of rain in winter where the snow lies; cherry blossom in early April, the rainy
  season from early June to mid July, now and then a typhoon from mid August to mid October.
* The player's flat (`home.txt`): a furnished one-room flat on the ground floor of a building in the
  capital about 860 m from the start (the walk-in floor nearest the start that fits), rented from
  the phone; the doorway is locked until then; the bed sleeps the night through.
* Fuel stations (`fuel.txt`): at the parking area (both sides) and on a roadside lot in 11 towns
  (apron, canopy over two pump islands, kiosk, a blank price board; generic, no brand).
* Schools: 17 elementary, junior high and high schools in the towns (a three-storey building with a
  clock, a sand sports ground with goals, a fence with a gate); the students among the residents
  attend the nearest one. The player can sit in on a class (the classroom is not shown).
* Estimated small street furniture (client side, from the road graph and the buildings): drink
  vending machines with recycling bins, parked bicycles, shop stand signs.
* Six weather regions (capital, west, Shion, north, mountains, southern island), each with its own
  weather and leanings (a fictional climate); the sky follows the region the player is in.
* Waves on the sea around the camera, a tide of up to about +-0.6 m (a game model), swimming.
* Traffic beyond the area around the player is drawn as simple moving cars on the far roads.

## Data (pipeline/cook_country.py → game/data/world/country)

* `cells/*.rjcell|rjdet` — per JIS 3rd-mesh cell (about 1.1 x 0.9 km): terrain, buildings, street
  detail, ground raster; the RJDET gains a land-cover section (RGBA weights: forest, paddy, field,
  bare) for the client's rural materials and forest canopy. Rural cells use a 1024 px ground raster.
* `far_height.bin`, `far_color.png`, `far_blds.bin`, `far.txt` — the far view (whole country on a
  lat/lon grid, 1/24 of a cell) drawn beyond the streamed cells.
* `snow.png` — snow potential.
* `rail.txt` (lines with names, stations, readings), `transport.txt` (airports, piers, ferry routes),
  `roads.rjroad`, `residents.csv`, `client.txt` (cells, place names, readings, POIs).

Figures of this build (v0.8.0): main island about 1,380 km², 1,507 cells, about 196,000 generated
buildings, about 535 MB of cell data (walk-in shops and stations add detail geometry; cook: about
45 minutes on 4 processes). 49 road tunnels and 49 viaducts, 7 level crossings, 4 interchanges with
toll plazas, a parking area on both carriageways, 13 fuel stations, 17 schools, about 5,980 walk-in
shop counters, the player's flat, 12,000 residents.

## Not implemented (honest list)

* Interchanges are simplified (one two-way slip road each); no switchbacks on mountain roads.
* Other buildings cannot be entered (stations, the walk-in shops and the player's flat only); the
  schools' classrooms, the hotel rooms and the taxi ride are not shown (time moves on).
* Talk, SNS posts, phone calls and announcements are fixed templates (no language model, no voices).
* People and cars are shown around the player only (simple moving cars on the far roads).
* Only the walk-in shops have see-through glass; other windows are opaque (photo texture or
  interior mapping).
