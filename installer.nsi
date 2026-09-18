; NSIS installer script for ECM Management System.
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
; Installs both the exe and dll\Transit.dll under it into Program Files, keeping
; the same relative layout (dll\Transit.dll next to the exe) the app's own
; TRANSIT_DLL_PATH ("dll\\Transit.dll", connection.c) expects at runtime.

!define APP_NAME "ECM Management System"
!define COMPANY_NAME "lankipolo123"
!define APP_VERSION "1.0.0.0"
!define EXE_NAME "digital_noise_config_multi.exe"
!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP_NAME}"

Name "${APP_NAME}"
OutFile "ECM_Management_System_Setup.exe"
InstallDir "$PROGRAMFILES64\${APP_NAME}"
InstallDirRegKey HKLM "Software\${COMPANY_NAME}\${APP_NAME}" "InstallDir"
RequestExecutionLevel admin
Icon "src\app.ico"
UninstallIcon "src\app.ico"

Page directory
Page instfiles
UninstPage uninstConfirm
UninstPage instfiles

Section "Install"
    SetOutPath "$INSTDIR"
    File "${EXE_NAME}"

    SetOutPath "$INSTDIR\dll"
    File "dll\Transit.dll"

    WriteRegStr HKLM "Software\${COMPANY_NAME}\${APP_NAME}" "InstallDir" "$INSTDIR"

    CreateDirectory "$SMPROGRAMS\${APP_NAME}"
    CreateShortCut "$SMPROGRAMS\${APP_NAME}\${APP_NAME}.lnk" "$INSTDIR\${EXE_NAME}"
    CreateShortCut "$SMPROGRAMS\${APP_NAME}\Uninstall.lnk" "$INSTDIR\Uninstall.exe"
    CreateShortCut "$DESKTOP\${APP_NAME}.lnk" "$INSTDIR\${EXE_NAME}"

    WriteUninstaller "$INSTDIR\Uninstall.exe"

    WriteRegStr HKLM "${UNINST_KEY}" "DisplayName" "${APP_NAME}"
    WriteRegStr HKLM "${UNINST_KEY}" "DisplayVersion" "${APP_VERSION}"
    WriteRegStr HKLM "${UNINST_KEY}" "Publisher" "${COMPANY_NAME}"
    WriteRegStr HKLM "${UNINST_KEY}" "UninstallString" "$INSTDIR\Uninstall.exe"
    WriteRegStr HKLM "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
    WriteRegStr HKLM "${UNINST_KEY}" "DisplayIcon" "$INSTDIR\${EXE_NAME}"
    WriteRegDWORD HKLM "${UNINST_KEY}" "NoModify" 1
    WriteRegDWORD HKLM "${UNINST_KEY}" "NoRepair" 1
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

    DeleteRegKey HKLM "${UNINST_KEY}"
    DeleteRegKey HKLM "Software\${COMPANY_NAME}\${APP_NAME}"
SectionEnd
