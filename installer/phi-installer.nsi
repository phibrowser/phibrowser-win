; Copyright 2026 The Fyde Innovations Limited. All rights reserved.
; Use of this source code is governed by a BSD-style license that can be
; found in the LICENSE file.
;
; The Phi Browser installer - WinSparkle's payload and the manual
; first-install, one script. Design (the Mac parity notes in
; windows-framework-status.md; the flow itself in docs/static-build-plan.md;
; the look in docs/windows-native-ui.md §12):
;   - ONE WINDOW, NO WIZARD: a 600 x 420 dip window on the Windows Settings
;     ground with the brand mark, Segoe UI Variable and Fluent controls
;     painted by the PhiSetupUI plugin (installer/ui). Three screens
;     install (Setup with an expander for the folders, progress, done), two
;     remove (confirm with a "delete my data" checkbox, progress). Every
;     question that used to be a MessageBox is an InfoBar in the window;
;     only the abort confirmation is still a box. Dark mode follows the
;     Windows app mode.
;   - PER MACHINE by default: C:\Program Files\Phi Browser, elevated (UAC),
;     Apps & Features entry and shortcuts for all users. The user may point
;     it anywhere else in the expander; a folder outside Program Files makes
;     install_static treat the install as per-user, so the data-dir value
;     is written to BOTH HKLM and HKCU. Crash-upload consent is NOT the
;     installer's: the client writes HKCU\Software\Phi\usagestats from the
;     user's own data-sharing choice (onboarding, Settings), and the fork's
;     crash reporter reads it per user (release review R1).
;   - THE DATA FOLDER IS THE USER'S CHOICE: profiles, history, settings and
;     the telemetry config live in a folder separate from the program,
;     default %LOCALAPPDATA%\Phi\User Data. Another folder is recorded as
;     Software\Phi\UserDataDir, which install_static consults after
;     --user-data-dir and the policy (chrome/install_static/user_data_dir.cc)
;     - every Phi process, the crash handler included, follows it.
;   - LAYOUT: Phi.exe + framework\ (the non-component framework;
;     make-installer.sh stages it so, /DPHI_LAYOUT_FRAMEWORK tells this
;     script). A component framework can only be flat: the exe's static
;     imports must sit in its own directory.
;   - IN-PLACE UPDATE: chrome.dll is locked while Phi runs, so the
;     installer runs when the app has quit (WinSparkle's flow), replaces
;     framework\ whole and copies the new files over - the stable path keeps
;     taskbar pins and shortcuts valid. No rollback snapshot: nothing ever
;     restored from one (an older version is reinstalled with its own
;     installer and /ALLOWDOWNGRADE). While Phi still runs the Setup screen
;     shows an InfoBar and polls; the section keeps a 60 s wait as the
;     safety net. Silent (/S) installs keep the recorded program and data
;     folders.
;   - NO SILENT DOWNGRADE: an older installer over a newer install stops
;     (the Setup screen is the notice; /S exits with code 3) unless run with
;     /ALLOWDOWNGRADE. The same version reinstalls.
;   - ONE INSTALLER PER ARCHITECTURE: PhiBrowserSetup-<ver>.exe carries the
;     x64 build, PhiBrowserSetup-<ver>-arm64.exe the Windows on ARM build
;     (/DPHI_ARCH). The stub itself is NSIS's 32-bit code, which every
;     Windows runs (x64 through WOW64, ARM64 through its x86 emulation), so
;     the payload's architecture is checked against the MACHINE in .onInit:
;     the arm64 installer refuses anything but an ARM64 PC (the Setup screen
;     becomes the notice), the x64 one notes on an ARM64 PC that its build
;     runs there emulated and continues - WinSparkle picks the enclosure by
;     architecture, so a silent update on an ARM PC that has the x64 build
;     is the x64 build again.
;   - DEFAULT-BROWSER REGISTRATION: a Program Files install is system-level
;     to install_static, and Chromium leaves a system-level install's
;     registration to its installer (shell_util.cc RegisterChromeBrowserImpl)
;     - so this script writes what Chrome's setup would (ProgIds,
;     StartMenuInternet\<app>\Capabilities, RegisteredApplications, App
;     Paths) under HKLM, never the user's choice itself: Windows keeps that
;     for the user, and Phi's "Make default" opens Settings at this entry.
;   - REMOVAL IS BY NAME: the uninstaller deletes the stage's own file list
;     (uninstall-list.py, at compile time) and removes a folder only once it
;     is empty - never a wildcard or RMDir /r in a folder the user may have
;     picked; "delete my data" removes the whole data folder only when it is
;     the default one or Setup created it, else only Phi's own entries.
;
; Build (makensis 3.x, on Linux via make-installer.sh):
;   makensis -INPUTCHARSET UTF8 -DPHI_VERSION=0.1.0 -DPHI_STAGE=<staged tree> \
;            [-DPHI_LAYOUT_FRAMEWORK] [-DPHI_ARCH=x64|arm64] [-DPHI_OUTDIR=dist] \
;            [-DPHI_TELEMETRY_JSON=phi_telemetry.json] [-DPHI_SIGN_CMD=<sign-pe.sh>] \
;            [-DPHI_UI_TEST] phi-installer.nsi
;   PHI_UI_TEST builds a look-and-feel test: it installs to C:\phi-setup-test
;   and records itself under Software\PhiSetupTest (and registers no
;   browser), so a throwaway payload never touches a real install.

!ifndef PHI_VERSION
  !error "pass /DPHI_VERSION=x.y.z"
!endif
!ifndef PHI_BUILD_NUMBER
  !define PHI_BUILD_NUMBER "0"
!endif
!define PHI_FULL_VERSION "${PHI_VERSION}.${PHI_BUILD_NUMBER}"
!ifndef PHI_STAGE
  !error "pass /DPHI_STAGE=<staged payload dir>"
!endif

!ifndef PHI_ARCH
  !define PHI_ARCH "x64"
!endif
!if "${PHI_ARCH}" == "arm64"
  !define PHI_ARCH_SUFFIX "-arm64"
!else
  !if "${PHI_ARCH}" != "x64"
    !error "PHI_ARCH must be x64 or arm64"
  !endif
  !define PHI_ARCH_SUFFIX ""
!endif

Unicode true
ManifestDPIAware true
!include "nsDialogs.nsh"
!include "LogicLib.nsh"
!include "x64.nsh"
!include "WinMessages.nsh"
!include "FileFunc.nsh"
!include "WordFunc.nsh"
!addplugindir /x86-unicode "${__FILEDIR__}/ui/x86-unicode"

Name "Phi Browser"
Caption "Phi Browser Setup"
UninstallCaption "Uninstall Phi Browser"
; Where the setup exe lands (makensis resolves a bare OutFile against this
; script's directory, not the working directory).
!ifndef PHI_OUTDIR
  !define PHI_OUTDIR "."
!endif
OutFile "${PHI_OUTDIR}/PhiBrowserSetup-${PHI_FULL_VERSION}${PHI_ARCH_SUFFIX}.exe"
; Authenticode (make-installer.sh passes its sign-pe.sh): the setup exe once
; it is written, and the uninstaller stub before it is packed into the
; installer - WriteUninstaller only drops that stub, so this is the one
; place Uninstall.exe can pick up a signature (NSIS 3.08+). The payload was
; signed in the stage. A failed sign fails the build (= 0).
!ifdef PHI_SIGN_CMD
  !finalize '"${PHI_SIGN_CMD}" "%1"' = 0
  !uninstfinalize '"${PHI_SIGN_CMD}" "%1"' = 0
!endif
RequestExecutionLevel admin
!ifdef PHI_UI_TEST
  !define REG_PRODUCT "Software\PhiSetupTest"
  !define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\PhiBrowserSetupTest"
  InstallDir "C:\phi-setup-test\Phi Browser"
!else
  !define REG_PRODUCT "Software\Phi"
  !define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\PhiBrowser"
  InstallDir "$PROGRAMFILES64\Phi Browser"
!endif
InstallDirRegKey HKLM "${REG_PRODUCT}" "InstallDir"
; The default-browser names Chromium derives for this brand - install_static
; (chromium_install_modes.h: base_app_name, browser/pdf_prog_id_prefix,
; direct_launch_url_scheme; system-level = no suffix) and the display
; strings installer::GetLocalizedString reads from chromium_strings.grd.
; They must stay what chrome.dll computes, or Settings' deep link
; (registeredAppMachine=<app>) and the default-state checks miss this entry.
; The doc icons are the exe's one icon (Phi.exe has no kHtmlDocIndex /
; kPDFDocIndex slots).
!define BROWSER_APP "Chromium"
!define BROWSER_HTML_PROGID "ChromiumHTM"
!define BROWSER_PDF_PROGID "ChromiumPDF"
!define BROWSER_URL_SCHEME "chromium"
!define BROWSER_NAME "Phi"
!define BROWSER_TOOLTIP "Access the Internet"
!define BROWSER_LONG_DESC "Phi is a web browser that runs webpages and applications with lightning speed. It's fast, stable, and easy to use. Browse the web more safely with malware and phishing protection built into Phi."
!define BROWSER_COMPANY "Phinomenon"
!define BROWSER_CLIENT_KEY "Software\Clients\StartMenuInternet\${BROWSER_APP}"
SetCompressor /SOLID lzma
Icon "${PHI_STAGE}\resources\phi.ico"
UninstallIcon "${PHI_STAGE}\resources\phi.ico"
ShowInstDetails nevershow
ShowUninstDetails nevershow
; The progress screen moves on by itself: to the Done screen when
; installing, out of the window when removing.
AutoCloseWindow true
XPStyle on
; Control 1028 is reused as the footer's left text; the plugin clears it.
BrandingText " "
LoadLanguageFile "${NSISDIR}\Contrib\Language files\English.nlf"

