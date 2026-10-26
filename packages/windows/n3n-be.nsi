; Copyright (C) Honey Bunny QT
; SPDX-License-Identifier: AGPL-3.0-only
;
; The installer of n3n-be for Windows (NSIS 3), built from the files of the
; release zip by packages/windows/installer.sh.  It puts the programs into
; Program Files, with the docs as their help, Start menu entries and an
; entry in "Apps" with the uninstaller; optionally starts the tray app with
; Windows, gets Wintun where there is no TAP driver, and installs the edge
; as a service (asking for a configuration if there is none, which it
; imports into %ProgramData%\n3n-be; "setup.exe /S /CONF=FILE" for one
; without asking).
;
; It knows earlier installs, its own and the zip's: it stops the services
; and closes the tray apps before it replaces the programs, points services
; that a zip installed elsewhere at the installed program (and so renews
; their firewall rule, tray file and rights), starts again those that ran,
; takes over a wintun.dll they had, and points "Start with Windows" at the
; installed tray app.  A newer version installed asks before a downgrade.
; The uninstaller removes the services, their firewall rules and the tray
; app's start with Windows; the configurations stay.

Unicode true
!include MUI2.nsh
!include LogicLib.nsh
!include x64.nsh
!include FileFunc.nsh
!include WordFunc.nsh
!include WinMessages.nsh
!include nsDialogs.nsh
!include Sections.nsh
!include StrFunc.nsh
${StrStr}

!ifndef VERSION
  !define VERSION "0.0.0"
!endif
!ifndef VERSION_NUM
  !define VERSION_NUM "0.0.0.0"
!endif
!ifndef SRCDIR
  !error "SRCDIR: the directory with the files of the release zip"
!endif
!ifndef OUTFILE
  !define OUTFILE "n3n-be-${VERSION}-windows-x86_64-setup.exe"
!endif

!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\n3n-be"
!define APP_KEY "Software\n3n-be"
!define RUN_KEY "Software\Microsoft\Windows\CurrentVersion\Run"
!define SERVICES_KEY "SYSTEM\CurrentControlSet\Services"
!define NET_CLASS_KEY "SYSTEM\CurrentControlSet\Control\Class\{4D36E972-E325-11CE-BFC1-08002BE10318}"
; the page on Windows of the docs: what the tray's Help opens too
!define HELP_PAGE "docs\quick_start\Windows.html"

Name "n3n-be"
OutFile "${OUTFILE}"
InstallDir "$PROGRAMFILES64\n3n-be"
RequestExecutionLevel admin
SetCompressor /SOLID lzma
BrandingText "n3n-be ${VERSION}"
ShowInstDetails show
ShowUninstDetails show

VIProductVersion "${VERSION_NUM}"
VIAddVersionKey "ProductName" "n3n-be"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "FileDescription" "n3n-be, a peer-to-peer VPN - installer"
VIAddVersionKey "LegalCopyright" "Honey Bunny QT and the n3n and n2n contributors; GNU AGPL and GPL"

Var PrevVersion         ; of an install before, "" if none
Var Welcome             ; what the welcome page says of it
Var ConfFile            ; a configuration chosen on its page
Var ConfField
Var TrayRan             ; a tray app was running and was closed
Var ProgramData         ; %ProgramData%, where the service's configuration is

!define MUI_ABORTWARNING
!define MUI_WELCOMEPAGE_TEXT "This installs n3n-be ${VERSION}, a peer-to-peer VPN, with the edge as a service \
and the tray app.$\r$\n$\r$\n$Welcome"
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${SRCDIR}\LICENSES\preferred\AGPL-3.0"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_COMPONENTS
Page custom ConfPage ConfLeave
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_RUN
!define MUI_FINISHPAGE_RUN_TEXT "Start the tray app"
!define MUI_FINISHPAGE_RUN_FUNCTION StartTray
!define MUI_FINISHPAGE_SHOWREADME
!define MUI_FINISHPAGE_SHOWREADME_TEXT "Open the help"
!define MUI_FINISHPAGE_SHOWREADME_NOTCHECKED
!define MUI_FINISHPAGE_SHOWREADME_FUNCTION OpenHelp
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"


