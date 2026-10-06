PROJECT: REAL JAPAN  —  development build v0.8.0
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
  Installer (RealJapan-0.8.0-win64-setup.exe, one file)
    Run it: it installs for you into %LOCALAPPDATA%\Programs\RealJapan (no administrator
    rights). Start from the Start menu or the desktop ("PROJECT REAL JAPAN"). Uninstall from
    Settings -> Apps (saves and settings are kept).
  ZIP (RealJapan-0.8.0-win64.zip, one file)
  1. Extract the ZIP anywhere (e.g. your Desktop).
  2. Double-click RealJapan.exe. No extra runtime (DLL) is needed.
     This development build is not signed: if Windows shows "Windows protected your PC",
     choose "More info" -> "Run anyway".
  3. Keep the "data" folder next to RealJapan.exe.

REQUIREMENTS
  OS      : Windows 10 / 11, 64-bit (x64)
  GPU     : OpenGL 3.3. A 2016-or-later discrete GPU or a recent integrated GPU is
            recommended (if it is slow, turn off post-processing / shadows in Settings)
  Memory  : 8 GB or more      Disk : about 1 GB
  Sound   : speakers / headphones (all sound is synthesised in code; Settings -> Volume)

GAME CONTROLLER
  Xbox-style and DualShock-style controllers work (plug one in and the on-screen help switches).
  Left stick: move (all the way: run)   Right stick: look   X: use (E)   Y: camera (V)
  A: jump   B: crouch   BACK: phone   START: menu   D-pad: arrow keys
  Driving: RT throttle, LT brake, A handbrake, B horn, LB fuel station   Flying: RT/LT power
  Guitar: A B X Y LB RB LT RT play eight notes, L3 the band, R3 put it away
  Menus and the phone: move the pointer with a stick or the D-pad, A selects, B goes back

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
                        crossings (lamps, bell and barriers as a train comes; lowered barriers
                        stop you and your car, and a train stops short of a crossing with
                        someone on it).
                        Lines: Chikage Loop, Akitsu Main Line (Nishigaoka - Shion - Asanagi),
                        Akitsu Shinkansen (Chikage-Chuo - Chikage-Kita - Shion - Shin-Asanagi),
                        Yukimi Shinkansen (Chikage-Kita - Yunosawa Onsen - Yukimi).
  Train driver        : phone "Work" app -> train driver. W/S notches (5 power, 7 brake),
                        Space emergency brake, stop near the mark and press E to open the
                        doors. ATS protection.
  Cars                : look at the door of a stopped car and press E. W/S throttle, brake /
                        reverse, A/D steer, Space handbrake, H horn, V chase / driver's seat,
                        E (stopped) get out. The Akitsu Expressway (Chikage-Nishi, Inaho,
                        Shion and Asanagi interchanges) can be driven too: the toll is taken
                        ETC-style at the exit plaza by the distance from the entry plaza
                        (game values); Inaho PA has a car park and a shop.
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
  generic procedural fit-out, no names or brands; see-through glass). Look at the counter and
  press E (or click) to buy: number keys or a click on an item. The goods and prices are game
  values; what you buy is kept in the save and listed in the phone's wallet.

DAILY LIFE (v0.8.0)
  Body     : hunger and thirst fall with time (at zero you cannot run); rain soaks your clothes.
             Eat, drink and use what you bought from the phone's Bag app (umbrella, flashlight and
             batteries, notebook, towel).
  Home     : rent a studio flat in the phone's Home app (rent ¥65,000 per 30 days, a game value;
             a house on the map). Look at the bed and press E to sleep to 7:00 (a nap by day).
  Phone    : 19 apps - messages, phone (forecast, speaking clock), transit, taxi, online shop
             (next day, to your flat's parcel box), food delivery, hotel, flights, camera, music,
             SNS and more (all fictional, in-game).
  People   : look at a passer-by and press E to talk (fixed text, not AI); residents remember
             you; clerks remember regulars; street vending machines (estimated) sell drinks.
  Car      : fuel and damage; at a fuel station (parking area, towns) press F to fill up or mend;
             road service from the Bag app.
  Music    : take out a guitar (general shop) from the Bag app and play with keys 1-8; N calls the
             band; people may stop and tip. J puts it away.
  Also     : schools (look at one on a weekday 8:30-15:30 and press E to sit in on a class),
             swimming, regional weather, cherry blossom / rainy season / typhoons, captions for
             station and on-board announcements.

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
  game tuning values. The guitar strings and the phone's music (composed as it runs) are
  synthesised too. There are no voices (announcements are captions, talk is speech bubbles).

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
  * In Akitsu you can enter the stations, some shops and your flat only; the taxi ride, hotel
    rooms and classrooms are not shown (time moves on). People and traffic appear only around
    the player. Rents, fares and tips are game values.
  * In Shibuya, estimated content (crossings, signals, street lights, utility poles on roads
    PLATEAU does not cover, near-field building detail, lit windows at night) is labelled as
    estimated in the game.
  * Not implemented: voices, free conversation by a language model (talk is fixed text),
    building interiors in Shibuya other than the verified underground mall (no public source,
    so not invented), real data beyond Shibuya, GI / ray tracing, photo-real quality (not
    reached), code signing. See docs/STATUS.md in the repository for every item.
  * Tested on Linux and Wine with software OpenGL; not yet on real Windows PCs / GPUs.
  * An Android build (experimental, not yet run on a device) is provided separately.

YOUR OWN 3D CHARACTERS
  Put rigged characters as binary FBX files (a Mixamo-style skeleton) into the "characters" folder
  next to RealJapan.exe (or %APPDATA%\RealJapan\characters) and start the game. They are converted
  once in the background (tens of seconds); then choose yours in Settings -> Your character. You see
  it in third person (V), and people nearby, passengers and clerks use them too.
  For example: pick a character on mixamo.com and download it as FBX Binary with the skin.
  No characters come with this package (their rights belong to their authors).

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
