PROJECT: REAL JAPAN  —  development build v0.6.0
==========================================================================

Two worlds are included:
  * Akitsu (fictional) - a procedurally generated country modelled on the look of Japan
    (the default): the capital Chikage, the old capital Shion, the port city Asanagi, the
    hot-spring town Yunosawa, snowy Yukimi, a southern island, farm villages, a mountain
    spine and the volcano Homuradake. The country, places, lines, stations, companies,
    ships and airline are all invented; no real place, shop or company names are used.
  * Shibuya - built from real data (MLIT PLATEAU, GSI elevation). Switch with
    world = shibuya in settings.ini or the launch option --world shibuya.

HOW TO START
  1. Extract the ZIP anywhere (e.g. your Desktop). If the download comes in several parts
     (-part1.zip ... -part19.zip), extract all of them to the same place (they all fill the
     same RealJapan-0.6.0-win64 folder); with a part missing the game says that world data
     is missing.
  2. Double-click RealJapan.exe. No installer or extra runtime (DLL) is needed.
     This development build is not signed: if Windows shows "Windows protected your PC",
     choose "More info" -> "Run anyway".
  3. Keep the "data" folder next to RealJapan.exe.

REQUIREMENTS
  OS      : Windows 10 / 11, 64-bit (x64)
  GPU     : OpenGL 3.3. A 2016-or-later discrete GPU or a recent integrated GPU is
            recommended (if it is slow, turn off post-processing / shadows in Settings)
  Memory  : 8 GB or more      Disk : about 1 GB
  Sound   : speakers / headphones (all sound is synthesised in code; Settings -> Volume)

CONTROLS (ON FOOT)
  W A S D / arrows : move     Shift : run     Space : jump     Mouse : look
  E or left click : use (sit, stand, drive a car, fly ... - the crosshair shows an icon and a
                    label when what you look at can be used) / examine
  V : first / third person   Tab : smartphone   F : free-fly (exploring)
  F3 : developer overlay     F5 : quick save    F12 : screenshot    Esc : menu

TRANSPORT (since v0.6.0 you get on and off everything on foot; the old "press E to board"
teleports are gone)
  Trains / Shinkansen : walk in from a station entrance, through the IC gates (the card is
                        touched automatically) and up the stairs to the platform. Walk in
                        through an open door of a stopped train; look at a free seat and
                        press E to sit, E again (or a move key) to stand (occupied seats are
                        taken). Walk along the car and through the gangways into the next car.
                        At a stop, walk out through the open doors; the distance fare (plus
                        a Shinkansen charge; game values) is taken at the exit gate, whose
                        flaps shut if the card is short. The Akitsu Main Line has level
                        crossings (lamps, bell and barriers as a train comes).
                        Lines: Chikage Loop, Akitsu Main Line (Nishigaoka - Shion - Asanagi),
                        Akitsu Shinkansen (Chikage-Chuo - Chikage-Kita - Shion - Shin-Asanagi),
                        Yukimi Shinkansen (Chikage-Kita - Yunosawa Onsen - Yukimi).
  Train driver        : phone "Work" app -> train driver. W/S notches (5 power, 7 brake),
                        Space emergency brake, stop near the mark and press E to open the
                        doors. ATS protection.
  Cars                : look at the door of a stopped car and press E. W/S throttle, brake /
                        reverse, A/D steer, Space handbrake, H horn, V chase / driver's seat,
                        E (stopped) get out. The Akitsu Expressway (Chikage-Nishi, Inaho,
                        Shion and Asanagi interchanges) can be driven too.
  Ferry               : while the ship is alongside a gangway is laid to the pier: walk up it
                        (the fare is taken as you step aboard). Walk the deck, look at a bench
                        and press E to sit / stand. After berthing walk down the gangway.
                        Harbour ferry, high-speed ferry and car ferry.
  Scheduled flight    : at the stand of Chikage or Minamijima airport, walk up the passenger
                        stairs to the front left door (the fare is taken at the door), walk the
                        aisle, look at a free seat and press E: the flight leaves once you sit.
                        After landing and parking, stand up and walk down the stairs.
  Light aircraft      : look at it on the apron and press E. Shift/Ctrl power, W/S pitch, A/D
                        roll, Q/E rudder, F/R flaps, Space brakes, V camera, E (stopped) get out.

SHOPS
  In the capital's old town (Furuichi) and the regional towns (Shion, Yunosawa Onsen, Yukimi,
  ...) about half the small shops can be walked into (convenience store, cafe, general shop;
  generic procedural fit-out, no names or brands). Nothing can be bought yet.

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
  * Akitsu is fictional. Numbers (fares, service frequency, pay, fish sizes, omikuji odds,
    vehicle / aircraft figures, climate) are game values, not statistics or real
    specifications. Trains, cars, aircraft, ships and stations are generic procedural
    models, not real types.
  * Not implemented in Akitsu: level crossings (railways run on viaducts, embankments, in
    cuttings or tunnels), expressways and road tunnels, individual trees in the
    countryside (forests are a canopy surface), falling snow (in winter snow only lies),
    interiors in the regional towns. People and traffic appear only around the player.
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
  textures" turns them off. Akitsu uses none of these data.

LICENCES
  See the LICENSES folder (raylib: zlib License; BIZ UDPGothic: SIL OFL 1.1, subset).
