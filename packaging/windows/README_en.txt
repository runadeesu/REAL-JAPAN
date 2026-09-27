PROJECT: REAL JAPAN  —  Shibuya Vertical Slice (development build) v0.2.0
==========================================================================

HOW TO START
  1. Extract the ZIP anywhere (e.g. your Desktop).
  2. Double-click "RealJapan.exe". No installer and no extra runtimes/DLLs are needed.
     This development build is not code-signed: if Windows SmartScreen shows
     "Windows protected your PC", choose "More info" -> "Run anyway".
  3. Keep the "data" folder next to RealJapan.exe.
  If the download came as "-part1.zip" and "-part2.zip", extract both into the same place (both fill the
  same RealJapan-0.2.0-win64 folder); with only one of them the game says that world data is missing.

REQUIREMENTS
  Windows 10 / 11 (64-bit x64), an OpenGL 3.3 capable GPU, 8 GB RAM recommended, ~300 MB disk.
  Shadows, SSAO and screen-space reflections want a discrete GPU from 2016 on or a recent integrated
  one; switch post effects / shadows off in Settings if it runs slowly.

CONTROLS
  WASD / arrows: move   Shift: run   Space: jump   Mouse: look
  F: toggle fly mode (Space up / Ctrl or C down)   V: first/third person
  E: inspect / enter (e.g. underground mall entrances)
  Tab: smartphone   Esc: menu   F3: developer overlay (position, FPS, draws, people / vehicles)
  F5: quick save to slot 1   F12: screenshot
  Look at a building to see its official PLATEAU attributes and verification status.
  Look at a person to see whether they are a simulated resident or a statistically generated visitor.

SAVES AND SETTINGS
  Saves:       %APPDATA%\RealJapan\saves\slot0.sav (autosave), slot1..3.sav
  Settings:    %APPDATA%\RealJapan\settings.ini (changed from the in-game Settings screen)
  Defaults:    data\config\default.ini
  Screenshots: %APPDATA%\RealJapan\screenshots\
  Portable mode: put an empty "portable.txt" next to RealJapan.exe to keep saves/settings in userdata\.

LANGUAGE
  Title screen -> Settings -> "Language / 言語" switches between Japanese and English.

IMPLEMENTED IN THIS BUILD (honest list)
  Real data (with sources)
  - 9,535 real buildings from MLIT Project PLATEAU (Shibuya City, FY2025), 3,064 with LOD2 shapes and
    aerial-photo based roof / wall textures
  - Real road shapes: carriageway, raised sidewalks with kerbs, medians; real street trees
  - PLATEAU street furniture (signals, lights, fences, signs, manholes, road markings) — surveyed on
    some roads only (e.g. east of Shibuya Station along Meiji-dori)
  - The verified PLATEAU LOD4 underground mall around Shibuya Station; walk in down the stairs
  - Terrain from GSI elevation tiles, astronomical sun position, Japanese national holidays

  Estimated / generated (not survey data; kept apart in the game as well)
  - Where PLATEAU has no markings / signals / lights: crossings, stop lines and lane lines derived from
    the real road shapes, signals (poles and heads) at major junctions, road lights, and utility poles
    with overhead lines and small street lamps on narrow streets, placed the usual Japanese way
  - Near-field building detail (shop fronts, glass, sign bands, awnings, windows, AC units). Sign
    lettering is a generic pattern, not real shop names (they are not in the data)
  - Lit windows at night (not in the photo textures; driven by time-of-day occupancy)
  - Signal timing (real signal plans are not public: a 100 s cycle is assumed)

  Simulation
  - 10 weather states (clear to thunderstorm, fog, wind); wet roads, puddles, people with umbrellas
  - Cars on the real carriageway network: left-hand traffic, stop at signals / stop lines, volume by hour
  - People on foot: fictional residents (living in real buildings, with daily plans) plus visitors
    generated statistically from the time of day, the use and floor area of the real buildings around
    and the real station entrances (not measured people-flow data). They keep to sidewalks and
    crossings and wait at red lights.
  - Rendering: physically based shading, sun shadows, SSAO, screen-space reflections, bloom,
    auto exposure, night city lighting
  - Smartphone (map, clock & sun, bank, town life), save/load, Japanese / English

NOT IMPLEMENTED YET
  Trains, aircraft, ships, driving by the player, shopping, jobs, dialogue, sound, interiors other
  than the verified underground mall (never fabricated: no public source), regions outside Shibuya
  (the other prefectures), ray tracing / global illumination, volumetric light, motion blur,
  photo-indistinguishable quality (not reached). People and cars are simple procedural models.
  Details: docs/STATUS.md in the repository.

DATA SOURCES
  - Processed from "3D City Model (Project PLATEAU), Shibuya City (FY2025)" by the Ministry of Land,
    Infrastructure, Transport and Tourism (MLIT), Japan.
    https://www.geospatial.jp/ckan/dataset/plateau-13113-shibuya-ku-2025
    Terms: Public Data License 1.0, compatible with CC BY 4.0 (PLATEAU site policy).
  - Source: GSI Japan elevation tiles (DEM5A/DEM5B/DEM10B), processed.
    https://maps.gsi.go.jp/development/ichiran.html
    The heights are baked into this build; whether a Survey Act procedure is needed is still being checked
    (development build).
  Details: LICENSES\DATA_SOURCES.txt
  Building names are shown exactly as given in PLATEAU's name attribute.
  PLATEAU photo textures contain real signs and advertising (third-party rights under review);
  switch them off in Settings -> "Building photo textures".

LICENCES
  See the LICENSES folder (raylib: zlib License; BIZ UDPGothic: SIL Open Font License 1.1, bundled as a subset).
