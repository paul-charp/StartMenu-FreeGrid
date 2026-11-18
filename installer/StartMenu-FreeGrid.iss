; Build: ISCC /DAppVersion=x.y.z StartMenu-FreeGrid.iss (build.ps1 -Installer)
#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif

[Setup]
AppId={{7D2C4B8E-5A31-4F6A-9C0D-3E81B6A4F2D7}
AppName=StartMenu FreeGrid
AppVersion={#AppVersion}
AppPublisher=paul-charp
AppPublisherURL=https://github.com/paul-charp/StartMenu-FreeGrid
AppSupportURL=https://github.com/paul-charp/StartMenu-FreeGrid/issues
DefaultDirName={localappdata}\Programs\StartMenu-FreeGrid
DisableProgramGroupPage=yes
DisableDirPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.22000
SetupIconFile=..\res\StartGrid.ico
UninstallDisplayIcon={app}\StartGrid.exe
OutputDir=..\build
OutputBaseFilename=StartMenu-FreeGrid-Setup-{#AppVersion}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
CloseApplications=yes
CloseApplicationsFilter=StartGrid.exe
RestartApplications=no

[Files]
Source: "..\build\StartGrid.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\StartGrid.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{userprograms}\StartMenu FreeGrid"; Filename: "{app}\StartGrid.exe"

[Run]
Filename: "{app}\StartGrid.exe"; Parameters: "enable"; Flags: runhidden waituntilterminated
Filename: "{app}\StartGrid.exe"; Description: "Open StartMenu FreeGrid"; Flags: nowait postinstall skipifsilent

[UninstallRun]
Filename: "{app}\StartGrid.exe"; Parameters: "disable"; Flags: runhidden waituntilterminated; RunOnceId: "Disable"

[Code]
procedure CleanBin(const Dir: String);
var
  Rec: TFindRec;
begin
  if FindFirst(Dir + '\bin\*', Rec) then
  try
    repeat
      if (Rec.Name <> '.') and (Rec.Name <> '..') then
        DelTree(Dir + '\bin\' + Rec.Name, True, True, True);  // locked ones stay
    until not FindNext(Rec);
  finally
    FindClose(Rec);
  end;
  RemoveDir(Dir + '\bin');
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Dir: String;
begin
  if CurUninstallStep <> usPostUninstall then
    Exit;
  Dir := ExpandConstant('{localappdata}\StartGrid');
  if not DirExists(Dir) then
    Exit;
  if (not UninstallSilent) and
     (MsgBox('Keep saved layout?', mbConfirmation, MB_YESNO) = IDNO) then
    DelTree(Dir, True, True, True)
  else
  begin
    DeleteFile(Dir + '\log.txt');
    CleanBin(Dir);
  end;
end;
