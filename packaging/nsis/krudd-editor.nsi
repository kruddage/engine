; SPDX-License-Identifier: GPL-2.0-or-later
;
; NSIS installer for the native KRUDD editor (krudd_qt) — the Windows
; equivalent of the Flatpak editor distribution (packaging/flatpak/), for #694.
;
; This script packages an already-staged payload directory into a single
; `krudd-editor-setup.exe`. It does NOT build anything: CI (or a local build)
; produces `krudd_qt.exe`, runs `windeployqt` to gather the Qt runtime + the
; `platforms/qwindows.dll` plugin beside it, drops the Vulkan loader
; (`vulkan-1.dll`) and the mingw runtime DLLs in the same folder, and points
; this script at that folder via /DKRUDD_STAGE_DIR. See packaging/nsis/README.md
; and .github/workflows/windows-build.yml for how the payload is assembled.
;
; The build feeds these in with `makensis /D<name>=<value>`; the !ifndef
; defaults let `makensis packaging/nsis/krudd-editor.nsi` run for a quick local
; smoke test from the repo root after a manual stage.

!ifndef KRUDD_VERSION
  !define KRUDD_VERSION "0.0.0-dev"
!endif
!ifndef KRUDD_STAGE_DIR
  !define KRUDD_STAGE_DIR "stage"
!endif
!ifndef KRUDD_OUTFILE
  !define KRUDD_OUTFILE "krudd-editor-setup.exe"
!endif

!define APP_NAME "KRUDD Editor"
!define APP_EXE "krudd_qt.exe"
!define APP_PUBLISHER "kruddage"
!define APP_URL "https://github.com/kruddage/engine"
; The Add/Remove Programs registry key. A stable, space-free id so an upgrade
; installs over the previous version instead of stacking a second entry.
!define APP_REGKEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\KRUDDEditor"

Name "${APP_NAME}"
OutFile "${KRUDD_OUTFILE}"
Unicode true
; 64-bit only: the editor's Vulkan backend and the mingw-w64 Qt runtime are
; x86-64, so install under the 64-bit Program Files rather than the WOW64 one.
InstallDir "$PROGRAMFILES64\${APP_NAME}"
InstallDirRegKey HKLM "${APP_REGKEY}" "InstallLocation"
RequestExecutionLevel admin
SetCompressor /SOLID lzma

!include "MUI2.nsh"

; An icon is optional — pass /DKRUDD_ICON=<path.ico> to brand the installer,
; its Add/Remove Programs entry, and the shortcuts. Without it NSIS uses its
; own default and the build still succeeds.
!ifdef KRUDD_ICON
  !define MUI_ICON "${KRUDD_ICON}"
  !define MUI_UNICON "${KRUDD_ICON}"
!endif

!define MUI_ABORTWARNING
!define MUI_FINISHPAGE_RUN "$INSTDIR\${APP_EXE}"
!define MUI_FINISHPAGE_RUN_TEXT "Launch ${APP_NAME}"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

Section "Install"
  SetOutPath "$INSTDIR"
  ; The whole staged payload: krudd_qt.exe, the Qt DLLs + plugin subdirs that
  ; windeployqt gathered, the Vulkan loader, and the mingw runtime DLLs.
  File /r "${KRUDD_STAGE_DIR}\*.*"

  ; Start Menu + Desktop shortcuts to the launcher.
  CreateDirectory "$SMPROGRAMS\${APP_NAME}"
  CreateShortcut "$SMPROGRAMS\${APP_NAME}\${APP_NAME}.lnk" "$INSTDIR\${APP_EXE}"
  CreateShortcut "$DESKTOP\${APP_NAME}.lnk" "$INSTDIR\${APP_EXE}"

  ; Uninstaller + Add/Remove Programs metadata.
  WriteUninstaller "$INSTDIR\Uninstall.exe"
  WriteRegStr HKLM "${APP_REGKEY}" "DisplayName" "${APP_NAME}"
  WriteRegStr HKLM "${APP_REGKEY}" "DisplayVersion" "${KRUDD_VERSION}"
  WriteRegStr HKLM "${APP_REGKEY}" "Publisher" "${APP_PUBLISHER}"
  WriteRegStr HKLM "${APP_REGKEY}" "URLInfoAbout" "${APP_URL}"
  WriteRegStr HKLM "${APP_REGKEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${APP_REGKEY}" "UninstallString" "$\"$INSTDIR\Uninstall.exe$\""
  WriteRegStr HKLM "${APP_REGKEY}" "QuietUninstallString" "$\"$INSTDIR\Uninstall.exe$\" /S"
  WriteRegDWORD HKLM "${APP_REGKEY}" "NoModify" 1
  WriteRegDWORD HKLM "${APP_REGKEY}" "NoRepair" 1
SectionEnd

Section "Uninstall"
  Delete "$DESKTOP\${APP_NAME}.lnk"
  Delete "$SMPROGRAMS\${APP_NAME}\${APP_NAME}.lnk"
  RMDir "$SMPROGRAMS\${APP_NAME}"

  ; Remove the installed tree. RMDir /r on $INSTDIR is safe here because the
  ; app installs into its own dedicated directory (never a shared prefix).
  RMDir /r "$INSTDIR"

  DeleteRegKey HKLM "${APP_REGKEY}"
SectionEnd