; ---- helpers

; As the user who started the installer, not as the administrator it runs
; as: what Explorer starts runs as the one logged in
!macro AsUser what
  Exec '"$WINDIR\explorer.exe" "${what}"'
!macroend

Function StartTray
  !insertmacro AsUser "$INSTDIR\n3n-be-tray.exe"
FunctionEnd

Function OpenHelp
  !insertmacro AsUser "$INSTDIR\${HELP_PAGE}"
FunctionEnd

; The tray apps of this desktop get WM_CLOSE: they hold n3n-be-tray.exe
!macro CloseTrays un
Function ${un}CloseTrays
  StrCpy $R1 0
  loop:
    FindWindow $R0 "n3n-be-tray" ""
    StrCmp $R0 0 done
    StrCpy $TrayRan 1
    SendMessage $R0 ${WM_CLOSE} 0 0 /TIMEOUT=2000
    Sleep 300
    IntOp $R1 $R1 + 1
    IntCmp $R1 20 done
    Goto loop
  done:
FunctionEnd
!macroend
!insertmacro CloseTrays ""
!insertmacro CloseTrays "un."

; Whether service $R0 runs: $R9 1 or 0, by "RUNNING" in what sc.exe says
!macro ServiceRuns un
Function ${un}ServiceRuns
  nsExec::ExecToStack 'sc.exe query "$R0"'
  Pop $R8
  Pop $R8
  StrCpy $R9 0
  StrCpy $R6 0
  loop:
    StrCpy $R7 $R8 7 $R6
    StrCmp $R7 "" done
    StrCmp $R7 "RUNNING" 0 +3
    StrCpy $R9 1
    Goto done
    IntOp $R6 $R6 + 1
    Goto loop
  done:
FunctionEnd
!macroend
!insertmacro ServiceRuns ""
!insertmacro ServiceRuns "un."

; The session of service $R0 into $R6: n3n-be-edge is the session edge,
; n3n-be-edge@NAME the session NAME
!macro SessionOf un
Function ${un}SessionOf
  StrCpy $R6 "edge"
  StrCmp $R0 "n3n-be-edge" +2
  StrCpy $R6 $R0 "" 12
FunctionEnd
!macroend
!insertmacro SessionOf ""
!insertmacro SessionOf "un."

; The services of n3n-be (n3n-be-edge, n3n-be-edge@NAME) into the values
; of HKLM ${APP_KEY}\$R5, each with 1 if it runs: a list that stays as it
; is while the services change
!macro ListServices un
Function ${un}ListServices
  DeleteRegKey HKLM "${APP_KEY}\$R5"
  StrCpy $R4 0
  loop:
    EnumRegKey $R0 HKLM "${SERVICES_KEY}" $R4
    StrCmp $R0 "" done
    IntOp $R4 $R4 + 1
    StrCpy $R3 $R0 11
    StrCmp $R3 "n3n-be-edge" 0 loop
    StrCpy $R3 $R0 1 11
    StrCmp $R3 "" +2
    StrCmp $R3 "@" 0 loop
    Call ${un}ServiceRuns
    WriteRegDWORD HKLM "${APP_KEY}\$R5" $R0 $R9
    Goto loop
  done:
FunctionEnd
!macroend
!insertmacro ListServices ""
!insertmacro ListServices "un."

; The program of service $R0, from its ImagePath ("C:\...\n3n-be.exe" ...),
; into $R2; "" if it has none
Function ServiceProgram
  StrCpy $R2 ""
  ReadRegStr $R1 HKLM "${SERVICES_KEY}\$R0" "ImagePath"
  StrCpy $R3 $R1 1
  StrCmp $R3 '"' 0 done
  StrCpy $R1 $R1 "" 1
  StrCpy $R4 0
  loop:
    StrCpy $R3 $R1 1 $R4
    StrCmp $R3 "" done
    StrCmp $R3 '"' found
    IntOp $R4 $R4 + 1
    Goto loop
  found:
    StrCpy $R2 $R1 $R4
  done:
