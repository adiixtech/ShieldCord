; ============================================================
;  ShieldCord — shieldcord.iss
;  Inno Setup script for the finished installer.
;
;  Build:  ISCC.exe installer\shieldcord.iss
;          (BuildAll passes /DAppVersion=... — see build_all.ps1)
;
;  Produces ONE Setup.exe that installs the Windows service and the tray app,
;  registers Add/Remove Programs, and can be silently deployed with /VERYSILENT.
;
;  The wizard shows exactly two tick boxes — kernel driver files, and the tray
;  application — both ticked, and nothing else to decide.
;
;  NOTE — the kernel driver is deliberately NOT loaded here. It is signed with
;  ShieldCord's own test certificate, so Windows refuses to load it unless test
;  signing is on; enabling that changes a security setting for the whole PC and
;  has to be explained and consented to. The app's setup screen does that, and
;  can report what went wrong — this installer runs the engine step hidden, so a
;  driver that failed here reached nobody. The payload still ships in
;  {app}\driver because the app installs it from there.
;
;  Requires Inno Setup 6.3 or newer (uses ArchitecturesAllowed=x64compatible).
; ============================================================

#ifndef AppVersion
  #define AppVersion "1.2.0"
#endif

#ifndef SourceRoot
  ; installer\shieldcord.iss -> repo root
  #define SourceRoot ".."
#endif

#define AppName        "ShieldCord"
#define AppPublisher   "ShieldCord"
#define AppExeName     "shieldcordui.exe"
#define ServiceName    "ShieldCordSvc"
; The kernel filter, checked but never installed here — see the header note.
; Must match SC_FILTER_NAME in src/installer/installer_common.h.
#define DriverServiceName "ShieldCordFilter"

; ── bundled .NET Desktop Runtime ─────────────────────────────
; The tray app is framework-dependent, so it needs Microsoft.WindowsDesktop.App
; 10 — NOT the ASP.NET Core runtime, which supplies a framework this app never
; references. Drop the offline installer at installer\redist\ to bundle it;
; without it, Setup still succeeds but the tray app cannot start, and the tray
; app is now the only way to install the kernel driver.
#define DotNetRedist "redist\windowsdesktop-runtime-10-win-x64.exe"

#if FileExists(AddBackslash(SourcePath) + DotNetRedist)
  #define BundlesDotNet
#endif

; Signed driver output from build_driver_host.ps1
#define DriverBin      SourceRoot + "\build_driver\bin\Release"
; C++ binaries from build_service.bat / cmake
#define ServiceBin     SourceRoot + "\build\bin\Release"
; dotnet publish output for the tray app
#define UiPublish      SourceRoot + "\src\ui\bin\Release\net10.0-windows\win-x64\publish"

