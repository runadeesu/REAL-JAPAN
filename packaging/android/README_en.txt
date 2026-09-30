PROJECT: REAL JAPAN  —  Android development build v0.7.0 (experimental)
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
  RealJapan-0.7.0-android-arm64.apk        the program, settings, languages, fonts, maps
  RealJapan-0.7.0-android-data01.apk ...   the city data, as split APKs under 30 MB each
  The main APK alone shows "World data is missing": install all of them together.

■ Installing (either way)
  A. On the phone: with an installer app for split APKs (e.g. SAI, Split APKs Installer),
     select the main APK and every data APK and install them in one go. Tapping them one by one
     in a file manager does not work. Allow "install unknown apps" for that installer if asked.
  B. From a PC: enable USB debugging, put all the APKs in one folder and run
       adb install-multiple RealJapan-0.7.0-android-*.apk
  Update the same way (all files at once; the same signature keeps your saves).
  Free space needed: about 600 MB (the data is read from the APKs; nothing is copied on start).

■ Touch controls
  Finger on the left half    : a joystick where you put it (push to the rim to run)
  Drag on the right half     : look around
  Tap on the right half      : use / examine what the crosshair is on (sit, board, doors, shop counters)
  Buttons bottom right       : use, jump, view ... (they change: handbrake and horn while driving,
                               throttle/flaps/rudder in the light aircraft, notch and emergency brake
                               as a train driver, reel when fishing, omikuji/goshuin at shrines)
  Buttons top right          : the phone (map, wallet, jobs ...) and the menu
  Back button                : same as Esc
  In menus and on the phone, tap the buttons; pinch with two fingers to zoom the map.
  A hardware keyboard, if connected, takes the same keys as on Windows.

■ Picture (phone defaults)
  The game draws 720 lines and the system scales them to the screen; view distance 800 m,
  post effects (SSAO, bloom, reflections) off, shadows on. Change them in Settings.

■ Saves
  In the app's private storage (removed when the app is uninstalled). The autosave slot is
  written whenever the app is paused (home button and so on).

■ Content, notes, credits
  As in the Windows build's README_en.txt (full status in the repository's docs/STATUS.md).
  - Akitsu is fictional; fares, tolls, prices and pay are game values.
  - The Shibuya world is made from MLIT PLATEAU and GSI elevation tiles (see the Windows
    build's LICENSES\DATA_SOURCES.txt).
  - Uses raylib (zlib License) and BIZ UDPGothic (SIL OFL 1.1, subset).