FunctionEnd

; A TAP adapter installed (tap-windows6): $R9 1 or 0
Function HasTap
  StrCpy $R9 0
  StrCpy $R4 0
  loop:
    EnumRegKey $R0 HKLM "${NET_CLASS_KEY}" $R4
    StrCmp $R0 "" done
    IntOp $R4 $R4 + 1
    ReadRegStr $R1 HKLM "${NET_CLASS_KEY}\$R0" "ComponentId"
    ${StrStr} $R2 $R1 "tap0901"
    StrCmp $R2 "" loop
    StrCpy $R9 1
  done:
FunctionEnd


; ---- what it installs

Section "n3n-be" SecMain
  SectionIn RO

  ; what runs from here, and from an install by a zip, stops first
  Call CloseTrays
  StrCpy $R5 "upgrade"
  Call ListServices
  StrCpy $R4 0
  stop:
    EnumRegValue $R0 HKLM "${APP_KEY}\upgrade" $R4
    StrCmp $R0 "" stopped
    IntOp $R4 $R4 + 1
    DetailPrint "stopping $R0"
    nsExec::Exec 'sc.exe stop "$R0"'
    Pop $R1
    StrCpy $R1 0
    wait:
      Call ServiceRuns
      StrCmp $R9 0 stop
      IntOp $R1 $R1 + 1
      IntCmp $R1 40 stop
      Sleep 500
      Goto wait
  stopped:

  ; an edge or supernode in a console holds the program: it has to go
  ${If} ${FileExists} "$INSTDIR\n3n-be.exe"
    retry:
    ClearErrors
    FileOpen $R0 "$INSTDIR\n3n-be.exe" a
    ${If} ${Errors}
      MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION "n3n-be.exe still runs from $INSTDIR - an edge or \
          supernode in a console?  Stop it (Ctrl-C), then Retry." /SD IDCANCEL IDRETRY retry
      Abort
    ${EndIf}
    FileClose $R0
  ${EndIf}

  SetOutPath "$INSTDIR"
  ; the docs of this version only
  RMDir /r "$INSTDIR\docs"
  File "${SRCDIR}\n3n-be.exe"
  File "${SRCDIR}\n3n-be-tray.exe"
  File "${SRCDIR}\README.txt"
  File /nonfatal "${SRCDIR}\n3n-be-ctl"
  File /nonfatal "${SRCDIR}\edge.conf.sample"
  File /nonfatal "${SRCDIR}\supernode.conf.sample"
  File /r "${SRCDIR}\LICENSES"
  File /nonfatal /r "${SRCDIR}\docs"

  ; a wintun.dll next to the program of a service from elsewhere (a zip)
  StrCpy $R5 0
  wintun:
    EnumRegValue $R0 HKLM "${APP_KEY}\upgrade" $R5
    StrCmp $R0 "" wintun_done
    IntOp $R5 $R5 + 1
    Call ServiceProgram
    StrCmp $R2 "" wintun
    ${GetParent} $R2 $R3
    ${If} ${FileExists} "$R3\wintun.dll"
    ${AndIfNot} ${FileExists} "$INSTDIR\wintun.dll"
      DetailPrint "taking over $R3\wintun.dll"
      CopyFiles /SILENT "$R3\wintun.dll" "$INSTDIR"
    ${EndIf}
    Goto wintun
  wintun_done:

  WriteUninstaller "$INSTDIR\uninstall.exe"
  WriteRegStr HKLM "${APP_KEY}" "InstallDir" "$INSTDIR"
  WriteRegStr HKLM "${APP_KEY}" "Version" "${VERSION}"
  WriteRegStr HKLM "${APP_KEY}" "VersionNum" "${VERSION_NUM}"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayName" "n3n-be"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "${UNINST_KEY}" "Publisher" "Honey Bunny QT"
  WriteRegStr HKLM "${UNINST_KEY}" "URLInfoAbout" "https://github.com/HoneyBunnyQT/n3n-be"
  WriteRegStr HKLM "${UNINST_KEY}" "HelpLink" "https://honeybunnyqt.github.io/n3n-be/"
  WriteRegStr HKLM "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayIcon" "$INSTDIR\n3n-be.exe"
  WriteRegStr HKLM "${UNINST_KEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegStr HKLM "${UNINST_KEY}" "QuietUninstallString" '"$INSTDIR\uninstall.exe" /S'
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoRepair" 1
  ${GetSize} "$INSTDIR" "/S=0K" $R0 $R1 $R2
  IntFmt $R0 "0x%08X" $R0
  WriteRegDWORD HKLM "${UNINST_KEY}" "EstimatedSize" "$R0"
