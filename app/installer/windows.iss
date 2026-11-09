; The Windows installer (Inno Setup 6.3+), made by release.yml from the folder
; `cmake --install` lays out:
;
;   iscc /DPackageVersion=1.2.3 /DPackageDir=<that folder> /O<output> windows.iss
;
; For one user, in %LOCALAPPDATA%\Programs: the app updates itself by replacing
; the files in its own folder (app/update/install.h), which it could not do in
; Program Files. For the same reason the folder is not chosen. What the app
; knows of the person stays in %LOCALAPPDATA%\ratcast, installed or uninstalled.

#ifndef PackageDir
  #error Pass /DPackageDir=<the folder cmake --install made>
#endif
#ifndef PackageVersion
  #define PackageVersion "0.0.0"
#endif

[Setup]
; Never to change: it is how a new installer finds the installed app.
AppId={{0F6DA19A-E2A7-4BD0-A4D9-4DC2809FDFA9}
AppName=ratcast
AppVersion={#PackageVersion}
AppVerName=ratcast {#PackageVersion}
AppPublisher=ratcast
AppPublisherURL=https://ratcast.app
AppSupportURL=https://github.com/librats/ratcast/issues
AppUpdatesURL=https://ratcast.app
DefaultDirName={autopf}\ratcast
DisableDirPage=yes
DefaultGroupName=ratcast
DisableProgramGroupPage=yes
DisableReadyPage=yes
PrivilegesRequired=lowest
; Windows.Graphics.Capture, which sharing the screen needs.
MinVersion=10.0.18362
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Held by the app as long as it runs (SingleInstance::listen): asked to be
; closed first, by this and by the uninstaller.
AppMutex=ratcast-app
CloseApplications=no
OutputBaseFilename=ratcast-{#PackageVersion}-windows-x64-setup
SetupIconFile=..\resources\ratcast.ico
UninstallDisplayIcon={app}\ratcast.exe
UninstallDisplayName=ratcast
WizardStyle=modern
Compression=lzma2/max
SolidCompression=yes

[Languages]
; The app's languages that Inno Setup itself carries a translation of.
Name: "en"; MessagesFile: "compiler:Default.isl"
Name: "de"; MessagesFile: "compiler:Languages\German.isl"
Name: "es"; MessagesFile: "compiler:Languages\Spanish.isl"
Name: "fr"; MessagesFile: "compiler:Languages\French.isl"
Name: "ja"; MessagesFile: "compiler:Languages\Japanese.isl"
Name: "ko"; MessagesFile: "compiler:Languages\Korean.isl"
Name: "pt"; MessagesFile: "compiler:Languages\BrazilianPortuguese.isl"
Name: "ru"; MessagesFile: "compiler:Languages\Russian.isl"
Name: "tr"; MessagesFile: "compiler:Languages\Turkish.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[InstallDelete]
; An update staged by the version replaced: it would go in over this one at
; the next start.
Type: filesandordirs; Name: "{app}\.update"

[Files]
Source: "{#PackageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\ratcast"; Filename: "{app}\ratcast.exe"
Name: "{autodesktop}\ratcast"; Filename: "{app}\ratcast.exe"; Tasks: desktopicon

[Registry]
; rats:// links, working before the first start; written as
; register_link_scheme() (app/src/links.cpp) writes them, or it writes them again.
Root: HKCU; Subkey: "Software\Classes\rats"; ValueType: string; ValueData: "URL:rats"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\rats"; ValueType: string; ValueName: "URL Protocol"; ValueData: ""
Root: HKCU; Subkey: "Software\Classes\rats\DefaultIcon"; ValueType: string; ValueData: """{app}\ratcast.exe"",0"
Root: HKCU; Subkey: "Software\Classes\rats\shell\open\command"; ValueType: string; ValueData: """{app}\ratcast.exe"" ""%1"""

[Run]
Filename: "{app}\ratcast.exe"; Description: "{cm:LaunchProgram,ratcast}"; Flags: nowait postinstall skipifsilent

[Code]
// The updater leaves files this installer never saw — new libraries, what it
// renamed aside — so the whole folder goes, but only one that is the app's
// by name, and never the one that holds the person's key.
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Dir: String;
begin
  if CurUninstallStep <> usPostUninstall then
    Exit;
  Dir := RemoveBackslashUnlessRoot(ExpandConstant('{app}'));
  if (CompareText(ExtractFileName(Dir), 'ratcast') = 0) and
     (CompareText(Dir, ExpandConstant('{localappdata}\ratcast')) <> 0) then
    DelTree(Dir, True, True, True);
end;
