#ifndef MyAppVersion
  #define MyAppVersion "0.1.0-alpha.1"
#endif

#define MyAppName "Curious Bipedal OBS Overlay"
#define MyAppPublisher "Curious Bipedal"
#define MyAppURL "https://github.com/alenpjose/curious-bipedal-obs-overlay"
#define PluginName "curious-bipedal-obs-overlay"

[Setup]
AppId={{53B98712-4F4E-4DDC-A903-ED6B11F353C4}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
DefaultDirName={commonappdata}\obs-studio\plugins\{#PluginName}
DisableDirPage=yes
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir=..\release\installer
OutputBaseFilename=Curious-Bipedal-OBS-Overlay-Setup-{#MyAppVersion}-windows-x64
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName={#MyAppName}
VersionInfoVersion=0.1.0.0
VersionInfoCompany={#MyAppPublisher}
VersionInfoDescription=Native OBS Studio session overlay
CloseApplications=yes
RestartApplications=no

[Files]
Source: "..\release\Release\{#PluginName}\bin\64bit\*"; DestDir: "{app}\bin\64bit"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "..\release\Release\{#PluginName}\data\*"; DestDir: "{app}\data"; Flags: ignoreversion recursesubdirs createallsubdirs

[Code]
function IsOBSRunning: Boolean;
var
  ResultCode: Integer;
begin
  Result := Exec(ExpandConstant('{cmd}'), '/C tasklist /FI "IMAGENAME eq obs64.exe" | find /I "obs64.exe" >nul', '', SW_HIDE, ewWaitUntilTerminated, ResultCode) and (ResultCode = 0);
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  if IsOBSRunning then
    Result := 'OBS Studio is still open. Close OBS Studio, then click Retry.'
  else
    Result := '';
end;