SectionEnd

Section "Start menu entries" SecShortcuts
  SetShellVarContext all
  CreateDirectory "$SMPROGRAMS\n3n-be"
  CreateShortcut "$SMPROGRAMS\n3n-be\n3n-be.lnk" "$INSTDIR\n3n-be-tray.exe"
  ${If} ${FileExists} "$INSTDIR\${HELP_PAGE}"
    CreateShortcut "$SMPROGRAMS\n3n-be\n3n-be help.lnk" "$INSTDIR\${HELP_PAGE}"
  ${EndIf}
  CreateShortcut "$SMPROGRAMS\n3n-be\Uninstall n3n-be.lnk" "$INSTDIR\uninstall.exe"
  SetShellVarContext current
SectionEnd

Section "Tray app at login" SecAutostart
  ; as the tray's own "Start with Windows" has it, for the session edge
  WriteRegStr HKCU "${RUN_KEY}" "n3n-be-tray" '"$INSTDIR\n3n-be-tray.exe" edge'
SectionEnd

Section /o "Wintun (no TAP driver)" SecWintun
  ${IfNot} ${FileExists} "$INSTDIR\wintun.dll"
    DetailPrint "getting wintun.dll from wintun.net"
    nsExec::ExecToLog '"$INSTDIR\n3n-be.exe" edge wintun get'
    Pop $R0
    StrCmp $R0 0 +2
    DetailPrint "Wintun could not be fetched: the tray's $\"Get Wintun$\" tries again"
  ${EndIf}
SectionEnd

Section "Edge as a service" SecService
  ; one installed before goes on below, with its own configuration
  ReadRegStr $R0 HKLM "${SERVICES_KEY}\n3n-be-edge" "ImagePath"
  StrCmp $R0 "" 0 done
  ; into %ProgramData%\n3n-be, for administrators only, if the edge can
  ; start with it - no copy of it, with the community's key, elsewhere
  ${If} $ConfFile != ""
    DetailPrint "importing $ConfFile"
    nsExec::ExecToLog '"$INSTDIR\n3n-be.exe" edge service import "$ConfFile" edge'
    Pop $R0
    StrCmp $R0 0 +2
    DetailPrint "$ConfFile is not taken: the tray's Configuration, $\"Import a file...$\" tries again"
  ${EndIf}
  ${If} ${FileExists} "$PROFILE\n3n-be\edge.conf"
  ${OrIf} ${FileExists} "$ProgramData\n3n-be\edge.conf"
    DetailPrint "installing the service n3n-be-edge"
    nsExec::ExecToLog '"$INSTDIR\n3n-be.exe" edge service install edge'
    Pop $R0
  ${Else}
    DetailPrint "no configuration for the service yet: the tray's $\"Install the service...$\" asks for one"
  ${EndIf}
  done:
SectionEnd

