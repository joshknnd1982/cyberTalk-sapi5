; Inno Setup 6 script for the CyberTalk SAPI5 wrapper.
; Built by build_all.bat from the staged files in ..\output.

#define MyAppName "CyberTalk SAPI5"
#define MyAppVersion "1.0.0"
#define MyAppPublisher "joshknnd1982"
#define MyAppURL "https://github.com/joshknnd1982/cybertalk-sapi5"
#define StageDir "..\output"

[Setup]
AppId={{B7E4A5C2-3D8F-4B61-9E2A-6C1F0D7A9B34}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
DefaultDirName={autopf}\CyberTalk SAPI5
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
UninstallDisplayIcon={app}\CyberTalkConfig.exe
UninstallDisplayName={#MyAppName}
OutputDir=.
OutputBaseFilename=CyberTalkSAPI_Setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
SetupLogging=yes
UsedUserAreasWarning=no
CloseApplications=no
LicenseFile={#StageDir}\LICENSE.txt
InfoBeforeFile={#StageDir}\README.txt

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop icon for the CyberTalk configuration utility"; GroupDescription: "Additional icons:"

[Files]
Source: "{#StageDir}\CyberTalkHost.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\CyberTalkConfig.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\CyberTalkSAPI.dll"; DestDir: "{app}"; Flags: ignoreversion regserver 32bit
Source: "{#StageDir}\x64\CyberTalkSAPI.dll"; DestDir: "{app}\x64"; Flags: ignoreversion regserver 64bit; Check: Is64BitInstallMode
Source: "{#StageDir}\engine\*"; DestDir: "{app}\engine"; Flags: ignoreversion
Source: "{#StageDir}\tools\*"; DestDir: "{app}\tools"; Flags: ignoreversion
Source: "{#StageDir}\README.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\LICENSE.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\CREDITS.txt"; DestDir: "{app}"; Flags: ignoreversion

[Registry]
Root: HKLM32; Subkey: "SOFTWARE\CyberTalkSAPI"; ValueType: string; ValueName: "InstallDir"; ValueData: "{app}"; Flags: uninsdeletekey
Root: HKLM32; Subkey: "SOFTWARE\CyberTalkSAPI"; ValueType: string; ValueName: "Version"; ValueData: "{#MyAppVersion}"

[Icons]
Name: "{group}\CyberTalk Configuration"; Filename: "{app}\CyberTalkConfig.exe"; Comment: "Adjust the CyberTalk Custom Voice"
Name: "{group}\CyberTalk log folder"; Filename: "{localappdata}\CyberTalkSAPI\logs"
Name: "{group}\Uninstall {#MyAppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\CyberTalk Configuration"; Filename: "{app}\CyberTalkConfig.exe"; Tasks: desktopicon; Comment: "Adjust the CyberTalk Custom Voice"

[Run]
Filename: "{app}\CyberTalkConfig.exe"; Description: "Open the CyberTalk configuration utility now"; Flags: nowait postinstall skipifsilent unchecked

[Code]
procedure KillProcess(const Name: String);
var
  ResultCode: Integer;
begin
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM ' + Name, '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
end;

procedure StopHost();
begin
  { the host keeps the engine and the DLL registration busy }
  KillProcess('CyberTalkHost.exe');
  KillProcess('STLTTS.EXE');
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  StopHost();
  Result := '';
end;

procedure CopySetupLog();
var
  LogDir: String;
begin
  { one copy next to the program (always reachable), one with the runtime logs }
  LogDir := ExpandConstant('{app}\logs');
  ForceDirectories(LogDir);
  Log('Copying setup log to ' + LogDir + '\install.log');
  CopyFile(ExpandConstant('{log}'), LogDir + '\install.log', False);
  LogDir := ExpandConstant('{localappdata}\CyberTalkSAPI\logs');
  ForceDirectories(LogDir);
  CopyFile(ExpandConstant('{log}'), LogDir + '\install.log', False);
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
    CopySetupLog();
  if CurStep = ssDone then
    CopySetupLog();
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
    StopHost();
end;
