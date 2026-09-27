PROJECT: REAL JAPAN  —  development build v0.4.0
==========================================================================

Two worlds are included:
  * Chikage Island (fictional) - a procedurally generated island modelled on Tokyo's
    townscape (the default). The island, lines, stations, companies, ships and airline are
    all invented; no real place, shop or company names are used.
  * Shibuya - built from real data (MLIT PLATEAU, GSI elevation). Switch with
    world = shibuya in settings.ini or the launch option --world shibuya.

HOW TO START
  1. Extract the ZIP anywhere (e.g. your Desktop). If the download comes in several parts
     (-part1.zip ... -part5.zip), extract all of them to the same place (they all fill the
     same RealJapan-0.4.0-win64 folder); with a part missing the game says that world data
     is missing.
  2. Double-click RealJapan.exe. No installer or extra runtime (DLL) is needed.
     This development build is not signed: if Windows shows "Windows protected your PC",
     choose "More info" -> "Run anyway".
  3. Keep the "data" folder next to RealJapan.exe.

REQUIREMENTS
  OS      : Windows 10 / 11, 64-bit (x64)
  GPU     : OpenGL 3.3. A 2016-or-later discrete GPU or a recent integrated GPU is
            recommended (if it is slow, turn off post-processing / shadows in Settings)
  Memory  : 8 GB or more      Disk : about 500 MB
  Sound   : speakers / headphones (all sound is synthesised in code; Settings -> Volume)

CONTROLS (ON FOOT)
  W A S D / arrows : move     Shift : run     Space : jump     Mouse : look
  E : interact / board / enter (shown at the bottom)   V : first / third person
  Tab : smartphone   F : free-fly (exploring)   F3 : developer overlay
  F5 : quick save    F12 : screenshot            Esc : menu

TRANSPORT
  Trains / Shinkansen : E at a station (through the gates) -> E on the platform (board)
                        -> E at a stop (alight). X leaves through the gates. Look around
                        with the mouse.
  Train driver        : phone "Work" app -> train driver. W/S notches (5 power, 7 brake),
                        Space emergency brake, stop near the mark and press E to open the
                        doors. ATS protection.
  Cars                : E beside a stopped car. W/S throttle, brake / reverse, A/D steer,
                        Space handbrake, H horn, V chase / driver's seat, E (stopped) get out.
  Ferry               : E at Chikage Port or Shiomi Islet Pier. Walk the deck with WASD;
                        E to go ashore after berthing.
  Scheduled flight    : E at the Chikage airport terminal; window seat for take-off and
                        landing. The mainland airport is not included: the flight returns.
  Light aircraft      : E on the apron. Shift/Ctrl power, W/S pitch, A/D roll, Q/E rudder,
                        F/R flaps, Space brakes, V camera, E (stopped) get out.

WORK AND HOBBIES (phone "Work" / "Hobbies" apps)
  Taxi driver (Tokyo tariff meter), delivery rider, train driver, convenience-store till;
  other jobs are "simple shifts" where time passes (labelled as such). Fishing (E at a quay,
  Space on a bite, keep the line tension while reeling), photos (E shoots, wheel zooms;
  landmarks count as photo spots), shrines and temples (E worship, O omikuji, G goshuin),
  walking log.

ABOUT THE SOUND
  No recordings, stock libraries or third-party audio are used: everything is synthesised at
  run time - engines (from rpm and cylinder count), tyres and wind, passing traffic with
  Doppler shift, rail-joint beats, inverter tones and flange squeal of trains, Shinkansen
  wind noise, a departure melody and door chimes (original tunes written for this game, not
  real station melodies), the ferry's horn, airliner cabin noise, the light aircraft's engine
  and stall horn, city hum, crossing guide tones, crows, rain, wind and footsteps. Levels are
  game tuning values. There are no voices (announcements, conversation).

SAVES AND SETTINGS
  Saves    : %APPDATA%\RealJapan\saves\slot0.sav (autosave), slot1-3.sav
  Settings : %APPDATA%\RealJapan\settings.ini (changed in-game, saved automatically)
  Photos   : %APPDATA%\RealJapan\photos\   Screenshots : %APPDATA%\RealJapan\screenshots\
  Portable : put an empty "portable.txt" next to RealJapan.exe to keep saves and settings
             in a userdata\ folder.

LANGUAGE
  Title screen -> Settings -> Language switches Japanese / English.

HONEST NOTES
  * Chikage Island is fictional. Numbers (driver's share of fares, pay, fish sizes, omikuji
    odds, vehicle / aircraft figures) are game values, not statistics or real
    specifications. Trains, cars, aircraft, ships and stations are generic procedural
    models, not real types.
  * In Shibuya, estimated content (crossings, signals, street lights, utility poles on roads
    PLATEAU does not cover, near-field building detail, lit windows at night) is labelled as
    estimated in the game.
  * Not implemented: 3D shop interiors, conversation, voices, a home, building interiors
    other than the verified underground mall (no public source, so not invented), real data
    beyond Shibuya, car damage, GI / ray tracing, photo-real quality (not reached). See
    docs/STATUS.md in the repository for every item.
  * Tested on Linux and Wine with software OpenGL; not yet on real Windows PCs / GPUs.

DATA SOURCES
  * 3D City Model (Project PLATEAU) Shibuya-ku (FY2025), MLIT - processed
    (https://www.geospatial.jp/ckan/dataset/plateau-13113-shibuya-ku-2025)
    Terms: Public Data License 1.0 / CC BY 4.0 compatible (PLATEAU site policy)
  * GSI elevation tiles (DEM5A/DEM5B/DEM10B) - processed
    (https://maps.gsi.go.jp/development/ichiran.html)
    Heights are baked into this build; whether the Survey Act requires an application for
    this form of distribution is still being checked (development build).
  See LICENSES\DATA_SOURCES.txt. PLATEAU photo textures show real signs and adverts (the
  handling of third-party rights is still being checked); Settings -> "Building photo
  textures" turns them off. Chikage Island uses none of these data.

LICENCES
  See the LICENSES folder (raylib: zlib License; BIZ UDPGothic: SIL OFL 1.1, subset).