; the services of an install before, now of this program, and running again
; if they ran; "Start with Windows" of the tray app at this one
Section "-upgrade"
  StrCpy $R5 0
  services:
    EnumRegValue $R0 HKLM "${APP_KEY}\upgrade" $R5
    StrCmp $R0 "" services_done
    IntOp $R5 $R5 + 1
    ReadRegDWORD $R8 HKLM "${APP_KEY}\upgrade" $R0
    Call SessionOf
    DetailPrint "$R0 with this program"
    ; an install again: the program, the firewall rule, the tray file, the
    ; rights - and it starts
    nsExec::ExecToLog '"$INSTDIR\n3n-be.exe" edge service install $R6'
    Pop $R1
    ${If} $R8 == 0
      nsExec::Exec 'sc.exe stop "$R0"'
      Pop $R1
    ${EndIf}
    Goto services
  services_done:
  DeleteRegKey HKLM "${APP_KEY}\upgrade"

  StrCpy $R5 0
  run:
    EnumRegValue $R0 HKCU "${RUN_KEY}" $R5
    StrCmp $R0 "" run_done
    IntOp $R5 $R5 + 1
    StrCpy $R1 $R0 11
    StrCmp $R1 "n3n-be-tray" 0 run
    ReadRegStr $R2 HKCU "${RUN_KEY}" $R0
    ${StrStr} $R3 $R2 'n3n-be-tray.exe"'
    StrCmp $R3 "" run
    StrCpy $R3 $R3 "" 16
    WriteRegStr HKCU "${RUN_KEY}" $R0 '"$INSTDIR\n3n-be-tray.exe"$R3'
    Goto run
  run_done:

  ${If} $TrayRan == 1
    Call StartTray
  ${EndIf}
SectionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SecMain} "n3n-be.exe (edge and supernode), n3n-be-tray.exe, the docs as the help."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecShortcuts} "The tray app, the help and the uninstaller in the Start menu."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecAutostart} "The tray app starts when you log in (the tray's $\"Start with Windows$\")."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecWintun} "wintun.dll from wintun.net, checked: an adapter of the edge's own, without the TAP driver. Chosen when no TAP adapter is installed."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecService} "The edge starts at boot. Its configuration: yours from %USERPROFILE%\n3n-be\edge.conf, or one to choose on the next page."
!insertmacro MUI_FUNCTION_DESCRIPTION_END


; ---- the page of the configuration, for a service without one

Function ConfPage
  ${IfNot} ${SectionIsSelected} ${SecService}
    Abort
  ${EndIf}
  ReadRegStr $R0 HKLM "${SERVICES_KEY}\n3n-be-edge" "ImagePath"
  StrCmp $R0 "" 0 skip
  ${If} ${FileExists} "$PROFILE\n3n-be\edge.conf"
  ${OrIf} ${FileExists} "$ProgramData\n3n-be\edge.conf"
    Abort
  ${EndIf}
  !insertmacro MUI_HEADER_TEXT "Configuration" "The edge's configuration, for the service"
  nsDialogs::Create 1018
  Pop $R0
  ${NSD_CreateLabel} 0 0 100% 48u "The service needs a configuration: the community, its key and the \
      supernode - an edge.conf as one of your community gave it to you, or edge.conf.sample filled in.$\r$\n$\r$\n\
      Without one, leave this empty: the tray app's $\"Install the service...$\" asks for it later."
  Pop $R0
  ${NSD_CreateText} 0 54u 78% 13u "$ConfFile"
  Pop $ConfField
  ${NSD_CreateBrowseButton} 80% 53u 20% 15u "Browse..."
  Pop $R0
  ${NSD_OnClick} $R0 ConfBrowse
  nsDialogs::Show
  skip:
FunctionEnd

Function ConfBrowse
  nsDialogs::SelectFileDialog open "" "Configurations (*.conf)|*.conf|All files|*.*"
  Pop $R0
  StrCmp $R0 "" +2
  ${NSD_SetText} $ConfField $R0
FunctionEnd

Function ConfLeave
  ${NSD_GetText} $ConfField $ConfFile
  ${If} $ConfFile != ""
  ${AndIfNot} ${FileExists} "$ConfFile"
    MessageBox MB_OK|MB_ICONEXCLAMATION "$ConfFile is not there."
    Abort
  ${EndIf}
FunctionEnd


