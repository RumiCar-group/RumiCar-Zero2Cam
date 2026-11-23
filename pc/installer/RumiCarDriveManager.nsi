; Installer of the RumiCar drive manager (RumiCarDriveManager.exe), for the
; current user only: no administrator rights, installed under
; %LOCALAPPDATA%\Programs, a Start menu shortcut, and an entry in Windows'
; installed apps (HKCU) through which it is removed (docs 7.3).
;
; Built by CI (pc.yml):
;   makensis -INPUTCHARSET UTF8 -DVERSION=X.Y.Z -DSOURCE=<published exe> -DOUTFILE=<setup.exe> RumiCarDriveManager.nsi
;
; Installing over an installed version replaces it (same folder).  The
; recordings and settings in %USERPROFILE%\RumiCarData are never touched,
; neither by installing nor by removing.

Unicode true
ManifestDPIAware true
SetCompressor /SOLID lzma

!include "MUI2.nsh"
!include "FileFunc.nsh"
!include "LogicLib.nsh"

!ifdef VERSION & SOURCE & OUTFILE
!else
  !error "makensis -INPUTCHARSET UTF8 -DVERSION=X.Y.Z -DSOURCE=<RumiCarDriveManager.exe> -DOUTFILE=<setup.exe> RumiCarDriveManager.nsi"
!endif

!define APP_NAME "RumiCar 走行管理"
!define APP_EXE "RumiCarDriveManager.exe"
!define UNINSTALL_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\RumiCarDriveManager"

Name "${APP_NAME} ${VERSION}"
OutFile "${OUTFILE}"
RequestExecutionLevel user
InstallDir "$LOCALAPPDATA\Programs\RumiCar Drive Manager"
InstallDirRegKey HKCU "${UNINSTALL_KEY}" "InstallLocation"
BrandingText "RumiCar Zero2Cam ${VERSION}"
ShowInstDetails show
ShowUninstDetails show

!define MUI_ABORTWARNING
!define MUI_COMPONENTSPAGE_NODESC
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_RUN "$INSTDIR\${APP_EXE}"
!define MUI_FINISHPAGE_RUN_TEXT "走行管理アプリを起動する"
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "Japanese"

VIProductVersion "${VERSION}.0"
VIFileVersion "${VERSION}.0"
VIAddVersionKey /LANG=${LANG_JAPANESE} "ProductName" "RumiCar Drive Manager"
VIAddVersionKey /LANG=${LANG_JAPANESE} "ProductVersion" "${VERSION}"
VIAddVersionKey /LANG=${LANG_JAPANESE} "FileVersion" "${VERSION}"
VIAddVersionKey /LANG=${LANG_JAPANESE} "FileDescription" "${APP_NAME} インストーラー"
VIAddVersionKey /LANG=${LANG_JAPANESE} "CompanyName" "RumiCar"
VIAddVersionKey /LANG=${LANG_JAPANESE} "LegalCopyright" "RumiCar"

; The application cannot be replaced or removed while it runs: the user is
; asked to close it (a silent run gives up with exit code 2).
!macro REMOVE_APP_EXE
  ${Do}
    Delete "$INSTDIR\${APP_EXE}"
    ${IfNot} ${FileExists} "$INSTDIR\${APP_EXE}"
      ${Break}
    ${EndIf}
    MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION "${APP_NAME}が起動しています。アプリを終了してから「再試行」を押してください。" /SD IDCANCEL IDRETRY app_retry
    Abort "${APP_NAME}が起動しているため中止しました"
    app_retry:
  ${Loop}
!macroend

Section "走行管理アプリ" SecApp
  SectionIn RO
  SetOutPath "$INSTDIR"
  !insertmacro REMOVE_APP_EXE
  File "/oname=${APP_EXE}" "${SOURCE}"
  WriteUninstaller "$INSTDIR\uninstall.exe"
  CreateShortcut "$SMPROGRAMS\${APP_NAME}.lnk" "$INSTDIR\${APP_EXE}"

  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayName" "${APP_NAME}"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "Publisher" "RumiCar"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayIcon" "$INSTDIR\${APP_EXE}"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegStr HKCU "${UNINSTALL_KEY}" "QuietUninstallString" '"$INSTDIR\uninstall.exe" /S'
  WriteRegStr HKCU "${UNINSTALL_KEY}" "URLInfoAbout" "https://github.com/RumiCar-group/RumiCar-Zero2Cam"
  WriteRegDWORD HKCU "${UNINSTALL_KEY}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINSTALL_KEY}" "NoRepair" 1
  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  IntFmt $0 "0x%08X" $0
  WriteRegDWORD HKCU "${UNINSTALL_KEY}" "EstimatedSize" "$0"
SectionEnd

Section "デスクトップにショートカットを作る" SecDesktop
  CreateShortcut "$DESKTOP\${APP_NAME}.lnk" "$INSTDIR\${APP_EXE}"
SectionEnd

Section "Uninstall"
  !insertmacro REMOVE_APP_EXE
  Delete "$INSTDIR\uninstall.exe"
  RMDir "$INSTDIR"   ; only when empty: nothing else in the folder is removed
  Delete "$SMPROGRAMS\${APP_NAME}.lnk"
  Delete "$DESKTOP\${APP_NAME}.lnk"
  DeleteRegKey HKCU "${UNINSTALL_KEY}"
SectionEnd
