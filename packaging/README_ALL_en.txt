PROJECT: REAL JAPAN  —  development build v{VER} (the Windows and Android builds in one file)

Contents
  Windows/RealJapan-{VER}-win64-setup.exe   installer for Windows 10/11 (64-bit), everything in one file
  Android/RealJapan-{VER}-android-arm64.apk  APK for Android 7.0+ (arm64, OpenGL ES 3.0), everything in one file

Windows
  1. Run setup.exe (no administrator rights needed; it installs into %LOCALAPPDATA%\Programs\RealJapan).
     It is not code-signed: if SmartScreen warns, choose "More info" -> "Run anyway".
  2. Start it from the Start menu or the desktop ("PROJECT REAL JAPAN").
  Uninstall from Settings -> Apps. Saves and settings (%APPDATA%\RealJapan) are kept.
  Prefer a ZIP? Extract RealJapan-{VER}-win64.zip from the GitHub release and run RealJapan.exe.

Android (experimental; not yet tested on a real device)
  Copy the APK to the phone, open it and allow installing from unknown sources,
  or from a PC: adb install Android/RealJapan-{VER}-android-arm64.apk
  It is about 560 MB; make sure there is room.

Notes
  Your own 3D characters (FBX): put them into the characters folder (Settings opens it).
  Akitsu is a fictional country modelled on Japanese landscapes; no real place, shop or person is depicted.
  Prices, fares and rents are game values. Talk, SNS posts and announcements are fixed text, not AI or voices.
  The Shibuya world uses PLATEAU (MLIT) and GSI data (see LICENSES).
