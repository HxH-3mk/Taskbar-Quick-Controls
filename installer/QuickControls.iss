; Inno Setup script for Taskbar Quick Controls (https://jrsoftware.org/isinfo.php).
; Per-user install by default: no administrator rights needed, installs to %LOCALAPPDATA%\Programs.
; Setup also offers "Install for all users" (Program Files, needs administrator rights).

#ifndef AppVersion
  #define AppVersion "1.1.0"
#endif
#define Root ".."
#ifndef ExePath
  #define ExePath "..\QuickControls.exe"
#endif

[Setup]
AppId={{B8E4F2D1-6C35-4EAB-9A42-0F3D5B2C8E02}
AppName=Taskbar Quick Controls
AppVersion={#AppVersion}
AppVerName=Taskbar Quick Controls {#AppVersion}
AppPublisher=HxH-3mk
AppPublisherURL=https://github.com/HxH-3mk/Taskbar-Quick-Controls
DefaultDirName={autopf}\Taskbar Quick Controls
DisableProgramGroupPage=yes
DisableDirPage=auto
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
OutputDir={#Root}\dist
OutputBaseFilename=QuickControls-Setup-{#AppVersion}
SetupIconFile={#Root}\res\app.ico
UninstallDisplayIcon={app}\QuickControls.exe
UninstallDisplayName=Taskbar Quick Controls
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
CloseApplications=no

[Tasks]
Name: "autostart"; Description: "Start Taskbar Quick Controls when I sign in to Windows"

[Files]
Source: "{#ExePath}"; DestDir: "{app}"; DestName: "QuickControls.exe"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\Taskbar Quick Controls"; Filename: "{app}\QuickControls.exe"

[Run]
Filename: "{app}\QuickControls.exe"; Description: "Start Taskbar Quick Controls now"; Flags: nowait postinstall skipifsilent
Filename: "https://buymeacoffee.com/hxh3mk"; Description: "Enjoying Taskbar Quick Controls? Support on Buy Me a Coffee"; Flags: shellexec postinstall unchecked skipifsilent nowait runasoriginaluser

[UninstallRun]
Filename: "{app}\QuickControls.exe"; Parameters: "--quit"; Flags: runhidden waituntilterminated; RunOnceId: "QuitApp"

[Code]
const
  RunKey = 'Software\Microsoft\Windows\CurrentVersion\Run';

procedure QuitRunningApp();
var
  Code: Integer;
begin
  if FileExists(ExpandConstant('{app}\QuickControls.exe')) then
    Exec(ExpandConstant('{app}\QuickControls.exe'), '--quit', '', SW_HIDE, ewWaitUntilTerminated, Code);
  Exec('taskkill.exe', '/F /IM QuickControls.exe', '', SW_HIDE, ewWaitUntilTerminated, Code);
  Sleep(300);
end;

function InstalledRunValue(): String;
begin
  Result := '"' + ExpandConstant('{app}\QuickControls.exe') + '"';
end;

procedure RemoveOwnAutostart();
var
  Value: String;
begin
  if RegQueryStringValue(HKCU, RunKey, 'TaskbarQuickControls', Value) and (CompareText(Value, InstalledRunValue()) = 0) then
    RegDeleteValue(HKCU, RunKey, 'TaskbarQuickControls');
end;

function CanWriteTo(Dir: String): Boolean;
var
  Parent, Probe: String;
begin
  Dir := RemoveBackslashUnlessRoot(Dir);
  if DirExists(Dir) then
  begin
    Probe := AddBackslash(Dir) + '~quickcontrols-write-test.tmp';
    Result := SaveStringToFile(Probe, '', False);
    DeleteFile(Probe);
    exit;
  end;
  Parent := Dir;
  while not DirExists(Parent) do
  begin
    if ExtractFileDir(Parent) = Parent then
    begin
      Result := False;
      exit;
    end;
    Parent := ExtractFileDir(Parent);
  end;
  Probe := AddBackslash(Parent) + '~quickcontrols-write-test';
  Result := CreateDir(Probe);
  if Result then
    RemoveDir(Probe);
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if (CurPageID = wpSelectDir) and not WizardSilent() and not CanWriteTo(WizardDirValue()) then
  begin
    if IsAdminInstallMode() then
      MsgBox('Setup cannot create or write to this folder:' + #13#10 + WizardDirValue() + #13#10#13#10 +
        'Please choose a different folder.', mbError, MB_OK)
    else
      MsgBox('Your account cannot create or write to this folder without administrator rights:' + #13#10 +
        WizardDirValue() + #13#10#13#10 +
        'Choose a folder you own (the default one always works), or close Setup, run it again and ' +
        'pick "Install for all users".', mbError, MB_OK);
    Result := False;
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssInstall then
    QuitRunningApp();
  if CurStep = ssPostInstall then
  begin
    if WizardIsTaskSelected('autostart') then
      RegWriteStringValue(HKCU, RunKey, 'TaskbarQuickControls', InstalledRunValue())
    else
      RemoveOwnAutostart();
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
    QuitRunningApp();
  if CurUninstallStep = usPostUninstall then
  begin
    RemoveOwnAutostart();
    RegDeleteKeyIncludingSubkeys(HKCU, 'Software\TaskbarQuickControls');
  end;
end;
