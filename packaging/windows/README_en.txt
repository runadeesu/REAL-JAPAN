PROJECT: REAL JAPAN  —  Shibuya Vertical Slice (Phase 1 development build) v0.1.0
==================================================================================

HOW TO START
  1. Extract the ZIP anywhere (e.g. your Desktop).
  2. Double-click "RealJapan.exe". No installer and no extra runtimes/DLLs are needed.
     This development build is not code-signed: if Windows SmartScreen shows
     "Windows protected your PC", choose "More info" -> "Run anyway".
  3. Keep the "data" folder next to RealJapan.exe.

REQUIREMENTS
  Windows 10 / 11 (64-bit x64), an OpenGL 3.3 capable GPU, 8 GB RAM recommended, ~300 MB disk.

CONTROLS
  WASD / arrows: move   Shift: run   Space: jump   Mouse: look
  F: toggle fly mode (Space up / Ctrl or C down)   V: first/third person
  Tab: smartphone   Esc: menu   F5: quick save to slot 1   F12: screenshot
  Look at a building to see its official PLATEAU attributes and verification status.

SAVES AND SETTINGS
  Saves:       %APPDATA%\RealJapan\saves\slot0.sav (autosave), slot1..3.sav
  Settings:    %APPDATA%\RealJapan\settings.ini (changed from the in-game Settings screen)
  Defaults:    data\config\default.ini
  Screenshots: %APPDATA%\RealJapan\screenshots\
  Portable mode: put an empty "portable.txt" next to RealJapan.exe to keep saves/settings in userdata\.

LANGUAGE
  Title screen -> Settings -> "Language / 言語" switches between Japanese and English.

IMPLEMENTED IN THIS BUILD (honest list)
  - 9,535 real buildings from MLIT Project PLATEAU (Shibuya City, FY2025), 3,064 with LOD2 roof shapes
  - PLATEAU photo textures (aerial-photo based roofs and walls) on all LOD2 buildings
  - Real road shapes (carriageway / sidewalk / medians), crosswalks and lane lines, and street furniture
    (signals, lights, poles, fences) from PLATEAU; terrain from GSI elevation tiles
  - JGD2011 geodesy, JIS mesh-based hierarchical streaming, floating origin
  - Astronomical sun position (day/night, shadows), Japanese national holiday calendar
  - Walking with building collision, fly mode, building info panel with sources and verification state
  - Smartphone: map, clock & sun, bank, town life
  - Daily schedules of 3,000 fictional residents and 4,000 inbound commuters; people on foot walk
    the real streets in 3D (look at them to see name, job and next activity)
  - Save/load (3 slots + autosave), persistent settings, Japanese / English

NOT IMPLEMENTED YET
  Building interiors, shopping, jobs, cars, trains, aircraft, ships, dialogue, weather,
  sound, regions outside Shibuya (the other prefectures), photoreal rendering.
  Interiors are closed on purpose: no public source exists, and they are never fabricated.
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