VIProductVersion "${PHI_FULL_VERSION}"
VIAddVersionKey "ProductName" "Phi Browser"
VIAddVersionKey "FileDescription" "Phi Browser Setup (${PHI_ARCH})"
VIAddVersionKey "CompanyName" "Fyde Innovations"
VIAddVersionKey "LegalCopyright" "Copyright 2026 The Fyde Innovations Limited"
VIAddVersionKey "FileVersion" "${PHI_FULL_VERSION}"
VIAddVersionKey "ProductVersion" "${PHI_FULL_VERSION}"

Var Dpi
Var Dark
Var DataDir          ; the data folder in effect
Var DataDirDefault   ; %LOCALAPPDATA%\Phi\User Data
Var DataDirChosen    ; 1 once the Setup screen ran (silent installs leave the record alone)
Var DataDirRecorded  ; the data folder a previous install recorded (or the default)
Var DataDirOwned     ; 1 when Setup created the (non-default) data folder: removable whole
Var DesktopShortcut  ; 1 = write the desktop shortcut (the expander's toggle)
Var PrevVersion      ; the installed version when this is an update, else ""
Var SubLine          ; the header's second line: version, update or reinstall
Var Verb             ; Install | Update | Reinstall
Var VerbIng          ; Installing… | Updating… | Reinstalling…
Var ArchState        ; "" | block (arm64 installer on a non-ARM PC) | warn (x64 on ARM)
Var Downgrade        ; 1 = a newer version is installed and /ALLOWDOWNGRADE was not given
Var OtherAdmin       ; 1 = elevated as another account than the session's (over-the-shoulder UAC)
Var Stage            ; setup | install | done - what Cancel means
Var Expanded         ; the expander's state
Var Running          ; 1 while Phi holds chrome.dll
Var ErrorShown       ; 1 while the InfoBar shows a folder error
; The Setup screen's controls.
Var hDlg
Var hLead
Var hInfo
Var hInfoGlyph
Var hInfoB
Var hInfoT
Var hInfoBtn
Var hCard
Var hChev
Var hXGlyph
Var hXLabel
Var hXHint
Var hSep1
Var hSep2
Var hSep3
Var hR1Label
Var hR1Field
Var hR1Edit
Var hR1Browse
Var hR2Label
Var hR2Hint
Var hR2Field
Var hR2Edit
Var hR2Browse
Var hR3Label
Var hR3Hint
Var hToggle
Var hToggleText
Var hPercent
Var hDeleteData
Var DeleteData

; ---------------------------------------------------------------- geometry
; Everything is laid out in dip (the artboards' units); the plugin scales.
; Only nsDialogs-created buttons need pixel maths here.
!macro Px out dip
  IntOp ${out} ${dip} * $Dpi
  IntOp ${out} ${out} / 96
!macroend
!define Px "!insertmacro Px"

; A clickable control: nsDialogs creates it (so ${NSD_OnClick} works), the
; plugin paints it. KIND is a PhiSetupUI kind, X Y W H dip.
!macro NsdButton out kind x y w h text
  ${Px} $R0 ${x}
  ${Px} $R1 ${y}
  ${Px} $R2 ${w}
  ${Px} $R3 ${h}
  nsDialogs::CreateControl BUTTON ${WS_CHILD}|${WS_VISIBLE}|${WS_TABSTOP}|${BS_OWNERDRAW} 0 $R0 $R1 $R2 $R3 "${text}"
  Pop ${out}
  PhiSetupUI::Style ${out} "${kind}" 0
!macroend
!define NsdButton "!insertmacro NsdButton"

!macro Create out parent kind x y w h text
  PhiSetupUI::Create ${parent} "${kind}" ${x} ${y} ${w} ${h} "${text}"
  Pop ${out}
!macroend
!define Create "!insertmacro Create"

; The header every screen opens with: the mark at 48, the name at 28 / 600
; and a 12 dip line under it.
!macro Header parent name sub
  ${Create} $R9 ${parent} "mark" 32 20 48 48 ""
  ${Create} $R9 ${parent} "text.title" 96 22 472 36 "${name}"
  ${Create} $R9 ${parent} "text.sub" 96 58 472 16 "${sub}"
!macroend
!define Header "!insertmacro Header"

; The progress screen (NSIS's instfiles page, installer and uninstaller):
; the header, "Installing..." with the percentage, the 4 dip bar, NSIS's
; status line restyled, a hint. Cancel alone in the footer.
!macro DecorateInstFiles caption name sub verb footer hint
  FindWindow $0 "#32770" "" $HWNDPARENT
  PhiSetupUI::Page $0
  PhiSetupUI::Caption "${caption}"
  ${Header} $0 "${name}" "${sub}"
  ${Create} $R9 $0 "text.strong" 32 98 400 20 "${verb}"
  ${Create} $hPercent $0 "text.hint!r" 488 100 80 16 ""
  GetDlgItem $1 $0 1004
  PhiSetupUI::Place $1 32 128 536 4
  PhiSetupUI::Progress $1 $hPercent
  GetDlgItem $1 $0 1006
  PhiSetupUI::Style $1 "text.hint" 0
  PhiSetupUI::Place $1 32 142 536 16
  ${Create} $R9 $0 "text.hint" 32 170 536 32 "${hint}"
  PhiSetupUI::Buttons "none" "" "Cancel"
  PhiSetupUI::Footer "${footer}"
!macroend

; Whether the recorded data folder is one Setup created (UserDataDirOwned,
; written beside UserDataDir): only then may "delete my data" remove it
; whole.
!macro ReadDataDirOwned
  ReadRegDWORD $DataDirOwned HKCU "${REG_PRODUCT}" "UserDataDirOwned"
  ${If} $DataDirOwned == ""
    ReadRegDWORD $DataDirOwned HKLM "${REG_PRODUCT}" "UserDataDirOwned"
  ${EndIf}
  ${If} $DataDirOwned != 1
    StrCpy $DataDirOwned 0
  ${EndIf}
!macroend

; ---------------------------------------------------------------- pages
Page custom SetupCreate SetupLeave
Page instfiles "" InstShow
Page custom DoneCreate DoneLeave
UninstPage custom un.ConfirmCreate un.ConfirmLeave
UninstPage instfiles "" un.InstShow

; ---------------------------------------------------------------- init
Function .onInit
  StrCpy $ArchState ""
  ; The payload's architecture against this PC's (the header's note on
  ; PHI_ARCH). ${IsNativeARM64} is the machine, whatever this 32-bit stub
  ; runs under. With a window to show, the Setup screen carries the notice;
  ; a silent run keeps the old box (answered by /SD).
!if "${PHI_ARCH}" == "arm64"
  ${IfNot} ${IsNativeARM64}
    ${If} ${Silent}
      MessageBox MB_OK|MB_ICONSTOP "This is Phi Browser for Windows on ARM, and this PC is not ARM-based.$\r$\n$\r$\nPlease use the standard installer, PhiBrowserSetup-${PHI_FULL_VERSION}.exe." /SD IDOK
      Abort
    ${EndIf}
    StrCpy $ArchState "block"
  ${EndIf}
!else
  ${If} ${IsNativeARM64}
    ${If} ${Silent}
      MessageBox MB_YESNO|MB_ICONEXCLAMATION "This PC is Windows on ARM. This installer carries the x64 build of Phi Browser, which runs here through emulation; the ARM64 build, PhiBrowserSetup-${PHI_FULL_VERSION}-arm64.exe, is the better fit.$\r$\n$\r$\nInstall the x64 build anyway?" /SD IDYES IDYES x64_on_arm_ok
      Abort
    x64_on_arm_ok:
    ${EndIf}
    StrCpy $ArchState "warn"
  ${EndIf}
!endif
  StrCpy $DataDirDefault "$LOCALAPPDATA\Phi\User Data"
  ; A recorded choice from a previous install is the starting point.
  ReadRegStr $DataDir HKCU "${REG_PRODUCT}" "UserDataDir"
  ${If} $DataDir == ""
    ReadRegStr $DataDir HKLM "${REG_PRODUCT}" "UserDataDir"
  ${EndIf}
  ${If} $DataDir == ""
    StrCpy $DataDir $DataDirDefault
  ${EndIf}
  StrCpy $DataDirRecorded $DataDir
  !insertmacro ReadDataDirOwned
  StrCpy $DataDirChosen 0
  ; Over-the-shoulder UAC runs this as the admin who typed the password:
  ; HKCU and %LOCALAPPDATA% are then THEIR profile, not the session user's.
  ; The session's user (WTSUserName) against this token's (GetUserName).
  StrCpy $OtherAdmin 0
  System::Call 'wtsapi32::WTSQuerySessionInformationW(p 0, i -1, i 5, *p .r1, *i .r2) i .r0'
  ${If} $0 != 0
    System::Call '*$1(&w${NSIS_MAX_STRLEN} .r3)'
    System::Call 'wtsapi32::WTSFreeMemory(p r1)'
    System::Call 'advapi32::GetUserNameW(w .r4, *i ${NSIS_MAX_STRLEN}) i .r0'
    ${If} $0 != 0
    ${AndIf} $3 != ""
    ${AndIf} $3 != $4
      StrCpy $OtherAdmin 1
    ${EndIf}
  ${EndIf}
  StrCpy $DesktopShortcut 1
  StrCpy $Stage "setup"
  StrCpy $Expanded 0
  StrCpy $Running 0
  StrCpy $ErrorShown 0
  ; An update: the Apps & Features entry names the installed version.
  StrCpy $PrevVersion ""
  SetRegView 64
  ReadRegStr $0 HKLM "${UNINST_KEY}" "DisplayVersion"
  SetRegView 32
  ${If} $0 != ""
  ${AndIf} ${FileExists} "$INSTDIR\Phi.exe"
    StrCpy $PrevVersion $0
  ${EndIf}
  ; An older version over a newer one only on purpose (/ALLOWDOWNGRADE):
  ; the Setup screen becomes the notice, a silent run exits with 3 (1 is a
  ; cancel, 2 an abort) and changes nothing.
  StrCpy $Downgrade 0
  StrCpy $1 0
  ${If} $PrevVersion != ""
    ${VersionCompare} $PrevVersion "${PHI_FULL_VERSION}" $1
  ${EndIf}
  ${If} $1 == 1
    ${GetParameters} $2
    ClearErrors
    ${GetOptions} $2 "/ALLOWDOWNGRADE" $3
    ${If} ${Errors}
      ${If} ${Silent}
        SetErrorLevel 3
        Quit
      ${EndIf}
      StrCpy $Downgrade 1
    ${EndIf}
  ${EndIf}
  ${If} $PrevVersion == ""
    StrCpy $SubLine "Version ${PHI_FULL_VERSION} · ${PHI_ARCH} · Fyde Innovations"
    StrCpy $Verb "Install"
    StrCpy $VerbIng "Installing…"
  ${ElseIf} $PrevVersion == "${PHI_FULL_VERSION}"
    StrCpy $SubLine "Reinstall ${PHI_FULL_VERSION} · ${PHI_ARCH}"
    StrCpy $Verb "Reinstall"
    StrCpy $VerbIng "Reinstalling…"
  ${ElseIf} $1 == 1
    StrCpy $SubLine "Downgrade $PrevVersion → ${PHI_FULL_VERSION} · ${PHI_ARCH}"
    StrCpy $Verb "Install"
    StrCpy $VerbIng "Installing…"
  ${Else}
    StrCpy $SubLine "Update $PrevVersion → ${PHI_FULL_VERSION} · ${PHI_ARCH}"
    StrCpy $Verb "Update"
    StrCpy $VerbIng "Updating…"
  ${EndIf}
FunctionEnd

Function .onGUIInit
  PhiSetupUI::Init
  Pop $Dark
  PhiSetupUI::Dpi
  Pop $Dpi
  PhiSetupUI::Frame
FunctionEnd

; Cancel: a question while there is something to stop, a plain close on
; the Done screen.
Function .onUserAbort
  ${If} $Stage == "done"
    Return
  ${EndIf}
  MessageBox MB_YESNO|MB_ICONEXCLAMATION|MB_DEFBUTTON2 "Stop installing Phi Browser?" IDYES stop
  Abort
  stop:
FunctionEnd

; The finish screen's launch: through the shell, so Phi runs as the user,
; not with this installer's elevated token (an elevated browser would own
; the data folder's files).
Function LaunchPhi
  Exec '"$WINDIR\explorer.exe" "$INSTDIR\Phi.exe"'
FunctionEnd

; 1 in $0 while a Phi process keeps chrome.dll mapped in the recorded
; program folder (open for write is the test; a rename is not - Windows
; lets a mapped image be renamed). Nothing installed yet: 0. The
; uninstaller has its own copy (un.IsPhiRunning).
!macro IsPhiRunningFn un
Function ${un}IsPhiRunning
  StrCpy $0 0
  StrCpy $2 "$INSTDIR\framework\chrome.dll"
  ${IfNot} ${FileExists} $2
    StrCpy $2 "$INSTDIR\chrome.dll"
  ${EndIf}
  ${If} ${FileExists} $2
    ClearErrors
    FileOpen $1 $2 a
    ${If} ${Errors}
      StrCpy $0 1
    ${Else}
      FileClose $1
    ${EndIf}
  ${EndIf}
FunctionEnd
!macroend
!insertmacro IsPhiRunningFn ""
!insertmacro IsPhiRunningFn "un."

!ifndef PHI_UI_TEST
; 1 in $1 when $2 occurs in $0, case-insensitively (paths). Uses $3-$5.
!macro StrHasFn un
Function ${un}StrHas
  StrCpy $1 0
  StrLen $3 $2
  StrCpy $4 0
  loop:
    StrCpy $5 $0 $3 $4
    StrCmp $5 "" done
    StrCmp $5 $2 0 next
      StrCpy $1 1
      Goto done
    next:
    IntOp $4 $4 + 1
    Goto loop
  done:
FunctionEnd
!macroend
!insertmacro StrHasFn ""
!insertmacro StrHasFn "un."
!endif

; ---------------------------------------------------------------- setup screen
; The whole install is one click: the header, one lead sentence and the
; expander (collapsed) with Install / Update in the footer. Opening the
; expander swaps the lead for three rows: program folder, browsing data,
; desktop shortcut. An InfoBar takes the lead's place while Phi is still
; running (Update disabled), on an ARM PC with the x64 build, or after a
; folder failed its write probe.

; Places the lead, the InfoBar and the card for the current state.
Function SetupLayout
  ${If} $Expanded == 1
    PhiSetupUI::Show $hLead 0
    PhiSetupUI::Show $hInfo 0
    PhiSetupUI::Show $hInfoGlyph 0
    PhiSetupUI::Show $hInfoB 0
    PhiSetupUI::Show $hInfoT 0
    PhiSetupUI::Show $hInfoBtn 0
    StrCpy $3 86
  ${Else}
    ${If} $Running == 1
    ${OrIf} $ArchState == "warn"
    ${OrIf} $ErrorShown == 1
      PhiSetupUI::Show $hLead 0
      PhiSetupUI::Show $hInfo 1
      PhiSetupUI::Show $hInfoGlyph 1
      PhiSetupUI::Show $hInfoB 1
      PhiSetupUI::Show $hInfoT 1
      ${If} $Running == 1
        PhiSetupUI::Show $hInfoBtn 1
      ${Else}
        PhiSetupUI::Show $hInfoBtn 0
      ${EndIf}
      StrCpy $3 166
    ${Else}
      PhiSetupUI::Show $hLead 1
      PhiSetupUI::Show $hInfo 0
      PhiSetupUI::Show $hInfoGlyph 0
      PhiSetupUI::Show $hInfoB 0
      PhiSetupUI::Show $hInfoT 0
      PhiSetupUI::Show $hInfoBtn 0
      StrCpy $3 146
    ${EndIf}
  ${EndIf}
  ; The card and its header row at $3.
  ${If} $Expanded == 1
    PhiSetupUI::Place $hCard 32 $3 536 220
  ${Else}
    PhiSetupUI::Place $hCard 32 $3 536 56
  ${EndIf}
  IntOp $4 $3 + 18
  PhiSetupUI::Place $hXGlyph 48 $4 20 20
  IntOp $4 $3 + 10
  PhiSetupUI::Place $hXLabel 84 $4 400 20
  IntOp $4 $3 + 30
  PhiSetupUI::Place $hXHint 84 $4 420 16
  IntOp $4 $3 + 12
  PhiSetupUI::Place $hChev 520 $4 32 32
  PhiSetupUI::Set $hChev $Expanded
  ; The rows.
  IntOp $4 $3 + 56
  PhiSetupUI::Place $hSep1 33 $4 534 1
  IntOp $4 $3 + 71
  PhiSetupUI::Place $hR1Label 84 $4 120 20
  IntOp $4 $3 + 65
  PhiSetupUI::Place $hR1Field 214 $4 254 32
  IntOp $4 $3 + 71
  PhiSetupUI::Place $hR1Edit 224 $4 234 20
  IntOp $4 $3 + 67
  PhiSetupUI::Place $hR1Browse 480 $4 72 28
  IntOp $4 $3 + 105
  PhiSetupUI::Place $hSep2 33 $4 534 1
  IntOp $4 $3 + 114
  PhiSetupUI::Place $hR2Label 84 $4 122 20
  IntOp $4 $3 + 134
  PhiSetupUI::Place $hR2Hint 84 $4 126 32
  IntOp $4 $3 + 122
  PhiSetupUI::Place $hR2Field 214 $4 254 32
  IntOp $4 $3 + 128
  PhiSetupUI::Place $hR2Edit 224 $4 234 20
  IntOp $4 $3 + 124
  PhiSetupUI::Place $hR2Browse 480 $4 72 28
  IntOp $4 $3 + 170
  PhiSetupUI::Place $hSep3 33 $4 534 1
  IntOp $4 $3 + 175
  PhiSetupUI::Place $hR3Label 84 $4 300 20
  IntOp $4 $3 + 195
  PhiSetupUI::Place $hR3Hint 84 $4 300 16
  IntOp $4 $3 + 185
  PhiSetupUI::Place $hToggle 476 $4 40 20
  IntOp $4 $3 + 187
  PhiSetupUI::Place $hToggleText 524 $4 28 16
  PhiSetupUI::Show $hSep1 $Expanded
  PhiSetupUI::Show $hR1Label $Expanded
  PhiSetupUI::Show $hR1Field $Expanded
  PhiSetupUI::Show $hR1Edit $Expanded
  PhiSetupUI::Show $hR1Browse $Expanded
  PhiSetupUI::Show $hSep2 $Expanded
  PhiSetupUI::Show $hR2Label $Expanded
  PhiSetupUI::Show $hR2Hint $Expanded
  PhiSetupUI::Show $hR2Field $Expanded
  PhiSetupUI::Show $hR2Edit $Expanded
  PhiSetupUI::Show $hR2Browse $Expanded
  PhiSetupUI::Show $hSep3 $Expanded
  PhiSetupUI::Show $hR3Label $Expanded
  PhiSetupUI::Show $hR3Hint $Expanded
  PhiSetupUI::Show $hToggle $Expanded
  PhiSetupUI::Show $hToggleText $Expanded
FunctionEnd

; The footer for the Setup screen's state.
Function SetupButtons
  PhiSetupUI::Buttons "pri" $Verb "Cancel"
  GetDlgItem $0 $HWNDPARENT 1
  ${If} $Running == 1
    EnableWindow $0 0
    PhiSetupUI::Footer "Waiting for Phi Browser to close…"
  ${Else}
    EnableWindow $0 1
    PhiSetupUI::Footer ""
  ${EndIf}
FunctionEnd

Function SetupChevron
  ${If} $Expanded == 1
    StrCpy $Expanded 0
  ${Else}
    StrCpy $Expanded 1
  ${EndIf}
  StrCpy $ErrorShown 0
  Call SetupLayout
FunctionEnd

Function SetupToggle
  ${If} $DesktopShortcut == 1
    StrCpy $DesktopShortcut 0
    PhiSetupUI::Text $hToggleText "Off"
  ${Else}
    StrCpy $DesktopShortcut 1
    PhiSetupUI::Text $hToggleText "On"
  ${EndIf}
  PhiSetupUI::Set $hToggle $DesktopShortcut
FunctionEnd

Function SetupBrowseProgram
  ${NSD_GetText} $hR1Edit $1
  nsDialogs::SelectFolderDialog "Choose the folder for the Phi Browser program" "$1"
  Pop $0
  ${If} $0 != error
    ; The picked folder is the PARENT: Phi gets a "Phi Browser" folder of its
    ; own in it (a drive root comes back as D:\), unless it is one already.
    StrCpy $1 $0 1 -1
    ${If} $1 == "\"
      StrCpy $0 $0 -1
    ${EndIf}
    StrCpy $1 $0 "" -12
    ${If} $1 != "\Phi Browser"
      StrCpy $0 "$0\Phi Browser"
    ${EndIf}
    PhiSetupUI::Text $hR1Edit "$0"
  ${EndIf}
FunctionEnd

Function SetupBrowseData
  ${NSD_GetText} $hR2Edit $1
  nsDialogs::SelectFolderDialog "Choose the folder for Phi's browsing data" "$1"
  Pop $0
  ${If} $0 != error
    PhiSetupUI::Text $hR2Edit "$0"
  ${EndIf}
FunctionEnd

; The InfoBar's Try again and the 1 s timer both re-probe chrome.dll.
Function SetupProbe
  ${If} $PrevVersion == ""
    Return
  ${EndIf}
  Call IsPhiRunning
  ${If} $0 != $Running
    StrCpy $Running $0
    ${If} $Running == 1
      PhiSetupUI::Text $hInfoB "Phi Browser is still running."
      PhiSetupUI::Text $hInfoT "Close it to continue; Setup checks again on its own."
    ${EndIf}
    StrCpy $ErrorShown 0
    Call SetupLayout
    Call SetupButtons
  ${EndIf}
FunctionEnd

Function SetupTick
  Call SetupProbe
FunctionEnd

Function SetupCreate
  StrCpy $Stage "setup"
  PhiSetupUI::Caption "Phi Browser Setup"
  nsDialogs::Create 1018
  Pop $hDlg
  ${If} $hDlg == error
    Abort
  ${EndIf}

  ${If} $ArchState == "block"
    ; The arm64 installer on a PC that is not ARM: the screen is the notice.
    ${Create} $R9 $hDlg "blockcircle" 32 20 48 48 ""
    ${Create} $R9 $hDlg "text.subtitle" 96 22 472 28 "This installer is for Windows on ARM"
    ${Create} $R9 $hDlg "text.sub" 96 54 472 16 "PhiBrowserSetup-${PHI_FULL_VERSION}-arm64.exe"
    ${Create} $R9 $hDlg "text.lead" 32 90 536 40 "This PC is not ARM-based, so the ARM64 build cannot run here. Use the standard installer instead: PhiBrowserSetup-${PHI_FULL_VERSION}.exe."
    PhiSetupUI::Buttons "pri" "Close" ""
    PhiSetupUI::Footer ""
    nsDialogs::Show
    Return
  ${EndIf}
  ${If} $Downgrade == 1
    ; A newer version is installed: the screen is the notice.
    ${Create} $R9 $hDlg "blockcircle" 32 20 48 48 ""
    ${Create} $R9 $hDlg "text.subtitle" 96 22 472 28 "A newer Phi Browser is installed"
    ${Create} $R9 $hDlg "text.sub" 96 54 472 16 "Installed $PrevVersion · this installer ${PHI_FULL_VERSION}"
    ${Create} $R9 $hDlg "text.lead" 32 90 536 60 "Setup doesn't replace a newer version with an older one. To go back anyway, run this installer with /ALLOWDOWNGRADE - your browsing data stays where it is, but a newer version's profile may not open in an older one."
    PhiSetupUI::Buttons "pri" "Close" ""
    PhiSetupUI::Footer ""
    nsDialogs::Show
    Return
  ${EndIf}

  ${Header} $hDlg "Phi Browser" "$SubLine"
  ${If} $PrevVersion != ""
    ${Create} $hLead $hDlg "text.lead" 32 90 536 40 "Your Spaces, settings and browsing data stay where they are."
  ${Else}
    ${Create} $hLead $hDlg "text.lead" 32 90 536 40 "A browser built around Spaces and agents. Setup puts the program in Program Files and keeps your browsing data in a folder of its own."
  ${EndIf}

  ; The InfoBar (hidden until a state needs it).
  ${Create} $hInfo $hDlg "infobar.warn" 32 90 536 60 ""
  ${Create} $hInfoGlyph $hDlg "glyph:E7BA/warn" 46 110 20 20 ""
  ${Create} $hInfoB $hDlg "text.warnb" 78 100 380 20 ""
  ${Create} $hInfoT $hDlg "text.warn" 78 120 380 20 ""
  ${NsdButton} $hInfoBtn "btn.sm/warn" 470 106 86 28 "Try again"
  ${NSD_OnClick} $hInfoBtn SetupProbe

  ; The expander card.
  ${Create} $hCard $hDlg "card" 32 146 536 56 ""
  ${Create} $hXGlyph $hDlg "glyph:E8B7/card" 48 164 20 20 ""
  ${Create} $hXLabel $hDlg "text.label/card" 84 156 400 20 "Install location and browsing data"
  ${If} $PrevVersion != ""
    ${Create} $hXHint $hDlg "text.hint/card" 84 176 420 16 "Recorded from the previous install · $INSTDIR"
  ${Else}
    ${Create} $hXHint $hDlg "text.hint/card" 84 176 420 16 "Program in Program Files · browsing data in this user's app data"
  ${EndIf}
  ${NsdButton} $hChev "chev/card" 520 158 32 32 ""
  ${NSD_OnClick} $hChev SetupChevron
  ; Row 1: program folder.
  ${Create} $hSep1 $hDlg "sep/card" 33 202 534 1 ""
  ${Create} $hR1Label $hDlg "text.label/card" 84 217 120 20 "Program folder"
  ${Create} $hR1Field $hDlg "field/card" 214 211 254 32 ""
  ${Create} $hR1Edit $hDlg "edit" 224 217 234 20 "$INSTDIR"
  ${NsdButton} $hR1Browse "btn.sm/card" 480 213 72 28 "Browse…"
  ${NSD_OnClick} $hR1Browse SetupBrowseProgram
  ; Row 2: browsing data.
  ${Create} $hSep2 $hDlg "sep/card" 33 251 534 1 ""
  ${Create} $hR2Label $hDlg "text.label/card" 84 260 122 20 "Browsing data"
  ${Create} $hR2Hint $hDlg "text.hint/card" 84 280 126 32 "An update never touches this folder."
  ${Create} $hR2Field $hDlg "field/card" 214 268 254 32 ""
  ${Create} $hR2Edit $hDlg "edit" 224 274 234 20 "$DataDir"
  ${NsdButton} $hR2Browse "btn.sm/card" 480 270 72 28 "Browse…"
  ${NSD_OnClick} $hR2Browse SetupBrowseData
  ; Row 3: desktop shortcut.
  ${Create} $hSep3 $hDlg "sep/card" 33 316 534 1 ""
  ${Create} $hR3Label $hDlg "text.label/card" 84 321 300 20 "Desktop shortcut"
  ${Create} $hR3Hint $hDlg "text.hint/card" 84 341 300 16 "A Start menu entry is always added."
  ${NsdButton} $hToggle "toggle/card" 476 331 40 20 ""
  ${NSD_OnClick} $hToggle SetupToggle
  PhiSetupUI::Set $hToggle $DesktopShortcut
  ${If} $DesktopShortcut == 1
    ${Create} $hToggleText $hDlg "text.label/card" 524 333 28 16 "On"
  ${Else}
    ${Create} $hToggleText $hDlg "text.label/card" 524 333 28 16 "Off"
  ${EndIf}

  ${If} $ArchState == "warn"
    PhiSetupUI::Text $hInfoB "This PC is Windows on ARM."
    PhiSetupUI::Text $hInfoT "This installer carries the x64 build, which runs here through emulation."
  ${EndIf}
  StrCpy $ErrorShown 0
  Call IsPhiRunning
  ${If} $PrevVersion == ""
    StrCpy $0 0
  ${EndIf}
  StrCpy $Running $0
  ${If} $Running == 1
    PhiSetupUI::Text $hInfoB "Phi Browser is still running."
    PhiSetupUI::Text $hInfoT "Close it to continue; Setup checks again on its own."
  ${EndIf}
  Call SetupLayout
  Call SetupButtons
  ${If} $PrevVersion != ""
    ${NSD_CreateTimer} SetupTick 1000
  ${EndIf}
  nsDialogs::Show
FunctionEnd

; Shows the InfoBar with an error and stays on the screen.
!macro SetupError bold text
  StrCpy $ErrorShown 1
  StrCpy $Expanded 0
  PhiSetupUI::Text $hInfoB "${bold}"
  PhiSetupUI::Text $hInfoT "${text}"
  Call SetupLayout
  Abort
!macroend

Function SetupLeave
  ${If} $ArchState == "block"
    Quit
  ${EndIf}
  ${If} $Downgrade == 1
    SetErrorLevel 3
    Quit
  ${EndIf}
  ${NSD_KillTimer} SetupTick
  ${If} $Running == 1
    Abort
  ${EndIf}
  ; The program folder: a folder we cannot write is no place to install.
  ${NSD_GetText} $hR1Edit $INSTDIR
  ClearErrors
  CreateDirectory "$INSTDIR"
  FileOpen $0 "$INSTDIR\.phi-write-probe" w
  ${If} ${Errors}
    !insertmacro SetupError "Phi Browser can't be installed in $INSTDIR." "Choose a program folder you can write to."
  ${EndIf}
  FileClose $0
  Delete "$INSTDIR\.phi-write-probe"
  ; The data folder: trim trailing separators - Chromium is unhappy with
  ; them - then the same probe.
  ${NSD_GetText} $hR2Edit $DataDir
  loop:
    StrCpy $0 $DataDir 1 -1
    ${If} $0 == "\"
    ${OrIf} $0 == "/"
      StrCpy $DataDir $DataDir -1
      Goto loop
    ${EndIf}
  StrCpy $0 $DataDir 2 1
  ${If} $DataDir == ""
  ${OrIf} $0 != ":\"
    !insertmacro SetupError "Enter a full folder path for the browsing data." "Something like D:\Phi Data."
  ${EndIf}
  ; A folder other than the recorded one is Phi's own only if it is new or
  ; empty now - checked before the probe below creates it.
  ${If} $DataDir != $DataDirRecorded
    StrCpy $DataDirOwned 1
    FindFirst $0 $1 "$DataDir\*.*"
    ${DoWhile} $1 != ""
      ${If} $1 != "."
      ${AndIf} $1 != ".."
        StrCpy $DataDirOwned 0
        ${Break}
      ${EndIf}
      FindNext $0 $1
    ${Loop}
    FindClose $0
  ${EndIf}
  ClearErrors
  CreateDirectory "$DataDir"
  FileOpen $0 "$DataDir\.phi-write-probe" w
  ${If} ${Errors}
    !insertmacro SetupError "Phi Browser can't write to $DataDir." "Choose a browsing-data folder you can write to."
  ${EndIf}
  FileClose $0
  Delete "$DataDir\.phi-write-probe"
  StrCpy $DataDirChosen 1
  StrCpy $Stage "install"
FunctionEnd

; ---------------------------------------------------------------- progress screen
Function InstShow
  ${If} $PrevVersion != ""
    !insertmacro DecorateInstFiles "Phi Browser Setup" "Phi Browser" "$SubLine" "$VerbIng" "Installing to $INSTDIR" "Your Spaces, settings and browsing data stay where they are. Phi Browser opens as you, not as the installer."
  ${Else}
    !insertmacro DecorateInstFiles "Phi Browser Setup" "Phi Browser" "$SubLine" "$VerbIng" "Installing to $INSTDIR" "Phi Browser opens as you, not as the installer."
  ${EndIf}
FunctionEnd

; ---------------------------------------------------------------- done screen
Function DoneCreate
  StrCpy $Stage "done"
  nsDialogs::Create 1018
  Pop $hDlg
  ${If} $hDlg == error
    Abort
  ${EndIf}
  ${Header} $hDlg "Phi Browser" "$SubLine"
  ${Create} $R9 $hDlg "card" 32 90 536 116 ""
  ${Create} $R9 $hDlg "okcircle/card" 48 106 40 40 ""
  ${Create} $R9 $hDlg "text.strong/card" 104 106 440 20 "Phi Browser is installed"
  ${Create} $R9 $hDlg "text.hint/card" 104 134 100 16 "Program"
  ${Create} $R9 $hDlg "text.caption/card" 216 134 336 16 "$INSTDIR"
  ${Create} $R9 $hDlg "text.hint/card" 104 154 100 16 "Browsing data"
  ${Create} $R9 $hDlg "text.caption/card" 216 154 336 16 "$DataDir"
  ${Create} $R9 $hDlg "text.hint/card" 104 174 100 16 "Shortcuts"
  ${If} $DesktopShortcut == 1
    ${Create} $R9 $hDlg "text.caption/card" 216 174 336 16 "Start menu · Desktop"
  ${Else}
    ${Create} $R9 $hDlg "text.caption/card" 216 174 336 16 "Start menu"
  ${EndIf}
  PhiSetupUI::Buttons "pri" "Launch Phi Browser" "Close"
  PhiSetupUI::Footer "Updates arrive on their own."
  nsDialogs::Show
FunctionEnd

Function DoneLeave
  Call LaunchPhi
FunctionEnd

; ---------------------------------------------------------------- default browser
!ifndef PHI_UI_TEST
; What Chrome's setup registers for a system-level install (shell_util.cc:
; GetChromeProgIdEntries, GetShellIntegrationEntries,
; GetChromeAppRegistrationEntries, GetDirectLaunchEntries), in HKLM's
; 64-bit view (the caller's SetRegView). It makes Phi a browser Windows can
; offer; choosing it stays the user's, in Settings.
!macro BrowserFileType ext progid
  WriteRegStr HKLM "${BROWSER_CLIENT_KEY}\Capabilities\FileAssociations" "${ext}" "${progid}"
  WriteRegStr HKLM "Software\Classes\${ext}\OpenWithProgids" "${progid}" ""
!macroend

!macro BrowserUrlType scheme
  WriteRegStr HKLM "${BROWSER_CLIENT_KEY}\Capabilities\URLAssociations" "${scheme}" "${BROWSER_HTML_PROGID}"
!macroend

!macro BrowserProgId progid desc
  WriteRegStr HKLM "Software\Classes\${progid}" "" "${desc}"
  WriteRegStr HKLM "Software\Classes\${progid}" "AppUserModelId" "${BROWSER_APP}"
  WriteRegStr HKLM "Software\Classes\${progid}\DefaultIcon" "" $R3
  WriteRegStr HKLM "Software\Classes\${progid}\shell\open\command" "" $R2
  WriteRegStr HKLM "Software\Classes\${progid}\Application" "AppUserModelId" "${BROWSER_APP}"
  WriteRegStr HKLM "Software\Classes\${progid}\Application" "ApplicationIcon" $R3
  WriteRegStr HKLM "Software\Classes\${progid}\Application" "ApplicationName" "${BROWSER_NAME}"
  WriteRegStr HKLM "Software\Classes\${progid}\Application" "ApplicationDescription" "${BROWSER_TOOLTIP}"
  WriteRegStr HKLM "Software\Classes\${progid}\Application" "ApplicationCompany" "${BROWSER_COMPANY}"
!macroend

Function RegisterBrowser
  ; The brand's names are Chromium's own, so a Chromium build on this PC may
  ; hold them: a client key or HTML ProgId that launches anything but a
  ; Phi.exe is left alone - taking it over would send that browser's links
  ; to Phi.
  StrCpy $2 "\Phi.exe"
  ReadRegStr $0 HKLM "${BROWSER_CLIENT_KEY}\shell\open\command" ""
  ${If} $0 != ""
    Call StrHas
    ${If} $1 == 0
      DetailPrint "Default-browser registration skipped: ${BROWSER_APP} belongs to $0"
      Return
    ${EndIf}
  ${EndIf}
  ReadRegStr $0 HKLM "Software\Classes\${BROWSER_HTML_PROGID}\shell\open\command" ""
  ${If} $0 != ""
    Call StrHas
    ${If} $1 == 0
      DetailPrint "Default-browser registration skipped: ${BROWSER_HTML_PROGID} belongs to $0"
      Return
    ${EndIf}
  ${EndIf}
  StrCpy $R1 '"$INSTDIR\Phi.exe"'
  ; ShellUtil::GetChromeShellOpenCmd: the URL or file is one argument.
  StrCpy $R2 '"$INSTDIR\Phi.exe" --single-argument %1'
  StrCpy $R3 "$INSTDIR\Phi.exe,0"

  ; The ProgIds: HTML (pages, http/https and the rest) and PDF.
  !insertmacro BrowserProgId "${BROWSER_HTML_PROGID}" "Chromium HTML Document"
  !insertmacro BrowserProgId "${BROWSER_PDF_PROGID}" "Chromium PDF Document"

  ; StartMenuInternet\<app>: the browser client, its install info and the
  ; Default Programs capabilities.
  WriteRegStr HKLM "${BROWSER_CLIENT_KEY}" "" "${BROWSER_NAME}"
  WriteRegStr HKLM "${BROWSER_CLIENT_KEY}\shell\open\command" "" $R1
  WriteRegStr HKLM "${BROWSER_CLIENT_KEY}\DefaultIcon" "" $R3
  WriteRegStr HKLM "${BROWSER_CLIENT_KEY}\InstallInfo" "ReinstallCommand" "$R1 --make-default-browser"
  WriteRegStr HKLM "${BROWSER_CLIENT_KEY}\InstallInfo" "HideIconsCommand" "$R1 --hide-icons"
  WriteRegStr HKLM "${BROWSER_CLIENT_KEY}\InstallInfo" "ShowIconsCommand" "$R1 --show-icons"
  WriteRegDWORD HKLM "${BROWSER_CLIENT_KEY}\InstallInfo" "IconsVisible" 1
  WriteRegStr HKLM "${BROWSER_CLIENT_KEY}\Capabilities" "ApplicationDescription" "${BROWSER_LONG_DESC}"
  WriteRegStr HKLM "${BROWSER_CLIENT_KEY}\Capabilities" "ApplicationIcon" $R3
  WriteRegStr HKLM "${BROWSER_CLIENT_KEY}\Capabilities" "ApplicationName" "${BROWSER_NAME}"
  WriteRegStr HKLM "${BROWSER_CLIENT_KEY}\Capabilities\Startmenu" "StartMenuInternet" "${BROWSER_APP}"
  ; ShellUtil::kPotentialFileAssociations, and .pdf to the PDF ProgId.
  !insertmacro BrowserFileType ".htm" "${BROWSER_HTML_PROGID}"
  !insertmacro BrowserFileType ".html" "${BROWSER_HTML_PROGID}"
  !insertmacro BrowserFileType ".mhtml" "${BROWSER_HTML_PROGID}"
  !insertmacro BrowserFileType ".shtml" "${BROWSER_HTML_PROGID}"
  !insertmacro BrowserFileType ".svg" "${BROWSER_HTML_PROGID}"
  !insertmacro BrowserFileType ".xht" "${BROWSER_HTML_PROGID}"
  !insertmacro BrowserFileType ".xhtml" "${BROWSER_HTML_PROGID}"
  !insertmacro BrowserFileType ".webp" "${BROWSER_HTML_PROGID}"
  !insertmacro BrowserFileType ".pdf" "${BROWSER_PDF_PROGID}"
  ; ShellUtil::kPotentialProtocolAssociations, and the direct-launch scheme.
  !insertmacro BrowserUrlType "http"
  !insertmacro BrowserUrlType "https"
  !insertmacro BrowserUrlType "irc"
  !insertmacro BrowserUrlType "mailto"
  !insertmacro BrowserUrlType "mms"
  !insertmacro BrowserUrlType "news"
  !insertmacro BrowserUrlType "nntp"
  !insertmacro BrowserUrlType "sms"
  !insertmacro BrowserUrlType "smsto"
  !insertmacro BrowserUrlType "snews"
  !insertmacro BrowserUrlType "tel"
  !insertmacro BrowserUrlType "urn"
  !insertmacro BrowserUrlType "webcal"
  !insertmacro BrowserUrlType "${BROWSER_URL_SCHEME}"
  WriteRegStr HKLM "Software\RegisteredApplications" "${BROWSER_APP}" "${BROWSER_CLIENT_KEY}\Capabilities"

  ; App Paths (Win+R "phi") and the direct-launch scheme itself.
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\App Paths\Phi.exe" "" "$INSTDIR\Phi.exe"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\App Paths\Phi.exe" "Path" "$INSTDIR"
  WriteRegStr HKLM "Software\Classes\${BROWSER_URL_SCHEME}" "" "URL:${BROWSER_URL_SCHEME}"
  WriteRegStr HKLM "Software\Classes\${BROWSER_URL_SCHEME}" "URL Protocol" ""
  WriteRegStr HKLM "Software\Classes\${BROWSER_URL_SCHEME}\shell\open\command" "" $R2

  ; SHCNE_ASSOCCHANGED, SHCNF_IDLIST - as RegisterChromeBrowserImpl does.
  System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'
FunctionEnd

; The uninstaller's half: in ROOT, everything above - and what chrome.dll
; itself writes there (a per-user install registers in HKCU when "Make
; default" is pressed; making Phi default adds the XP-style http/https and
; StartMenuInternet values) - but each key only while it launches this
; install's exe ($2).
!macro UnBrowserFileType root ext progid
  DeleteRegValue ${root} "Software\Classes\${ext}\OpenWithProgids" "${progid}"
  ReadRegStr $0 ${root} "Software\Classes\${ext}" ""
  ${If} $0 == "${progid}"
    DeleteRegValue ${root} "Software\Classes\${ext}" ""
  ${EndIf}
!macroend

!macro UnBrowserScheme root scheme
  ReadRegStr $0 ${root} "Software\Classes\${scheme}\shell\open\command" ""
  Call un.StrHas
  ${If} $1 == 1
    DeleteRegKey ${root} "Software\Classes\${scheme}"
  ${EndIf}
!macroend

!macro UnregisterBrowserIn root
  ReadRegStr $0 ${root} "${BROWSER_CLIENT_KEY}\shell\open\command" ""
  Call un.StrHas
  ${If} $1 == 1
    DeleteRegKey ${root} "${BROWSER_CLIENT_KEY}"
    ReadRegStr $0 ${root} "Software\RegisteredApplications" "${BROWSER_APP}"
    ${If} $0 == "${BROWSER_CLIENT_KEY}\Capabilities"
      DeleteRegValue ${root} "Software\RegisteredApplications" "${BROWSER_APP}"
    ${EndIf}
    ReadRegStr $0 ${root} "Software\Clients\StartMenuInternet" ""
    ${If} $0 == "${BROWSER_APP}"
      DeleteRegValue ${root} "Software\Clients\StartMenuInternet" ""
    ${EndIf}
  ${EndIf}
  ReadRegStr $0 ${root} "Software\Classes\${BROWSER_HTML_PROGID}\shell\open\command" ""
  Call un.StrHas
  ${If} $1 == 1
    DeleteRegKey ${root} "Software\Classes\${BROWSER_HTML_PROGID}"
    !insertmacro UnBrowserFileType ${root} ".htm" "${BROWSER_HTML_PROGID}"
    !insertmacro UnBrowserFileType ${root} ".html" "${BROWSER_HTML_PROGID}"
    !insertmacro UnBrowserFileType ${root} ".mhtml" "${BROWSER_HTML_PROGID}"
    !insertmacro UnBrowserFileType ${root} ".shtml" "${BROWSER_HTML_PROGID}"
    !insertmacro UnBrowserFileType ${root} ".svg" "${BROWSER_HTML_PROGID}"
    !insertmacro UnBrowserFileType ${root} ".xht" "${BROWSER_HTML_PROGID}"
    !insertmacro UnBrowserFileType ${root} ".xhtml" "${BROWSER_HTML_PROGID}"
    !insertmacro UnBrowserFileType ${root} ".webp" "${BROWSER_HTML_PROGID}"
  ${EndIf}
  ReadRegStr $0 ${root} "Software\Classes\${BROWSER_PDF_PROGID}\shell\open\command" ""
  Call un.StrHas
  ${If} $1 == 1
    DeleteRegKey ${root} "Software\Classes\${BROWSER_PDF_PROGID}"
    !insertmacro UnBrowserFileType ${root} ".pdf" "${BROWSER_PDF_PROGID}"
  ${EndIf}
  !insertmacro UnBrowserScheme ${root} "${BROWSER_URL_SCHEME}"
  !insertmacro UnBrowserScheme ${root} "http"
  !insertmacro UnBrowserScheme ${root} "https"
  ReadRegStr $0 ${root} "Software\Microsoft\Windows\CurrentVersion\App Paths\Phi.exe" ""
  ${If} $0 == "$INSTDIR\Phi.exe"
    DeleteRegKey ${root} "Software\Microsoft\Windows\CurrentVersion\App Paths\Phi.exe"
  ${EndIf}
!macroend

Function un.UnregisterBrowser
  StrCpy $2 "$INSTDIR\Phi.exe"
  !insertmacro UnregisterBrowserIn HKLM
  !insertmacro UnregisterBrowserIn HKCU
  System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'
FunctionEnd
!endif

; ---------------------------------------------------------------- install
Section "Install"
  SetShellVarContext all

  ; The update path must not race a running Phi: chrome.dll being locked is
  ; the tell - it stays mapped for as long as any Phi process lives. The
  ; probe OPENS it for writing (a rename is no test: Windows lets a mapped
  ; image be renamed, only not overwritten). WinSparkle launches this
  ; installer and THEN asks Phi to quit, so a locked file is the normal
  ; first sight here - the Setup screen already waited with its InfoBar;
  ; this is the safety net for silent runs - wait up to a minute for
  ; Chromium to unwind before asking, ask-and-retry rather than failing
  ; halfway through a copy. The chrome.dll a previous install keeps mapped
  ; is framework\chrome.dll (the ship layout) or chrome.dll beside the exe
  ; (the flat, component-era layout) - whichever is there.
  check_running:
    StrCpy $2 "$INSTDIR\framework\chrome.dll"
    IfFileExists $2 have_dll
    StrCpy $2 "$INSTDIR\chrome.dll"
    IfFileExists $2 have_dll fresh_install
  have_dll:
    StrCpy $1 0
  wait_running:
    ClearErrors
    FileOpen $0 $2 a
    IfErrors 0 not_running
      IntOp $1 $1 + 1
      IntCmp $1 120 ask_running 0 ask_running
      Sleep 500
      Goto wait_running
  ask_running:
    MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION \
      "Phi Browser is running. Close it, then press Retry." \
      IDRETRY check_running
    Abort
  not_running:
    FileClose $0

!ifdef PHI_LAYOUT_FRAMEWORK
    ; Upgrading a FLAT install: its 500-odd DLLs, paks and manifests sit
    ; beside the exe and nothing in the framework\ layout would ever
    ; remove them. Everything at the top level but the client's own
    ; resources goes; the stage puts back what belongs there. Only when
    ; the folder IS that layout - Phi.exe with chrome.dll beside it and no
    ; framework\ - never as a sweep of whatever folder holds an install.
    ${If} ${FileExists} "$INSTDIR\chrome.dll"
    ${AndIf} ${FileExists} "$INSTDIR\Phi.exe"
    ${AndIfNot} ${FileExists} "$INSTDIR\framework\chrome.dll"
      Delete "$INSTDIR\*.dll"
      Delete "$INSTDIR\*.pak"
      Delete "$INSTDIR\*.bin"
      Delete "$INSTDIR\*.dat"
      Delete "$INSTDIR\*.manifest"
      Delete "$INSTDIR\Phi.exe"
      Delete "$INSTDIR\chrome_proxy.exe"
      RMDir /r "$INSTDIR\locales"
      RMDir /r "$INSTDIR\MEIPreload"
      RMDir /r "$INSTDIR\Dictionaries"
      RMDir /r "$INSTDIR\resources\accessibility"
      RMDir /r "$INSTDIR\resources\inspector_overlay"
    ${EndIf}
!endif

    ; The previous framework\ goes whole - it is Phi's (its chrome.dll is
    ; the one just probed) - so nothing a previous version had and this
    ; one dropped lingers past the uninstaller's file list. The rollback\
    ; snapshot earlier installers made (never restored from) goes too.
    ${If} ${FileExists} "$INSTDIR\framework\chrome.dll"
      RMDir /r "$INSTDIR\framework"
    ${EndIf}
    ${If} ${FileExists} "$INSTDIR\rollback\Phi.exe"
      RMDir /r "$INSTDIR\rollback"
    ${EndIf}
    ; The Widevine VMP signature of the previous Phi.exe (make-installer.sh
    ; --vmp; chrome.dll's went with framework\) - an update without one
    ; would leave it behind, matching nothing and missing from the uninstall
    ; list.
    Delete "$INSTDIR\Phi.exe.sig"

  fresh_install:
  SetOutPath "$INSTDIR"
  File /r "${PHI_STAGE}\*.*"

  ; The sandbox's AppContainer utilities (proxy resolver, media CDM, ...)
  ; run under the chromeInstallFiles / lpacChromeInstallFiles capabilities
  ; and must read and execute the install tree; a user folder grants them
  ; nothing by default and the launch then fails (sandbox_win.cc's
  ; AddAppContainerProfileToConfig access check). Chrome's setup grants the
  ; same two capability SIDs (installer/setup/configure_app_container_
  ; sandbox.cc).
  nsExec::ExecToLog 'icacls "$INSTDIR" /grant "*S-1-15-3-1024-3424233489-972189580-2057154623-747635277-1604371224-316187997-3786583170-1043257646:(OI)(CI)(RX)" "*S-1-15-3-1024-2302894289-466761758-1166120688-1039016420-2430351297-4240214049-4028510897-3317428798:(OI)(CI)(RX)"'

  CreateShortCut "$SMPROGRAMS\Phi Browser.lnk" "$INSTDIR\Phi.exe"
  ${If} $DesktopShortcut == 1
    CreateShortCut "$DESKTOP\Phi Browser.lnk" "$INSTDIR\Phi.exe"
  ${EndIf}

  ; The program folder, for the next (silent) update and the uninstaller;
  ; the data folder only when the user chose one on the Setup screen - a
  ; silent update keeps whatever is recorded. The default is recorded as
  ; "no value" so a later move back to the default is a plain delete.
  WriteRegStr HKLM "${REG_PRODUCT}" "InstallDir" "$INSTDIR"
  WriteRegStr HKCU "${REG_PRODUCT}" "InstallDir" "$INSTDIR"
  ${If} $DataDirChosen == 1
    ${If} $DataDir == $DataDirDefault
    ${OrIf} $DataDirOwned != 1
      DeleteRegValue HKLM "${REG_PRODUCT}" "UserDataDirOwned"
      DeleteRegValue HKCU "${REG_PRODUCT}" "UserDataDirOwned"
    ${Else}
      WriteRegDWORD HKLM "${REG_PRODUCT}" "UserDataDirOwned" 1
      WriteRegDWORD HKCU "${REG_PRODUCT}" "UserDataDirOwned" 1
    ${EndIf}
    ${If} $DataDir == $DataDirDefault
      DeleteRegValue HKLM "${REG_PRODUCT}" "UserDataDir"
      DeleteRegValue HKCU "${REG_PRODUCT}" "UserDataDir"
    ${Else}
      WriteRegStr HKLM "${REG_PRODUCT}" "UserDataDir" "$DataDir"
      WriteRegStr HKCU "${REG_PRODUCT}" "UserDataDir" "$DataDir"
    ${EndIf}
  ${EndIf}

!ifdef PHI_TELEMETRY_JSON
  ; Deployment configuration (docs/sentry-crash-reporting.md): the crash
  ; endpoint and the analytics key, read once at startup from the data
  ; folder. The installer OWNS this file - rewritten on every install so a
  ; rotated key or endpoint reaches the machine with the next update. The
  ; file names WHERE reports go; WHETHER anything goes is the user's
  ; data-sharing switch in the app, for crash uploads and analytics alike.
  CreateDirectory "$DataDir"
  SetOutPath "$DataDir"
  File "/oname=phi_telemetry.json" "${PHI_TELEMETRY_JSON}"
  SetOutPath "$INSTDIR"
!endif
  ; Installers before 2026-10-14 wrote usagestats = 1 here (HKLM, and HKCU
  ; for the session user), which had Crashpad uploading with no consent.
  ; The HKLM value is not read any more and goes; the HKCU value is the
  ; client's now - it rewrites it from the user's choice on every launch.
  DeleteRegValue HKLM "${REG_PRODUCT}" "usagestats"

  WriteUninstaller "$INSTDIR\Uninstall.exe"
  ; Apps & Features: the 64-bit view, where a 64-bit program's entry
  ; belongs (this stub is 32-bit, so the default view would be WOW6432Node).
  ; Software\Phi above stays in the 32-bit view - that is where
  ; install_static reads it (nt::WOW6432).
  SetRegView 64
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayName" "Phi Browser"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayVersion" "${PHI_FULL_VERSION}"
  WriteRegStr HKLM "${UNINST_KEY}" "Publisher" "Fyde Innovations"
  WriteRegStr HKLM "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayIcon" "$INSTDIR\Phi.exe"
  WriteRegStr HKLM "${UNINST_KEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegStr HKLM "${UNINST_KEY}" "QuietUninstallString" '"$INSTDIR\Uninstall.exe" /S'
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoRepair" 1
!ifndef PHI_UI_TEST
  Call RegisterBrowser
!endif
  SetRegView 32
  ; On to the Done screen without a click (AutoCloseWindow covers the
  ; installer; the flag is set again here so the uninstaller's copy below
  ; reads the same).
  SetAutoClose true
SectionEnd

; ---------------------------------------------------------------- uninstall
Function un.onInit
  StrCpy $DataDirDefault "$LOCALAPPDATA\Phi\User Data"
  ReadRegStr $DataDir HKCU "${REG_PRODUCT}" "UserDataDir"
  ${If} $DataDir == ""
    ReadRegStr $DataDir HKLM "${REG_PRODUCT}" "UserDataDir"
  ${EndIf}
  ${If} $DataDir == ""
    StrCpy $DataDir $DataDirDefault
  ${EndIf}
  !insertmacro ReadDataDirOwned
  StrCpy $DeleteData 0
FunctionEnd

; "Delete my data" in a folder Setup did not create: only what Phi and
; Chromium keep there, by name - the profiles (a folder counts only with
; its Preferences in it), the browser-wide state, Phi's own store and
; files. Anything else stays, and so does the folder unless it is empty.
!macro UnDataDir name
  RMDir /r "$DataDir\${name}"
!macroend
!macro UnDataFile name
  Delete "$DataDir\${name}"
!macroend
Function un.DeletePhiData
  ${If} ${FileExists} "$DataDir\Default\Preferences"
    RMDir /r "$DataDir\Default"
  ${EndIf}
  ${If} ${FileExists} "$DataDir\Guest Profile\Preferences"
    RMDir /r "$DataDir\Guest Profile"
  ${EndIf}
  ${If} ${FileExists} "$DataDir\System Profile\Preferences"
    RMDir /r "$DataDir\System Profile"
  ${EndIf}
  FindFirst $0 $1 "$DataDir\Profile *"
  ${DoWhile} $1 != ""
    ${If} ${FileExists} "$DataDir\$1\Preferences"
      RMDir /r "$DataDir\$1"
    ${EndIf}
    FindNext $0 $1
  ${Loop}
  FindClose $0
  ${If} ${FileExists} "$DataDir\Phi\users\*.*"
  ${OrIf} ${FileExists} "$DataDir\Phi\syncstate\*.*"
  ${OrIf} ${FileExists} "$DataDir\Phi\guestDataMigration\*.*"
    RMDir /r "$DataDir\Phi"
  ${EndIf}
  !insertmacro UnDataDir "Crashpad"
  !insertmacro UnDataDir "BrowserMetrics"
  !insertmacro UnDataDir "DeferredBrowserMetrics"
  !insertmacro UnDataDir "Safe Browsing"
  !insertmacro UnDataDir "ShaderCache"
  !insertmacro UnDataDir "GrShaderCache"
  !insertmacro UnDataDir "GraphiteDawnCache"
  !insertmacro UnDataDir "Webstore Downloads"
  !insertmacro UnDataDir "component_crx_cache"
  !insertmacro UnDataDir "extensions_crx_cache"
  !insertmacro UnDataFile "Local State"
  !insertmacro UnDataFile "First Run"
  !insertmacro UnDataFile "Last Version"
  !insertmacro UnDataFile "Last Browser"
  !insertmacro UnDataFile "lockfile"
  !insertmacro UnDataFile "Variations"
  !insertmacro UnDataFile "chrome_debug.log"
  !insertmacro UnDataFile "BrowserMetrics-spare.pma"
  !insertmacro UnDataFile "CrashpadMetrics-active.pma"
  !insertmacro UnDataFile "CDPAgentSocket"
  !insertmacro UnDataFile "PhiAuth.dat"
  !insertmacro UnDataFile "PhiConsent.json"
  !insertmacro UnDataFile "PhiCredentialGrants.json"
  !insertmacro UnDataFile "PhiCredentialGrants.json.imported"
  !insertmacro UnDataFile "PhiCredentials.dat"
  !insertmacro UnDataFile "PhiGhostSpaces.json"
  !insertmacro UnDataFile "PhiGhostSpaces.json.imported"
  !insertmacro UnDataFile "PhiPreferences.json"
  !insertmacro UnDataFile "PhiPrefs.json"
  !insertmacro UnDataFile "PhiPrefs.json.imported"
  !insertmacro UnDataFile "PhiRestore.json"
  !insertmacro UnDataFile "PhiStartupProfile"
  !insertmacro UnDataFile "PhiSyncSidecars.json"
  !insertmacro UnDataFile "phi_first_time_actions"
  !insertmacro UnDataFile "phi_last_version"
  !insertmacro UnDataFile "phi_telemetry.json"
  !insertmacro UnDataFile "phi_telemetry_id"
  !insertmacro UnDataFile "phi_telemetry_spool.json"
  RMDir "$DataDir"
FunctionEnd

Function un.onGUIInit
  PhiSetupUI::Init
  Pop $Dark
  PhiSetupUI::Dpi
  Pop $Dpi
  PhiSetupUI::Frame
FunctionEnd

Function un.onUserAbort
  MessageBox MB_YESNO|MB_ICONEXCLAMATION|MB_DEFBUTTON2 "Stop removing Phi Browser?" IDYES stop
  Abort
  stop:
FunctionEnd

Function un.ConfirmCheck
  ${If} $DeleteData == 1
    StrCpy $DeleteData 0
  ${Else}
    StrCpy $DeleteData 1
  ${EndIf}
  PhiSetupUI::Set $hDeleteData $DeleteData
FunctionEnd

; The browsing data is the user's: the checkbox is off, and only offered
; when the folder looks like a Phi data folder.
Function un.ConfirmCreate
  PhiSetupUI::Caption "Uninstall Phi Browser"
  nsDialogs::Create 1018
  Pop $hDlg
  ${If} $hDlg == error
    Abort
  ${EndIf}
  ${Header} $hDlg "Remove Phi Browser" "Version ${PHI_FULL_VERSION} · $INSTDIR"
  ${Create} $R9 $hDlg "text.lead" 32 90 536 40 "The program, its shortcuts and the Apps & Features entry will be removed."
  ${If} ${FileExists} "$DataDir\Local State"
  ${OrIf} ${FileExists} "$DataDir\PhiPreferences.json"
    ${Create} $R9 $hDlg "card" 32 146 536 94 ""
    ${NsdButton} $hDeleteData "check/card" 48 160 20 20 ""
    ${NSD_OnClick} $hDeleteData un.ConfirmCheck
    ${Create} $R9 $hDlg "text.label/card" 84 158 460 20 "Also delete my browsing data"
    ${Create} $R9 $hDlg "text.hint/card" 84 180 460 48 "Spaces, history, bookmarks, settings and saved passwords in $DataDir. Leave this off to keep them for a later install."
  ${EndIf}
  PhiSetupUI::Buttons "dng" "Remove" "Cancel"
  PhiSetupUI::Footer ""
  nsDialogs::Show
FunctionEnd

Function un.ConfirmLeave
FunctionEnd

Function un.InstShow
  !insertmacro DecorateInstFiles "Uninstall Phi Browser" "Remove Phi Browser" "Version ${PHI_FULL_VERSION} · $INSTDIR" "Removing…" "Removing from $INSTDIR" "Shortcuts and the Apps & Features entry go with the program."
FunctionEnd

Section "Uninstall"
  SetShellVarContext all

  ; Not while Phi runs: chrome.dll would stay and the rest would go. The
  ; install section's probe and prompt; Cancel (and a silent run) leaves
  ; everything in place.
  check_running:
    Call un.IsPhiRunning
    ${If} $0 == 1
      MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION \
        "Phi Browser is running. Close it, then press Retry." \
        /SD IDCANCEL IDRETRY check_running
      SetErrorLevel 2
      Quit
    ${EndIf}

  ; Only what this installer put there, by name - never a wildcard or a
  ; blind RMDir /r in a folder the user chose. The payload's own list
  ; (uninstall-list.py over the stage this uninstaller was built with);
  ; each folder goes only once empty. A rollback\ snapshot an earlier
  ; installer made is all ours, so it goes whole when it holds a Phi.exe.
  !tempfile PHI_UNINSTALL_LIST
  !system 'python3 "${__FILEDIR__}/uninstall-list.py" "${PHI_STAGE}" > "${PHI_UNINSTALL_LIST}"' = 0
  !include "${PHI_UNINSTALL_LIST}"
  !delfile "${PHI_UNINSTALL_LIST}"
  !undef PHI_UNINSTALL_LIST
  Delete "$INSTDIR\Uninstall.exe"
  ${If} ${FileExists} "$INSTDIR\rollback\Phi.exe"
    RMDir /r "$INSTDIR\rollback"
  ${EndIf}
  RMDir "$INSTDIR"

  Delete "$SMPROGRAMS\Phi Browser.lnk"
  Delete "$DESKTOP\Phi Browser.lnk"

  ; The browsing data goes only when the confirm screen's checkbox was on;
  ; a silent uninstall keeps it. Whole only when the folder is Phi's: the
  ; default one (then its empty Phi parent too) or one Setup created;
  ; anywhere else only Phi's own entries in it.
  ${If} $DeleteData == 1
    ${If} ${FileExists} "$DataDir\Local State"
    ${OrIf} ${FileExists} "$DataDir\PhiPreferences.json"
      ${If} $DataDir == $DataDirDefault
        RMDir /r "$DataDir"
        StrCpy $0 $DataDirDefault -10
        RMDir "$0"
      ${ElseIf} $DataDirOwned == 1
        RMDir /r "$DataDir"
      ${Else}
        Call un.DeletePhiData
      ${EndIf}
    ${EndIf}
  ${EndIf}
  ; The deployment config is the installer's file, so it goes with the
  ; program either way (the telemetry spool and id are data: they go only
  ; with the box ticked); a data folder left empty goes too.
  Delete "$DataDir\phi_telemetry.json"
  RMDir "$DataDir"
  ${If} $DataDir == $DataDirDefault
    StrCpy $0 $DataDirDefault -10
    RMDir "$0"
  ${EndIf}

  SetRegView 64
!ifndef PHI_UI_TEST
  Call un.UnregisterBrowser
!endif
  DeleteRegKey HKLM "${UNINST_KEY}"
  SetRegView 32
  DeleteRegKey HKLM "${UNINST_KEY}"
  DeleteRegKey HKLM "${REG_PRODUCT}"
  DeleteRegKey HKCU "${REG_PRODUCT}"
  ; The updater's own state (last check time, skipped version).
  DeleteRegKey HKCU "Software\Fyde Innovations\Phi Browser\WinSparkle"
  ; The window closes by itself once the files are gone; there is no
  ; "Completed" state to click through (AutoCloseWindow does not carry
  ; the uninstaller past its last page, so this is explicit).
  Quit
SectionEnd
