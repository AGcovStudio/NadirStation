; NADIR Station - Windows 10/11 x64 installer (Inno Setup 6.3 or later).
; Built by make_installer.bat after build_windows.bat; the files come from dist\stage.
#define AppName "NADIR Station"
#define AppVersion "2.1.2"
#define AppExe "NADIR_Station.exe"

[Setup]
AppId={{6B0E6C1E-7A55-4C8E-9C1B-2D3A9E5F4B21}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=AGcov (BI2QYN)
AppPublisherURL=https://github.com/AGcovStudio/NadirStation
AppSupportURL=https://github.com/AGcovStudio/NadirStation/issues
AppUpdatesURL=https://github.com/AGcovStudio/NadirStation/releases
VersionInfoVersion={#AppVersion}.0
; The program keeps its decoded data and log next to itself, so it is installed per user by
; default (%LOCALAPPDATA%\Programs), where it can write without administrator rights.
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
MinVersion=10.0
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
LicenseFile=..\LICENSE
SetupIconFile=..\resources\icon\nadirstation.ico
UninstallDisplayIcon={app}\{#AppExe}
OutputDir=..\dist
OutputBaseFilename=NADIR_Station_{#AppVersion}_win10_x64_setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
Source: "..\dist\stage\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\{#AppExe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#AppExe}"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: nowait postinstall skipifsilent
