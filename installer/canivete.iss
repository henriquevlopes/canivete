; Canivete installer (Inno Setup 6). Build with tools\make_setup.ps1 (passes /DAppVersion=x.y.z).
; Per-user, no admin: installs to %LOCALAPPDATA%\Programs\Canivete (same place as tools\deploy.ps1),
; so a silent self-update (/VERYSILENT /SUPPRESSMSGBOXES /NORESTART) never needs elevation.
;
; Running copy: it holds the session-local mutex Local\Canivete.Running, which AppMutex cannot
; reliably watch, so [Code] kills Canivete.exe (taskkill) before files are replaced / removed.
;
; TEST SWITCH: pass /TESTRUN to setup or to unins000.exe. It skips every step that touches the
; real, running app: no taskkill, no post-install launch, no "--uninstall" call. Example:
;   Canivete-Setup-0.1.0.exe /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /TESTRUN /NOICONS /DIR="%TEMP%\CaniveteSetupTest"
;   "%TEMP%\CaniveteSetupTest\unins000.exe" /VERYSILENT /TESTRUN

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#define AppExe "Canivete.exe"

[Setup]
AppId={{B7E2C4A1-5D3F-4E8A-9C61-2F0A7D4B1E93}
AppName=Canivete
AppVersion={#AppVersion}
AppPublisher=9000
DefaultDirName={localappdata}\Programs\Canivete
DisableDirPage=yes
DisableProgramGroupPage=yes
DefaultGroupName=Canivete
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
WizardStyle=modern
Compression=lzma2
SolidCompression=yes
SetupIconFile=..\res\canivete.ico
UninstallDisplayIcon={app}\{#AppExe}
UninstallDisplayName=Canivete
OutputDir=..\dist
OutputBaseFilename=Canivete-Setup-{#AppVersion}
CloseApplications=force
RestartApplications=no

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "..\build\Release\{#AppExe}"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\Canivete"; Filename: "{app}\{#AppExe}"
Name: "{autodesktop}\Canivete"; Filename: "{app}\{#AppExe}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#AppExe}"; Description: "Start Canivete"; Flags: nowait postinstall; Check: not IsTestRun

[UninstallRun]
Filename: "{app}\{#AppExe}"; Parameters: "--uninstall"; Flags: runhidden waituntilterminated skipifdoesntexist; RunOnceId: "CaniveteOwnCleanup"; Check: not IsTestRun

[Code]
function IsTestRun: Boolean;
var
  I: Integer;
begin
  Result := False;
  for I := 1 to ParamCount do
    if CompareText(ParamStr(I), '/TESTRUN') = 0 then
      Result := True;
end;

procedure KillCanivete;
var
  RC: Integer;
begin
  if IsTestRun then Exit;
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/im {#AppExe} /f', '', SW_HIDE, ewWaitUntilTerminated, RC);
  Sleep(500);
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  KillCanivete;
  Result := '';
end;

function InitializeUninstall: Boolean;
begin
  KillCanivete;
  Result := True;
end;
