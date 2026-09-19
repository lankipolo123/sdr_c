; NSIS installer script for ECM Controller.
; Build with: makensis installer.nsi
;
; Expects, in this same directory:
;   digital_noise_config_multi.exe  (built via build.bat/the mingw command in README)
;   dll\Transit.dll                 (the proprietary vendor DLL - never committed to
;                                     git, see .gitignore's own comment on dll/ -
;                                     whoever builds the installer needs their own
;                                     copy of it locally, same as building the app
;                                     itself does)
;   src\app.ico                     (installer/uninstaller icon, same mark as the app)
;
; Installs both the exe and dll\Transit.dll under it, keeping the same
; relative layout (dll\Transit.dll next to the exe) the app's own
; TRANSIT_DLL_PATH ("dll\\Transit.dll", connection.c) expects at runtime.
;
; Installs to a plain top-level C:\ECM Controller, NOT Program
; Files - direct consequence of a real bug found in testing: the app
; writes its own .ini, branding.bmp, and branding\icon.ico's [Branding]
; state straight next to the exe at runtime (get_ini_path()/
; get_branding_bmp_path()/get_branding_icon_path() in main.c - portable,
; no-installer-required by design). Program Files needs admin rights to
; write to, so once installed there, every one of those writes silently
; fails unless the app is run elevated every single time - not just
; custom branding, ALL persisted settings (port/baud, per-channel mode/
; level/output, uptime) stop saving. The root of C:\ is writable by a
; standard user without elevation on a normal Windows install (unlike
; Program Files/Windows) - direct request for a plain, top-level folder
; instead of one tucked inside a user-profile folder (Documents,
; %LOCALAPPDATA%\Programs) that a corporate/shared machine could
; redirect or restrict differently per account.
!define APP_NAME "ECM Controller"
!define COMPANY_NAME "lankipolo123"
!define APP_VERSION "1.0.0.0"
!define EXE_NAME "digital_noise_config_multi.exe"
!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP_NAME}"

Name "${APP_NAME}"
OutFile "ECM_Controller_Setup.exe"
InstallDir "C:\${APP_NAME}"
InstallDirRegKey HKCU "Software\${COMPANY_NAME}\${APP_NAME}" "InstallDir"
RequestExecutionLevel user
Icon "src\app.ico"
UninstallIcon "src\app.ico"

; Plain `Page instfiles` alone never shows a real "Installation Complete"
; screen - just the raw install log with a Close button, easy to read as
; "did this actually finish?" (direct report). Modern UI 2's finish page
; is the standard fix: a dedicated success screen, with an optional
; "run the app now" checkbox.
!include "MUI2.nsh"
!define MUI_FINISHPAGE_RUN "$INSTDIR\${EXE_NAME}"
!define MUI_FINISHPAGE_RUN_TEXT "Launch ${APP_NAME} now"

!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

Section "Install"
    SetOutPath "$INSTDIR"
    File "${EXE_NAME}"

    SetOutPath "$INSTDIR\dll"
    File "dll\Transit.dll"

    WriteRegStr HKCU "Software\${COMPANY_NAME}\${APP_NAME}" "InstallDir" "$INSTDIR"

    CreateDirectory "$SMPROGRAMS\${APP_NAME}"
    CreateShortCut "$SMPROGRAMS\${APP_NAME}\${APP_NAME}.lnk" "$INSTDIR\${EXE_NAME}"
    CreateShortCut "$SMPROGRAMS\${APP_NAME}\Uninstall.lnk" "$INSTDIR\Uninstall.exe"
    CreateShortCut "$DESKTOP\${APP_NAME}.lnk" "$INSTDIR\${EXE_NAME}"

    WriteUninstaller "$INSTDIR\Uninstall.exe"

    WriteRegStr HKCU "${UNINST_KEY}" "DisplayName" "${APP_NAME}"
    WriteRegStr HKCU "${UNINST_KEY}" "DisplayVersion" "${APP_VERSION}"
    WriteRegStr HKCU "${UNINST_KEY}" "Publisher" "${COMPANY_NAME}"
    WriteRegStr HKCU "${UNINST_KEY}" "UninstallString" "$INSTDIR\Uninstall.exe"
    WriteRegStr HKCU "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
    WriteRegStr HKCU "${UNINST_KEY}" "DisplayIcon" "$INSTDIR\${EXE_NAME}"
    WriteRegDWORD HKCU "${UNINST_KEY}" "NoModify" 1
    WriteRegDWORD HKCU "${UNINST_KEY}" "NoRepair" 1
SectionEnd

Section "Uninstall"
    Delete "$INSTDIR\${EXE_NAME}"
    Delete "$INSTDIR\dll\Transit.dll"
    Delete "$INSTDIR\Uninstall.exe"
    RMDir "$INSTDIR\dll"
    RMDir "$INSTDIR"

    Delete "$SMPROGRAMS\${APP_NAME}\${APP_NAME}.lnk"
    Delete "$SMPROGRAMS\${APP_NAME}\Uninstall.lnk"
    RMDir "$SMPROGRAMS\${APP_NAME}"
    Delete "$DESKTOP\${APP_NAME}.lnk"

    DeleteRegKey HKCU "${UNINST_KEY}"
    DeleteRegKey HKCU "Software\${COMPANY_NAME}\${APP_NAME}"
SectionEnd
