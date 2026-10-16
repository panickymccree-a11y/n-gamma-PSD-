#define AppName "MCA Studio"
#define AppVersion "0.1.0"
#define PayloadDir "..\dist\MCAStudio\bin"
#define VCRedistFile "..\build-d-local\vc_redist.x64.exe"

[Setup]
AppId={{8C3E76B2-5B8B-4C33-A4C2-6A7C7B7A6E01}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher=MCA Studio
DefaultDirName={autopf}\MCAStudio
DefaultGroupName=MCA Studio
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
Uninstallable=yes
WizardStyle=modern
Compression=lzma2/max
SolidCompression=yes
OutputDir=..\..\MCAStudio_Installer
OutputBaseFilename=MCAStudio-Setup
DisableProgramGroupPage=yes
SetupLogging=yes

[Files]
; Only the production executable and its Qt runtime are included.
; Tests, simulation binaries, source files and build intermediates are excluded.
Source: "{#PayloadDir}\*"; DestDir: "{app}\bin"; Flags: recursesubdirs createallsubdirs ignoreversion
Source: "{#VCRedistFile}"; DestDir: "{tmp}"; Flags: deleteafterinstall

[Icons]
Name: "{autoprograms}\MCA Studio"; Filename: "{app}\bin\mca_studio.exe"; WorkingDir: "{app}\bin"
Name: "{autodesktop}\MCA Studio"; Filename: "{app}\bin\mca_studio.exe"; WorkingDir: "{app}\bin"

[Run]
Filename: "{tmp}\vc_redist.x64.exe"; Parameters: "/install /passive /norestart"; StatusMsg: "正在安装 Microsoft Visual C++ 运行库..."; Flags: waituntilterminated
Filename: "{app}\bin\mca_studio.exe"; Description: "启动 MCA Studio"; Flags: nowait postinstall skipifsilent
