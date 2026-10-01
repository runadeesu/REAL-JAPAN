; One-file installer for PROJECT: REAL JAPAN (Windows x64).
;   makensis -DVER=0.8.0 -DSRC=dist/RealJapan-0.8.0-win64 -DICON=client/res/realjapan.ico -DOUT=dist/RealJapan-0.8.0-win64-setup.exe installer.nsi
; Installs for the current user (no administrator rights) into %LOCALAPPDATA%\Programs\RealJapan,
; with Start menu and desktop shortcuts and an uninstaller. The saves and settings
; (%APPDATA%\RealJapan) are not touched by installing, updating or uninstalling.
Unicode true
SetCompressor /SOLID lzma
SetCompressorDictSize 64
RequestExecutionLevel user

!ifndef VER
  !define VER "0.8.0"
!endif
!include "MUI2.nsh"

Name "PROJECT: REAL JAPAN ${VER}"
OutFile "${OUT}"
InstallDir "$LOCALAPPDATA\Programs\RealJapan"
InstallDirRegKey HKCU "Software\RealJapan" "InstallDir"
BrandingText "PROJECT: REAL JAPAN ${VER}"
VIProductVersion "${VER}.0"
VIAddVersionKey /LANG=0 "ProductName" "PROJECT: REAL JAPAN"
VIAddVersionKey /LANG=0 "FileDescription" "PROJECT: REAL JAPAN installer"
VIAddVersionKey /LANG=0 "FileVersion" "${VER}"
VIAddVersionKey /LANG=0 "ProductVersion" "${VER}"
VIAddVersionKey /LANG=0 "LegalCopyright" "PROJECT: REAL JAPAN"

!define MUI_ICON "${ICON}"
!define MUI_UNICON "${ICON}"
!define MUI_FINISHPAGE_RUN "$INSTDIR\RealJapan.exe"
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "Japanese"
!insertmacro MUI_LANGUAGE "English"

Section "PROJECT: REAL JAPAN" SecMain
  SectionIn RO
  ; (an update replaces the program and its data; the old world cells go first)
  RMDir /r "$INSTDIR\data"
  SetOutPath "$INSTDIR"
  File /r "${SRC}\*"
  WriteUninstaller "$INSTDIR\Uninstall.exe"
  WriteRegStr HKCU "Software\RealJapan" "InstallDir" "$INSTDIR"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\RealJapan" "DisplayName" "PROJECT: REAL JAPAN"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\RealJapan" "DisplayVersion" "${VER}"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\RealJapan" "DisplayIcon" "$INSTDIR\RealJapan.exe"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\RealJapan" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\RealJapan" "NoModify" 1
  WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\RealJapan" "NoRepair" 1
  CreateDirectory "$SMPROGRAMS\PROJECT REAL JAPAN"
  CreateShortcut "$SMPROGRAMS\PROJECT REAL JAPAN\PROJECT REAL JAPAN.lnk" "$INSTDIR\RealJapan.exe"
  CreateShortcut "$SMPROGRAMS\PROJECT REAL JAPAN\Uninstall.lnk" "$INSTDIR\Uninstall.exe"
  CreateShortcut "$DESKTOP\PROJECT REAL JAPAN.lnk" "$INSTDIR\RealJapan.exe"
SectionEnd

Section "Uninstall"
  Delete "$DESKTOP\PROJECT REAL JAPAN.lnk"
  RMDir /r "$SMPROGRAMS\PROJECT REAL JAPAN"
  RMDir /r "$INSTDIR\data"
  RMDir /r "$INSTDIR\LICENSES"
  Delete "$INSTDIR\RealJapan.exe"
  Delete "$INSTDIR\README_ja.txt"
  Delete "$INSTDIR\README_en.txt"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"
  DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\RealJapan"
  DeleteRegKey HKCU "Software\RealJapan"
SectionEnd
