; CTM Bridge — Windows service installer (Inno Setup 6).
;
; Packages ctm-usbip.exe + the ffmpeg runtime DLLs + profiles/maps into
; C:\Program Files\CTM Bridge, then registers the auto-start LocalSystem
; service and the LAN firewall rules by invoking `ctm-usbip.exe install`.
; Uninstall calls `ctm-usbip.exe uninstall` (stop + remove service + rules)
; before the files are deleted. Service logs live in %ProgramData%\CTM Bridge.
;
; Build steps:
;   1. Build Release:   .\build.ps1 -Configuration Release
;   2. Compile this:    ISCC.exe installer\ctm-usbip.iss
;      (Inno Setup 6 — winget install JRSoftware.InnoSetup)
;   Output: out\installer\CTM-Bridge-Setup.exe
;
; The usbip-win2 driver (the WHLK/Microsoft-signed vhci) is bundled and installed
; automatically when it is not already present. usbip-win2 is BSD-2-Clause
; (© Vadym Hrynchyshyn); its signed installer is shipped unmodified and its
; LICENSE.txt is installed alongside the app. Installed without its pdb/sdk/gui
; components (debug symbols and tools the agent does not use).

#define AppName "CTM Bridge"
#define AppVersion "0.0.2"
#define AppPublisher "CTM"
#define ExeName "ctm-usbip.exe"
#define UsbipInstaller "USBip-0.9.7.7-x64.exe"
#define SrcRoot ".."

