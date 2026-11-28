Unicode true
!include "MUI2.nsh"
!include "x64.nsh"
!include "WinVer.nsh"

Name "Navidog"
OutFile "${OUTPUT_FILE}"
InstallDir "$LOCALAPPDATA\Programs\Navidog"
InstallDirRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Navidog" "InstallLocation"
RequestExecutionLevel user
ManifestSupportedOS all
SetCompressor /SOLID lzma
!define MUI_ICON "${ICON_FILE}"
!define MUI_UNICON "${ICON_FILE}"

!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"
!insertmacro MUI_LANGUAGE "SimpChinese"

Function .onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP "Navidog requires 64-bit Windows." /SD IDOK
    Abort
  ${EndIf}
!if "${WINDOWS_EDITION}" == "8"
  ${IfNot} ${AtLeastWin8}
!else if "${WINDOWS_EDITION}" == "10"
  ${IfNot} ${AtLeastBuild} 17763
!else if "${WINDOWS_EDITION}" == "11"
  ${IfNot} ${AtLeastWin11}
!else
  ${IfNot} ${AtLeastWin7}
!endif
    MessageBox MB_ICONSTOP "Use the Navidog package for this version of Windows." /SD IDOK
    Abort
  ${EndIf}
FunctionEnd

Section "Navidog"
  SetShellVarContext current
  SetOutPath "$INSTDIR\application"
  File /r "${PAYLOAD_DIR}\*"
  WriteUninstaller "$INSTDIR\Uninstall.exe"
  CreateShortcut "$SMPROGRAMS\Navidog.lnk" "$INSTDIR\application\navidog.exe"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Navidog" "DisplayName" "Navidog"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Navidog" "DisplayVersion" "${APP_VERSION}"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Navidog" "DisplayIcon" "$INSTDIR\application\navidog.exe"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Navidog" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Navidog" "UninstallString" '$\"$INSTDIR\Uninstall.exe$\"'
SectionEnd

Section "Uninstall"
  SetShellVarContext current
  Delete "$SMPROGRAMS\Navidog.lnk"
  DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Navidog"
  RMDir /r "$INSTDIR\application"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"
SectionEnd
