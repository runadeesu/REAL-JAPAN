PROJECT: REAL JAPAN  —  Android build v1.0.0 (experimental)
======================================================================

The same game as the Windows build (the fictional country Akitsu and the real-data Shibuya
slice from PLATEAU), running on OpenGL ES 3.0 with on-screen touch controls.

■ Honest note first
  - This build has not been run on a real phone or tablet yet. What was checked: the same
    renderer on OpenGL ES 3.0 under Linux (Mesa), and every shader validated as GLSL ES 3.00.
    It may fail to install, fail to start, or run slowly.
  - Needs Android 7.0 or later, 64-bit ARM (arm64-v8a) and an OpenGL ES 3.0 GPU. 32-bit and x86
    devices (most emulators) are not supported.
  - Signed with a public debug key kept in the repository (not a Google Play release).

■ Files
  RealJapan-1.0.0-android-arm64-full.apk   everything in this one file (about 560 MB)

■ Installing (either way)
  A. On the phone: save the APK, tap it in a file manager and allow "install unknown apps".
  B. From a PC: enable USB debugging and run  adb install RealJapan-1.0.0-android-arm64-full.apk
  Update the same way (the same signature keeps your saves).
  (For developers, tools/package_android.py can also make a main APK plus split data APKs.)
  Free space needed: about 600 MB (the data is read from the APKs; nothing is copied on start).

■ Touch controls
  Finger on the left half    : a joystick where you put it (push to the rim to run)
  Drag on the right half     : look around
  Tap on the right half      : use / examine what the crosshair is on (sit, board, doors, shop counters)
  Buttons bottom right       : use, jump, view ... (they change: handbrake and horn while driving,
                               throttle/flaps/rudder in the light aircraft, notch and emergency brake
                               as a train driver, reel when fishing, omikuji/goshuin at shrines,
                               refuel at a fuel station, notes 1-6 / band / put away with a guitar)
  Buttons top right          : the phone (map, wallet, jobs ...) and the menu
  Back button                : same as Esc
  In menus and on the phone, tap the buttons; pinch with two fingers to zoom the map.
  A hardware keyboard, if connected, takes the same keys as on Windows.
  A game controller (Bluetooth etc.) works too, mapped as in the Windows README.

■ Picture (phone defaults)
  The game draws 720 lines and the system scales them to the screen; view distance 800 m,
  post effects (SSAO, bloom, reflections) off, shadows on. Change them in Settings.

■ Your own 3D characters
  Copy binary FBX files (a Mixamo-style skeleton) over USB into
  internal storage/Android/data/<this app>/files/characters and start the game (converted the first
  time; at most 8). See README_en.txt of the Windows build.

■ Saves
  In the app's private storage (removed when the app is uninstalled). The autosave slot is
  written whenever the app is paused (home button and so on).

■ Content, notes, credits
  As in the Windows build's README_en.txt (full status in the repository's docs/STATUS.md).
  - Akitsu is fictional; fares, tolls, prices and pay are game values.
  - The Shibuya world is made from MLIT PLATEAU and GSI elevation tiles (see the Windows
    build's LICENSES\DATA_SOURCES.txt).
  - Uses raylib (zlib License) and BIZ UDPGothic (SIL OFL 1.1, subset).