[Setup]
AppId={{A7E5C2F4-3D9B-4C81-B6E2-9F1A4D7C8E50}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#AppPublisher}
VersionInfoVersion=0.0.1.1
VersionInfoProductVersion=0.0.1
DefaultDirName={commonpf64}\CTM Bridge
DisableProgramGroupPage=yes
DisableDirPage=auto
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
OutputDir={#SrcRoot}\out\installer
OutputBaseFilename=CTM-Bridge-Setup
Compression=lzma2
SolidCompression=yes
UninstallDisplayName={#AppName}
UninstallDisplayIcon={app}\{#ExeName}
SetupIconFile={#SrcRoot}\app\ctm-usbip.ico
WizardStyle=modern

[Files]
Source: "{#SrcRoot}\out\x64\Release\{#ExeName}"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SrcRoot}\third_party\ffmpeg\x64\release\bin\*.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SrcRoot}\profiles\descriptors\*"; DestDir: "{app}\profiles\descriptors"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#SrcRoot}\maps\*"; DestDir: "{app}\maps"; Flags: ignoreversion recursesubdirs createallsubdirs
; Bundled usbip-win2 (BSD-2-Clause): the signed installer runs from {tmp} only
; when usbip-win2 is not already present; its license ships in the app folder.
Source: "{#SrcRoot}\third_party\usbip-win2\{#UsbipInstaller}"; DestDir: "{tmp}"; Flags: deleteafterinstall
Source: "{#SrcRoot}\third_party\usbip-win2\LICENSE.txt"; DestDir: "{app}"; DestName: "usbip-win2-LICENSE.txt"; Flags: ignoreversion
; Ciprian's Bridge — GUI for plugging PC-Bluetooth controllers through the CTM
; pipeline (self-contained .NET publish; run apps\CipriansBridge dotnet publish
; before compiling this script — build-installer.ps1 does it).
Source: "{#SrcRoot}\apps\CipriansBridge\out\publish\CipriansBridge.exe"; DestDir: "{app}"; DestName: "CipriansBridge.exe"; Flags: ignoreversion

[Dirs]
Name: "{commonappdata}\CTM Bridge"

[Icons]
Name: "{commonprograms}\Ciprian's Bridge"; Filename: "{app}\CipriansBridge.exe"; WorkingDir: "{app}"

[Run]
; Components main,client only: no pdb/sdk (debug symbols) and no GUI tool — the
; agent drives usbip.exe directly. Their VC++ redist task still runs by default.
; Their setup is AlwaysRestart=yes; /NORESTART suppresses that and our
; NeedRestart() prompts for the reboot instead when we installed the driver.
Filename: "{tmp}\{#UsbipInstaller}"; Parameters: "/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /COMPONENTS=main,client"; StatusMsg: "Installing the usbip-win2 driver..."; Flags: waituntilterminated; Check: NeedsUsbip
Filename: "{app}\{#ExeName}"; Parameters: "install"; StatusMsg: "Registering the CTM Bridge service..."; Flags: runhidden waituntilterminated

[UninstallRun]
Filename: "{app}\{#ExeName}"; Parameters: "uninstall"; Flags: runhidden waituntilterminated; RunOnceId: "RemoveCtmBridgeService"

[UninstallDelete]
Type: filesandordirs; Name: "{commonappdata}\CTM Bridge"

[Code]
var
  GInstallUsbip: Boolean;

function GetUninstallString(): String;
var
  Key, Value: String;
begin
  Key := 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{A7E5C2F4-3D9B-4C81-B6E2-9F1A4D7C8E50}_is1';
  Value := '';
  if not RegQueryStringValue(HKLM, Key, 'UninstallString', Value) then
    RegQueryStringValue(HKCU, Key, 'UninstallString', Value);
  Result := Value;
end;

procedure RemovePreviousVersion();
var
  UninstallString: String;
  ResultCode, I: Integer;
begin
  UninstallString := GetUninstallString();
  if UninstallString = '' then
    Exit;
  UninstallString := RemoveQuotes(UninstallString);
  { Uninstall the previous version first: stops + deregisters the service,
    removes the firewall rule, and deletes the files it installed. Custom
    .map/.profile files the user added are not tracked by that uninstaller,
    so they are left in place. }
  Exec(UninstallString, '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  { The uninstaller relaunches from a temp copy and the call above returns
    early; wait (up to ~30s) until the old executable is gone before we copy
    the new files over it. }
  for I := 1 to 60 do
  begin
    if not FileExists(ExpandConstant('{app}\ctm-usbip.exe')) then
      Break;
    Sleep(500);
  end;
end;

function DesktopRuntimeInstalled(): Boolean;
var
  FindRec: TFindRec;
begin
  { Ciprian's Bridge is a framework-dependent .NET 10 WPF app: any 10.x
    Windows Desktop Runtime satisfies it. }
  Result := False;
  if FindFirst(ExpandConstant('{commonpf64}\dotnet\shared\Microsoft.WindowsDesktop.App\10.*'), FindRec) then
  begin
    Result := True;
    FindClose(FindRec);
  end;
end;

procedure EnsureDesktopRuntime();
var
  ResultCode: Integer;
  RuntimeExe: String;
begin
  if DesktopRuntimeInstalled() then
    Exit;
  try
    { Official "latest 10.0 x64 desktop runtime" alias; downloaded only when
      the runtime is genuinely absent. }
    DownloadTemporaryFile(
      'https://aka.ms/dotnet/10.0/windowsdesktop-runtime-win-x64.exe',
      'windowsdesktop-runtime.exe', '', nil);
    RuntimeExe := ExpandConstant('{tmp}\windowsdesktop-runtime.exe');
    Exec(RuntimeExe, '/install /quiet /norestart', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  except
    { Offline / download blocked: continue the install — the service and CLI
      work without .NET; only Ciprian's Bridge needs it (fails with a clear
      OS message pointing at the runtime). }
  end;
end;

procedure StopPreviousService();
var
  ResultCode: Integer;
  ExePath: String;
begin
  { Fast upgrade path: just stop + deregister the service so its binary is
    unlocked; the file copy overwrites everything in place (same AppId keeps
    the uninstall entry current). Running the FULL previous uninstaller here
    (service stop + file deletion + a 30 s poll) was the pre-progress hang. }
  ExePath := ExpandConstant('{app}\ctm-usbip.exe');
  if FileExists(ExePath) then
    Exec(ExePath, 'uninstall', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  StopPreviousService();
  EnsureDesktopRuntime();
  { Decide once, before the [Run] steps, whether we install the bundled
    usbip-win2 — so the flag stays valid for the post-install .pdb cleanup
    even after usbip.exe has been created. }
  GInstallUsbip := not FileExists(ExpandConstant('{commonpf64}\USBip\usbip.exe'));
  Result := '';
end;

function NeedsUsbip(): Boolean;
begin
  { True only when usbip-win2 was absent, so we never downgrade or clobber a
    user-managed install. }
  Result := GInstallUsbip;
end;

function NeedRestart(): Boolean;
begin
  { usbip-win2's own setup is AlwaysRestart=yes (driver install); we run it
    /NORESTART, so surface the reboot prompt from our installer when we were
    the ones who installed the driver. }
  Result := GInstallUsbip;
end;