[Setup]
AppId={{8F3B2C41-7D5E-4A96-B1C4-2E7A9F0D6B33}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
VersionInfoVersion={#AppVersion}

DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes

; The folder page is shown. It is safe to let the user change it because
; shieldcord_installer.exe and shieldcord_uninstaller.exe now work from their own
; directory rather than from a hard-coded C:\Program Files\ShieldCord — before
; that, choosing another folder placed the files in one place and registered the
; service against another, producing an install that reported success and had no
; engine at all.
DisableDirPage=no
AllowNoIcons=yes

; The kernel driver and the service both require elevation, and the installer
; cannot do its job unelevated.
PrivilegesRequired=admin
PrivilegesRequiredOverridesAllowed=

; x64 only: the driver and the service are built for x64.
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible

; Ask Restart Manager to close a running tray app so its exe can be replaced
; during an upgrade. Do not relaunch it automatically — the postinstall step
; offers that instead.
CloseApplications=yes
RestartApplications=no

OutputDir={#SourceRoot}\dist
OutputBaseFilename=ShieldCord-Setup-{#AppVersion}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern

; Branded artwork, replacing Inno's own blue-arrow defaults.
;
; SetupIconFile is the icon in File Explorer — what the user clicks is what they
; get, rather than a generic setup glyph. WizardSmallImageFile is the mark in the
; wizard's top-right corner, drawn from the same artwork so the two match.
;
; There is deliberately NO WizardImageFile: the tall sidebar picture was tried
; and removed. It read as decoration bolted onto somebody else's installer, and
; the icon in the corner is the branding.
;
; Both come from tools\make_artwork.ps1, which build_all.ps1 runs before this
; script — the .ico is also embedded in shieldcordui.exe, so one file drives the
; app icon, Setup.exe and the wizard corner. Paths are bare filenames: Inno
; resolves them relative to this script's directory.
SetupIconFile=shieldcord.ico
WizardSmallImageFile=wizard-small-image.bmp

UninstallDisplayName={#AppName} {#AppVersion}
UninstallDisplayIcon={app}\ui\{#AppExeName}

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
; TWO OPTIONS, BOTH TICKED. That is the whole wizard.
;
; These are tasks rather than components on purpose. A [Components] section brings
; Inno's "Select Setup Type" dropdown with it — and the docs are explicit that
; removing [Types] does not remove it, because Inno creates a default set of types
; when components exist. A single iscustom type could suppress the list, but its
; interaction with components that have to stay optional-but-ticked is not
; documented, and getting it wrong ships an installer that quietly installs
; nothing.
;
; Tasks have none of that: no type list, no parent/child semantics, and the
; documented default is ticked unless a task carries the `unchecked` flag.
Name: "driverfiles"; Description: "Kernel driver"; GroupDescription: "Components:"
Name: "trayapp";     Description: "Tray application"; GroupDescription: "Components:"

[Files]
; ── engine binaries ──────────────────────────────────────────
; No task gate: the Windows service IS the product, and every other choice here
; only decides what is placed around it. Unticking both boxes leaves a working
; engine with no tray app and no driver payload — the headless install.
Source: "{#ServiceBin}\shieldcord_svc.exe";          DestDir: "{app}"; Flags: ignoreversion
Source: "{#ServiceBin}\shieldcord_ctl.exe";          DestDir: "{app}"; Flags: ignoreversion
Source: "{#ServiceBin}\shieldcord_installer.exe";    DestDir: "{app}"; Flags: ignoreversion
Source: "{#ServiceBin}\shieldcord_uninstaller.exe";  DestDir: "{app}"; Flags: ignoreversion
Source: "{#ServiceBin}\shieldcord_driver_setup.exe"; DestDir: "{app}"; Flags: ignoreversion

; ── driver payload ───────────────────────────────────────────
; NOT loaded by this script — the app's setup runs shieldcord_driver_setup.exe
; against these, which is why the payload ships even though nothing here installs
; it. Gated on the task so it is a visible choice rather than something that
; silently arrives.
Source: "{#DriverBin}\shieldcord_filter.sys";        DestDir: "{app}\driver"; Flags: ignoreversion; Tasks: driverfiles
Source: "{#SourceRoot}\src\driver\shieldcord_filter.inf"; DestDir: "{app}\driver"; Flags: ignoreversion; Tasks: driverfiles
Source: "{#SourceRoot}\src\driver\shieldcord_filter.cat"; DestDir: "{app}\driver"; Flags: ignoreversion; Tasks: driverfiles
Source: "{#SourceRoot}\src\driver\ShieldCordCert.cer";    DestDir: "{app}\driver"; Flags: ignoreversion; Tasks: driverfiles

; ── tray application ─────────────────────────────────────────
Source: "{#UiPublish}\*"; DestDir: "{app}\ui"; Flags: ignoreversion recursesubdirs createallsubdirs; Tasks: trayapp

; ── optional bundled .NET runtime ────────────────────────────
; Present only if the offline installer was dropped into installer\redist.
; Bundling it is what makes this Setup.exe work on a machine that has never had
; .NET: a framework-dependent tray app cannot start without it, and the tray app
; is now the only thing that can install the kernel driver.
#ifdef BundlesDotNet
Source: "{#DotNetRedist}"; DestDir: "{tmp}"; Flags: deleteafterinstall; Tasks: trayapp
#endif

[Icons]
Name: "{group}\{#AppName}";           Filename: "{app}\ui\{#AppExeName}"; Tasks: trayapp
Name: "{group}\Uninstall {#AppName}"; Filename: "{uninstallexe}"

[Run]
; 1. The engine installer does the SERVICE, and reports what actually happened.
;    --no-driver because the kernel driver cannot be installed without first
;    turning on test signing, which the app asks about explicitly. See the
;    header note. The driver payload is still unpacked for the app to use.
Filename: "{app}\shieldcord_installer.exe"; Parameters: "--no-driver"; \
    StatusMsg: "Installing the ShieldCord service..."; \
    Flags: runhidden waituntilterminated

; 2. The runtime, only when the user picked the tray app and only when it is
;    actually missing. Inno evaluates Check: when it reaches this entry, which is
;    AFTER the service step — so NeedsDotNet must be a live query, not a cached
;    one.
#ifdef BundlesDotNet
Filename: "{tmp}\windowsdesktop-runtime-10-win-x64.exe"; Parameters: "/install /quiet /norestart"; \
    StatusMsg: "Installing the .NET Desktop Runtime..."; \
    Flags: runhidden waituntilterminated; Check: NeedsDotNet; Tasks: trayapp
#endif

; 3. Offer to open the app. This is no longer a convenience: with the driver
;    installed from inside the app, opening it is how the user finishes setting
;    protection up. Left ticked for that reason.
Filename: "{app}\ui\{#AppExeName}"; Description: "Open {#AppName} to finish setup"; \
    Flags: nowait postinstall skipifsilent; Tasks: trayapp; Check: CanRunTrayApp

[UninstallRun]
; Runs BEFORE files are deleted. The uninstaller stops the service and removes
; the driver; --keep-data is chosen by the prompt in CurUninstallStepChanged.
Filename: "{app}\shieldcord_uninstaller.exe"; Parameters: "--keep-data"; \
    Flags: runhidden waituntilterminated; RunOnceId: "ScUninstallKeep"; Check: KeepDataWanted
Filename: "{app}\shieldcord_uninstaller.exe"; \
    Flags: runhidden waituntilterminated; RunOnceId: "ScUninstall";     Check: NotKeepDataWanted

[Code]
var
  GDotNetMissing: Boolean;
  GKeepData: Boolean;

{ ── .NET detection ─────────────────────────────────────────
  The engine is native C++ and needs no runtime; only the tray app does. So a
  missing runtime downgrades the install, it never blocks it — refusing to
  protect someone's tokens because a UI dependency is absent would be the wrong
  trade every time. }
function HasDotNet10Desktop(): Boolean;
var
  Version: String;
  FindRec: TFindRec;
begin
  { Documented detection key for the shared framework. }
  Result := RegQueryStringValue(HKLM,
    'SOFTWARE\dotnet\Setup\InstalledVersions\x64\sharedfx\Microsoft.WindowsDesktop.App',
    'Version', Version) and (Pos('10.', Version) = 1);

  if Result then exit;

  { Fall back to the on-disk layout, in case the registry value is absent. }
  Result := FindFirst(ExpandConstant('{commonpf}\dotnet\shared\Microsoft.WindowsDesktop.App\10.*'), FindRec);
  if Result then FindClose(FindRec);
end;

#ifdef BundlesDotNet
{ The runtime ships inside this Setup.exe and the [Run] step below installs it, so
  a missing runtime is not the user's problem to solve — there is nothing to ask.
  The old prompt sent people to a download page for something setup was about to
  install itself.

  Two whole function definitions rather than one with an #ifdef in the body: the
  unelevated branch is the only one that uses ErrCode, and a body-level #ifdef
  left it declared-but-unused, which is a compile hint that would sit in the build
  output hiding real warnings. }
function InitializeSetup(): Boolean;
begin
  Result := True;
  GDotNetMissing := not HasDotNet10Desktop;
end;
#else
function InitializeSetup(): Boolean;
var
  ErrCode: Integer;
begin
  Result := True;
  GDotNetMissing := not HasDotNet10Desktop;

  if GDotNetMissing then
  begin
    if MsgBox('ShieldCord''s tray app needs the .NET 10 Desktop Runtime, which is not installed on this PC.'
              + #13#10#13#10
              + 'Protection itself does not need it — the service will still be installed and the driver can still be set up. Only the tray app will not start.'
              + #13#10#13#10
              + 'Open the .NET download page?',
              mbConfirmation, MB_YESNO) = IDYES then
    begin
      ShellExec('open', 'https://dotnet.microsoft.com/download/dotnet/10.0', '', '',
                SW_SHOWNORMAL, ewNoWait, ErrCode);
    end;
  end;
end;
#endif

function NeedsDotNet(): Boolean;
begin
  Result := GDotNetMissing;
end;

function CanRunTrayApp(): Boolean;
begin
  { Re-queried rather than reusing GDotNetMissing, which was read BEFORE setup ran.
    On a bare machine the bundled-runtime step above has just installed .NET, so
    the cached answer would hide the "Open ShieldCord to finish setup" entry on
    exactly the machine that most needs it — leaving a protected-nothing PC and
    no obvious way to finish. }
  Result := (not GDotNetMissing) or HasDotNet10Desktop();
end;

{ ── post-install verification ──────────────────────────────
  The engine installer runs hidden, so a failure there would otherwise be
  invisible. Check the things that actually matter and say so plainly.

  Two separate checks, and the difference between them is the whole point:

    * No SERVICE  — the engine is not installed. That is a failed install.
    * Service but no DRIVER — the NORMAL outcome now. Protection is off until
      the app's setup runs, and the user has to be told, because a green
      "Setup completed" page over an unprotected machine is precisely the lie
      this check exists to prevent. }
function ServiceInstalled(): Boolean;
var
  Dummy: Cardinal;
begin
  Result := RegQueryDWordValue(HKLM,
    'SYSTEM\CurrentControlSet\Services\{#ServiceName}', 'Start', Dummy);
end;

function DriverInstalled(): Boolean;
var
  Dummy: Cardinal;
begin
  Result := RegQueryDWordValue(HKLM,
    'SYSTEM\CurrentControlSet\Services\{#DriverServiceName}', 'Start', Dummy);
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssDone then
  begin
    if not ServiceInstalled() then
    begin
      MsgBox('The ShieldCord service was not registered, so protection is not active.'
             + #13#10#13#10
             + 'Run C:\Program Files\ShieldCord\shieldcord_installer.exe as Administrator to see the detailed error.',
             mbError, MB_OK);
      exit;
    end;

    { Expected on a fresh install. Say it once, plainly, and point at the app —
       never let the wizard's finished page imply the machine is protected. }
    if (not DriverInstalled()) and (not WizardSilent) then
      MsgBox('ShieldCord is installed, but protection is not switched on yet.'
             + #13#10#13#10
             + 'Open ShieldCord and choose "Set up protection" on the dashboard. It installs '
             + 'the kernel driver, which needs Administrator approval and one restart.'
             + #13#10#13#10
             + 'Until you do that, your Discord and browser tokens are NOT protected.',
             mbInformation, MB_OK);
  end;
end;

{ ── uninstall data prompt ──────────────────────────────────
  Logs and settings are small but they are the user's evidence of what was
  blocked. Deleting them silently on uninstall is a surprise; asking is not. }
function KeepDataWanted(): Boolean;
begin
  Result := GKeepData;
end;

function NotKeepDataWanted(): Boolean;
begin
  Result := not GKeepData;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
  begin
    GKeepData :=
      MsgBox('Keep your ShieldCord logs and settings?'
             + #13#10#13#10
             + 'Yes — keep them in C:\ProgramData\ShieldCord'
             + #13#10
             + 'No — remove them too',
             mbConfirmation, MB_YESNO) = IDYES;
  end;
end;
