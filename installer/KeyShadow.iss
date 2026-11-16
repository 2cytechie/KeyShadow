; 键影 KeyShadow 安装包脚本（Inno Setup 6）
; 编译方式：ISCC.exe installer\KeyShadow.iss

#define MyAppName "键影"
#define MyAppVersion "26.11.16"
#define MyAppExeName "KeyShadow.exe"
#define MyDistDir "..\dist\KeyShadow-" + MyAppVersion

[Setup]
AppId={{1C50B74A-BA7D-490C-970D-C6A1EE61D4FC}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher=KeyShadow
DefaultDirName={autopf}\KeyShadow
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
UninstallDisplayIcon={app}\{#MyAppExeName}
UninstallDisplayName={#MyAppName} {#MyAppVersion}
SetupIconFile=..\resources\icons\app-icon.ico
OutputDir=..\dist
OutputBaseFilename=KeyShadow-{#MyAppVersion}-Setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
VersionInfoCompany=KeyShadow
VersionInfoDescription={#MyAppName} 安装程序
VersionInfoProductName={#MyAppName}
VersionInfoProductVersion={#MyAppVersion}
VersionInfoVersion=26.11.16.0

[Languages]
Name: "chinesesimplified"; MessagesFile: "ChineseSimplified.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
Source: "{#MyDistDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#MyAppName}}"; Flags: nowait postinstall skipifsilent