Function .onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_OK|MB_ICONSTOP "n3n-be is for 64 bit Windows." /SD IDOK
    Abort
  ${EndIf}
  SetRegView 64
  ReadEnvStr $ProgramData "ProgramData"
  ; /CONF=FILE: the configuration of the service, as its page would take it
  ClearErrors
  ${GetOptions} $CMDLINE "/CONF=" $R0
  ${IfNot} ${Errors}
    StrCpy $ConfFile $R0
  ${EndIf}

  ; an install before: there again, and whether this is older
  ReadRegStr $R0 HKLM "${UNINST_KEY}" "InstallLocation"
  StrCmp $R0 "" +2
  StrCpy $INSTDIR $R0
  ReadRegStr $PrevVersion HKLM "${APP_KEY}" "Version"
  ReadRegStr $R1 HKLM "${APP_KEY}" "VersionNum"
  ${If} $PrevVersion != ""
    StrCpy $Welcome "n3n-be $PrevVersion is installed: this replaces it, and keeps its configurations and services."
    ${VersionCompare} "${VERSION_NUM}" "$R1" $R2
    ${If} $R2 == 2
      MessageBox MB_YESNO|MB_ICONQUESTION "n3n-be $PrevVersion is installed, newer than this ${VERSION}.  \
          Install the older one over it?" /SD IDNO IDYES +2
      Abort
    ${EndIf}
  ${Else}
    ; services that a zip installed count too
    ReadRegStr $R0 HKLM "${SERVICES_KEY}\n3n-be-edge" "ImagePath"
    StrCmp $R0 "" +2
    StrCpy $Welcome "The service n3n-be-edge is there, from a zip: from now on it runs the installed program."
  ${EndIf}

  ; Wintun where there is no TAP adapter, nor wintun.dll yet
  Call HasTap
  ${If} $R9 == 0
  ${AndIfNot} ${FileExists} "$INSTDIR\wintun.dll"
    !insertmacro SelectSection ${SecWintun}
  ${EndIf}
FunctionEnd


; ---- the uninstaller

Section "Uninstall"
  SetRegView 64
  Call un.CloseTrays

  ; each service its own uninstall: stopped, its firewall rule and tray
  ; file gone; the configurations stay
  StrCpy $R5 "uninstall"
  Call un.ListServices
  StrCpy $R5 0
  services:
    EnumRegValue $R0 HKLM "${APP_KEY}\uninstall" $R5
    StrCmp $R0 "" services_done
    IntOp $R5 $R5 + 1
    Call un.SessionOf
    DetailPrint "uninstalling $R0"
    nsExec::ExecToLog '"$INSTDIR\n3n-be.exe" edge service uninstall $R6'
    Pop $R1
    Goto services
  services_done:

  ; the tray's start with Windows, of each session
  run:
    StrCpy $R5 0
  run_next:
    EnumRegValue $R0 HKCU "${RUN_KEY}" $R5
    StrCmp $R0 "" run_done
    IntOp $R5 $R5 + 1
    StrCpy $R1 $R0 11
    StrCmp $R1 "n3n-be-tray" 0 run_next
    DeleteRegValue HKCU "${RUN_KEY}" $R0
    Goto run
  run_done:

  Delete "$INSTDIR\n3n-be.exe"
  Delete "$INSTDIR\n3n-be-tray.exe"
  Delete "$INSTDIR\wintun.dll"
  Delete "$INSTDIR\n3n-be-ctl"
  Delete "$INSTDIR\README.txt"
  Delete "$INSTDIR\edge.conf.sample"
  Delete "$INSTDIR\supernode.conf.sample"
  RMDir /r "$INSTDIR\docs"
  RMDir /r "$INSTDIR\LICENSES"
  Delete "$INSTDIR\uninstall.exe"
  RMDir "$INSTDIR"

  SetShellVarContext all
  RMDir /r "$SMPROGRAMS\n3n-be"
  SetShellVarContext current

  DeleteRegKey HKLM "${UNINST_KEY}"
  DeleteRegKey HKLM "${APP_KEY}"
  DetailPrint "the configurations stay: %ProgramData%\n3n-be, %USERPROFILE%\n3n-be"
SectionEnd

Function un.onInit
  SetRegView 64
FunctionEnd
